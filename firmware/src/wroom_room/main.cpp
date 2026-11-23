#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "espnow_protocol.h"
#include "ble_protocol.h"

// ==================== CẤU HÌNH PHẦN CỨNG ====================
#define PIN_RELAY         26
#define PIN_BUTTON        27
#define PIN_LIGHT_AO      34  // ADC1 - cắm chân AO của MH-Sensor khi có hàng

// Relay kích mức LOW (tải đấu COM + NO; nếu đấu COM + NC thì đảo ngược 2 dòng này)
#define RELAY_OFF          LOW
#define RELAY_ON           HIGH

// 1 = Giả lập cảm biến ánh sáng, 0 = Đọc thật từ GPIO 34
#define MOCK_LIGHT_SENSOR 0

// ==================== CẤU HÌNH ESP-NOW ====================
// Thay bằng địa chỉ MAC thật của YoloUNO (ESP32-S3)
uint8_t S3_HUB_MAC[6] = {0x80, 0xB5, 0x4E, 0xE2, 0x01, 0x14};

const char* TARGET_WIFI_SSID    = "TEN_WIFI_NHA_BAN";
const uint8_t DEFAULT_WIFI_CHAN = 1; // Khớp FALLBACK_CHANNEL của YoloUNO

// ==================== THÔNG SỐ THỜI GIAN ====================
const uint32_t TELEMETRY_INTERVAL_MS  = 3000;
const uint32_t BLE_RESCAN_INTERVAL_MS = 4000;
const uint32_t BLE_SCAN_DURATION_MS   = 4000;

// ==================== ĐỐI TƯỢNG ĐỒNG BỘ FREERTOS ====================
SemaphoreHandle_t xStateMutex     = nullptr; // Bảo vệ trạng thái dùng chung
SemaphoreHandle_t xButtonSem      = nullptr; // Đánh thức TaskButtonRelay từ ISR
SemaphoreHandle_t xTxImmediateSem = nullptr; // Yêu cầu TaskEspNow gửi ngay lập tức
QueueHandle_t     xRxCmdQueue     = nullptr; // Hàng đợi lệnh ESP-NOW từ S3

// ==================== TRẠNG THÁI DÙNG CHUNG (BẢO VỆ BỞI xStateMutex) ====================
struct NodeState {
  bool  relayOn   = false;
  bool  awayMode  = false;
  float bleTemp   = 0.0f;
  float bleHum    = 0.0f;
  bool  bleOnline = false;
} nodeState;

// ==================== BIẾN NỘI BỘ ESP-NOW ====================
uint16_t packetSeq = 0;
uint8_t currentEspNowChannel = DEFAULT_WIFI_CHAN;

volatile bool espNowSendDone     = false;
volatile bool espNowLastOk       = false;
volatile bool isHuntingChan      = false;
volatile uint8_t espNowFailCount = 0;

// ==================== BIẾN NỘI BỘ BLE (NimBLE 2.x) ====================
NimBLEClient* pBleClient = nullptr;
NimBLEAddress targetBleAddr;
volatile bool doBleConnect = false;
volatile bool bleConnected = false;
volatile bool bleScanning  = false;
uint32_t lastBleScanAttempt = 0;

// ==================== ĐIỀU KHIỂN RELAY & ĐỌC ÁNH SÁNG ====================
// Chỉ gọi hàm này khi ĐÃ giữ xStateMutex
void applyRelayLocked(bool turnOn) {
  nodeState.relayOn = turnOn;
  digitalWrite(PIN_RELAY, turnOn ? RELAY_ON : RELAY_OFF);
  Serial.printf("[RELAY] Trang thai: %s (GPIO %d = %d)\n",
                turnOn ? "BAT (ON)" : "TAT (OFF)",
                PIN_RELAY,
                turnOn ? RELAY_ON : RELAY_OFF);
}

uint16_t readLightLevel() {
#if MOCK_LIGHT_SENSOR
  static uint16_t mockLux = 350;
  mockLux = (mockLux >= 750) ? 300 : (mockLux + 30);
  return mockLux;
#else
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(PIN_LIGHT_AO);
  }
  uint16_t raw = sum / 8;
  uint16_t inverted = 4095 - raw;
  return map(inverted, 0, 4095, 0, 1000);
#endif
}

// ==================== NGẮT PHẦN CỨNG NÚT BẤM (IRAM_ATTR) ====================
void IRAM_ATTR onButtonISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(xButtonSem, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// ==================== TASK 1: NÚT BẤM & RELAY (CORE 1 - ƯU TIÊN 3) ====================
void TaskButtonRelay(void* pvParameters) {
  for (;;) {
    // Ngủ hoàn toàn cho tới khi có cạnh xuống từ nút bấm
    if (xSemaphoreTake(xButtonSem, portMAX_DELAY) == pdTRUE) {
      // Chờ 25ms qua nhiễu dội tiếp điểm cơ khí (vẫn bắt kịp thao tác bấm nhanh)
      vTaskDelay(pdMS_TO_TICKS(25));

      if (digitalRead(PIN_BUTTON) == LOW) {
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          Serial.println("[BUTTON] Nhan nut thu cong -> Dao trang thai Relay");
          bool nextState = !nodeState.relayOn;
          if (nextState) {
            nodeState.awayMode = false; // Tự bật đèn thủ công thì hủy Away Mode
          }
          applyRelayLocked(nextState);
          xSemaphoreGive(xStateMutex);
        }

        // Báo TaskEspNow gửi ngay gói tin đồng bộ lên YoloUNO
        xSemaphoreGive(xTxImmediateSem);

        // Chờ người dùng nhả nút để không kích đúp nếu giữ lâu
        while (digitalRead(PIN_BUTTON) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(30)); // Chống dội cạnh lên khi nhả nút
      }

      // Xóa mọi tín hiệu nhiễu phát sinh thêm trong lúc đang chống dội
      xSemaphoreTake(xButtonSem, 0);
    }
  }
}

// ==================== XỬ LÝ BLE CLIENT (NimBLE 2.x) ====================
void parseBlePayload(const uint8_t* data, size_t len, const char* tag) {
  char buf[32];
  size_t copyLen = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
  memcpy(buf, data, copyLen);
  buf[copyLen] = '\0';

  float t = 0.0f, h = 0.0f;
  if (sscanf(buf, "%f,%f", &t, &h) == 2) {
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      nodeState.bleTemp   = t;
      nodeState.bleHum    = h;
      nodeState.bleOnline = true;
      xSemaphoreGive(xStateMutex);
    }
    Serial.printf("[BLE %s] Nhan tu C3: '%s' -> Temp=%.1f C | Hum=%.1f %%\n",
                  tag, buf, t, h);
  }
}

void bleNotifyCallback(NimBLERemoteCharacteristic* pChr, uint8_t* pData, size_t len, bool isNotify) {
  parseBlePayload(pData, len, isNotify ? "NOTIFY" : "INDICATE");
}

class EnvClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    Serial.printf("[BLE] >>> DA KET NOI (BIND) toi %s!\n",
                  pClient->getPeerAddress().toString().c_str());
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    bleConnected = false;
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      nodeState.bleOnline = false;
      xSemaphoreGive(xStateMutex);
    }
    lastBleScanAttempt = millis();
    Serial.printf("[BLE] Mat ket noi C3 (reason=%d) -> ble_online=0\n", reason);
  }
};

class EnvScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    bool matchName = (advertisedDevice->getName() == BLE_DEVICE_NAME);
    bool matchUuid = advertisedDevice->isAdvertisingService(NimBLEUUID(BLE_SERVICE_UUID));

    if (matchName || matchUuid) {
      Serial.printf("[BLE] Tim thay '%s' (MAC: %s | RSSI: %d dBm)\n",
                    advertisedDevice->getName().c_str(),
                    advertisedDevice->getAddress().toString().c_str(),
                    advertisedDevice->getRSSI());
      NimBLEDevice::getScan()->stop();
      bleScanning   = false;
      targetBleAddr = advertisedDevice->getAddress();
      doBleConnect  = true;
    }
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    bleScanning = false;
  }
};

static EnvClientCallbacks clientCallbacks;
static EnvScanCallbacks   scanCallbacks;

void startBleScan() {
  if (bleConnected || bleScanning || doBleConnect || isHuntingChan) return;
  NimBLEScan* pScan = NimBLEDevice::getScan();
  bleScanning = true;
  lastBleScanAttempt = millis();
  Serial.printf("[BLE] Dang quet tim '%s'...\n", BLE_DEVICE_NAME);
  pScan->start(BLE_SCAN_DURATION_MS, false, true);
}

bool connectToBleServer() {
  if (pBleClient == nullptr) {
    pBleClient = NimBLEDevice::createClient();
    pBleClient->setClientCallbacks(&clientCallbacks, false);
    pBleClient->setConnectTimeout(5 * 1000);
  }

  if (!pBleClient->connect(targetBleAddr)) {
    Serial.println("[BLE] Ket noi C3 that bai!");
    return false;
  }

  NimBLERemoteService* pSvc = pBleClient->getService(BLE_SERVICE_UUID);
  if (!pSvc) {
    pBleClient->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic* pChr = pSvc->getCharacteristic(BLE_CHAR_ENV_UUID);
  if (!pChr) {
    pBleClient->disconnect();
    return false;
  }

  if (pChr->canRead()) {
    std::string val = pChr->readValue();
    if (!val.empty()) {
      parseBlePayload((const uint8_t*)val.data(), val.size(), "READ");
    }
  }

  if (pChr->canNotify()) {
    if (!pChr->subscribe(true, bleNotifyCallback)) {
      pBleClient->disconnect();
      return false;
    }
    Serial.println("[BLE] >>> Bat Notify thanh cong!");
  }

  bleConnected = true;
  return true;
}

// ==================== TASK 2: QUẢN LÝ BLE CLIENT (CORE 0 - ƯU TIÊN 1) ====================
void TaskBleClient(void* pvParameters) {
  NimBLEDevice::init("SmartHome_WROOM");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setInterval(100);
  pScan->setWindow(45); // Nhường 55% thời gian sóng 2.4GHz cho ESP-NOW
  pScan->setActiveScan(true);

  startBleScan();

  for (;;) {
    if (doBleConnect) {
      doBleConnect = false;
      // Lệnh này có thể block tối đa 5s nhưng chạy trên Core 0 -> KHÔNG ảnh hưởng nút bấm
      if (!connectToBleServer()) {
        lastBleScanAttempt = millis();
      }
    } else if (!bleConnected && !bleScanning && !isHuntingChan &&
               (millis() - lastBleScanAttempt >= BLE_RESCAN_INTERVAL_MS)) {
      startBleScan();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ==================== XỬ LÝ ESP-NOW & TỰ SĂN KÊNH ====================
bool isBroadcastMac(const uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0xFF) return false;
  }
  return true;
}

void lockEspNowChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  currentEspNowChannel = ch;
}

EspNowPacket buildTelemetryPacket() {
  NodeState snap;
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snap = nodeState;
    xSemaphoreGive(xStateMutex);
  }

  EspNowPacket pkt = {};
  pkt.node_id     = NODE_WROOM_ROOM;
  pkt.msg_type    = MSG_TELEMETRY;
  pkt.seq         = ++packetSeq;
  pkt.temperature = snap.bleTemp;
  pkt.humidity    = snap.bleHum;
  pkt.light_lux   = readLightLevel();
  pkt.switch_mask = snap.relayOn ? SW_BED_LIGHT_BIT : 0;
  pkt.away_mode   = snap.awayMode ? 1 : 0;
  pkt.alert_code  = ALERT_NONE;
  pkt.ble_online  = snap.bleOnline ? 1 : 0;
  return pkt;
}

void sendTelemetryToHub() {
  EspNowPacket pkt = buildTelemetryPacket();
  esp_err_t res = esp_now_send(S3_HUB_MAC, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[ESP-NOW Ch=%u] seq=%u | BLE=%s (T=%.1fC, H=%.1f%%) | Light=%u | Relay=%s | Send=%s\n",
                currentEspNowChannel,
                pkt.seq,
                pkt.ble_online ? "ON" : "OFF",
                pkt.temperature,
                pkt.humidity,
                pkt.light_lux,
                (pkt.switch_mask & SW_BED_LIGHT_BIT) ? "ON" : "OFF",
                (res == ESP_OK) ? "OK" : "ERR");
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onDataSent(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
#else
void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
#endif
  espNowLastOk   = (status == ESP_NOW_SEND_SUCCESS);
  espNowSendDone = true;

  if (!isHuntingChan) {
    if (espNowLastOk) {
      espNowFailCount = 0;
    } else {
      espNowFailCount = espNowFailCount + 1;
    }
  }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#else
void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len == sizeof(EspNowPacket) && xRxCmdQueue != nullptr) {
    xQueueSend(xRxCmdQueue, data, 0);
  }
}

void huntS3Channel() {
  if (isBroadcastMac(S3_HUB_MAC)) {
    Serial.println("[CANH BAO] S3_HUB_MAC dang la broadcast FF:FF... -> Khong tu san kenh!");
    espNowFailCount = 0;
    return;
  }

  isHuntingChan = true;

  if (bleScanning) {
    NimBLEDevice::getScan()->stop();
    bleScanning = false;
  }

  Serial.println("[ESP-NOW] Mat ACK 3 lan -> Tu do kenh 1..13 tim YoloUNO...");
  EspNowPacket probe = buildTelemetryPacket();

  for (uint8_t ch = 1; ch <= 13; ch++) {
    lockEspNowChannel(ch);
    vTaskDelay(pdMS_TO_TICKS(15));

    espNowSendDone = false;
    espNowLastOk   = false;
    esp_now_send(S3_HUB_MAC, (uint8_t*)&probe, sizeof(probe));

    uint32_t t0 = millis();
    while (!espNowSendDone && (millis() - t0 < 80)) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (espNowLastOk) {
      Serial.printf("[ESP-NOW] >>> BAT LAI DUOC YOLOUNO O KENH %u!\n", ch);
      espNowFailCount = 0;
      isHuntingChan   = false;
      return;
    }
  }

  lockEspNowChannel(DEFAULT_WIFI_CHAN);
  espNowFailCount = 0;
  isHuntingChan   = false;
  Serial.printf("[ESP-NOW] Chua thay YoloUNO -> Tam dung o kenh %u.\n", DEFAULT_WIFI_CHAN);
}

uint8_t detectWiFiChannel(const char* ssid) {
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) {
      uint8_t ch = WiFi.channel(i);
      Serial.printf("[WIFI] Tim thay '%s' o kenh %u\n", ssid, ch);
      return ch;
    }
  }
  Serial.printf("[WIFI] Khong thay '%s', dung kenh mac dinh %u\n",
                ssid, DEFAULT_WIFI_CHAN);
  return DEFAULT_WIFI_CHAN;
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("[WROOM] MAC Wi-Fi cua node nay: ");
  Serial.println(WiFi.macAddress());

  uint8_t initChan = detectWiFiChannel(TARGET_WIFI_SSID);
  lockEspNowChannel(initChan);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Khoi tao that bai!");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, S3_HUB_MAC, 6);
  peerInfo.channel = 0;
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ESP-NOW] Khong the them peer YoloUNO!");
  }
}

// ==================== TASK 3: GIAO TIẾP ESP-NOW (CORE 1 - ƯU TIÊN 2) ====================
void TaskEspNow(void* pvParameters) {
  TickType_t lastTxTick = xTaskGetTickCount();
  EspNowPacket cmd;

  for (;;) {
    // 1. Tự săn kênh 1-13 nếu mất ACK 3 lần liên tiếp
    if (espNowFailCount >= 3) {
      huntS3Channel();
      lastTxTick = xTaskGetTickCount();
    }

    // 2. Chờ lệnh từ YoloUNO (tối đa 20ms để vòng lặp luôn nhẹ CPU)
    if (xQueueReceive(xRxCmdQueue, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (cmd.msg_type == MSG_COMMAND) {
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          nodeState.awayMode = (cmd.away_mode != 0);
          if (nodeState.awayMode) {
            applyRelayLocked(false);
          } else {
            bool targetState = (cmd.switch_mask & SW_BED_LIGHT_BIT) != 0;
            applyRelayLocked(targetState);
          }
          xSemaphoreGive(xStateMutex);
        }
        sendTelemetryToHub();
        lastTxTick = xTaskGetTickCount();
      }
    }

    // 3. Gửi telemetry ngay nếu nút bấm vừa đổi Relay, hoặc đúng chu kỳ 3 giây
    bool immediateTx = (xSemaphoreTake(xTxImmediateSem, 0) == pdTRUE);
    bool periodicTx  = ((xTaskGetTickCount() - lastTxTick) >= pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));

    if (immediateTx || periodicTx) {
      lastTxTick = xTaskGetTickCount();
      sendTelemetryToHub();
    }
  }
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Ghi sẵn mức TẮT trước khi set OUTPUT tránh relay kêu lúc cấp nguồn
  digitalWrite(PIN_RELAY, RELAY_OFF);
  pinMode(PIN_RELAY, OUTPUT);

  // Nút bấm có điện trở kéo lên ngoài
  pinMode(PIN_BUTTON, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LIGHT_AO, ADC_11db);

  // Tạo các đối tượng đồng bộ FreeRTOS trước khi gắn ngắt và khởi tạo Task
  xStateMutex     = xSemaphoreCreateMutex();
  xButtonSem      = xSemaphoreCreateBinary();
  xTxImmediateSem = xSemaphoreCreateBinary();
  xRxCmdQueue     = xQueueCreate(6, sizeof(EspNowPacket));

  // Gắn ngắt cạnh xuống cho nút bấm (phản hồi tức thời)
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButtonISR, FALLING);

  // Khởi tạo Wi-Fi/ESP-NOW trước rồi mới bật Task BLE
  initEspNow();

  // Khởi chạy 3 Task FreeRTOS ghim rõ từng nhân
  xTaskCreatePinnedToCore(TaskButtonRelay, "BtnRelay", 2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(TaskEspNow,      "EspNow",   4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(TaskBleClient,   "BleCli",   6144, nullptr, 1, nullptr, 0);

  Serial.println("[SYSTEM] ESP32-WROOM FreeRTOS san sang (3 Tasks | Dual-Core)!");
}

void loop() {
  // Xóa loopTask mặc định của Arduino để tiết kiệm ~8KB RAM
  vTaskDelete(NULL);
}