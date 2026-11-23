#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include "espnow_protocol.h"
#include "ble_protocol.h"

// ==================== CẤU HÌNH PHẦN CỨNG ====================
#define PIN_RELAY         26
#define PIN_BUTTON        27
#define PIN_LIGHT_AO      34  // ADC1 - cắm chân AO của MH-Sensor khi có hàng

// Relay kích mức LOW (tải đấu vào COM + NO; nếu đấu COM + NC thì đảo ngược 2 dòng này)
#define RELAY_ON          LOW
#define RELAY_OFF         HIGH

// 1 = Giả lập cảm biến ánh sáng, 0 = Đọc thật từ GPIO 34
#define MOCK_LIGHT_SENSOR 1

// ==================== CẤU HÌNH ESP-NOW ====================
// BẮT BUỘC thay bằng địa chỉ MAC thật của YoloUNO để tính năng tự săn kênh hoạt động!
// Ví dụ nếu YoloUNO in ra AA:BB:CC:DD:EE:FF thì điền {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
uint8_t S3_HUB_MAC[6] = {0x80, 0xB5, 0x4E, 0xE2, 0x01, 0x14};

const char* TARGET_WIFI_SSID    = "TEN_WIFI_NHA_BAN";
const uint8_t DEFAULT_WIFI_CHAN = 1; // Khớp với FALLBACK_CHANNEL của YoloUNO

// ==================== BIẾN TRẠNG THÁI CHUNG ====================
bool relayState = false;
bool awayMode   = false;
uint16_t packetSeq = 0;
uint8_t currentEspNowChannel = DEFAULT_WIFI_CHAN;

// Chống dội nút bấm thủ công (có điện trở kéo lên ngoài -> nhấn = LOW)
const unsigned long DEBOUNCE_MS = 50;
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long lastDebounceTime = 0;

// Chu kỳ gửi gói tin ESP-NOW định kỳ (3 giây)
const unsigned long TELEMETRY_INTERVAL_MS = 3000;
unsigned long lastTelemetryTime = 0;

// Đồng bộ lệnh đến từ ESP-NOW
portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool hasPendingCmd = false;
EspNowPacket incomingCmd;

// Theo dõi ACK phần cứng để tự săn kênh 1-13
volatile bool espNowSendDone = false;
volatile bool espNowLastOk   = false;
volatile bool isHuntingChan  = false;
volatile uint8_t espNowFailCount = 0;

// ==================== BIẾN QUẢN LÝ BLE (NimBLE 2.x) ====================
portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;
float bleTemp   = 0.0f;
float bleHum    = 0.0f;
bool  bleOnline = false;

NimBLEClient* pBleClient = nullptr;
NimBLEAddress targetBleAddr;
volatile bool doBleConnect = false;
volatile bool bleConnected = false;
volatile bool bleScanning  = false;
unsigned long lastBleScanAttempt = 0;
const unsigned long BLE_RESCAN_INTERVAL_MS = 4000;
const uint32_t BLE_SCAN_DURATION_MS = 4000;

// ==================== PHÂN TÍCH DỮ LIỆU BLE TỪ ESP32-C3 ====================
void parseBlePayload(const uint8_t* data, size_t len, const char* tag) {
  char buf[32];
  size_t copyLen = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
  memcpy(buf, data, copyLen);
  buf[copyLen] = '\0';

  float t = 0.0f, h = 0.0f;
  if (sscanf(buf, "%f,%f", &t, &h) == 2) {
    portENTER_CRITICAL(&bleMux);
    bleTemp   = t;
    bleHum    = h;
    bleOnline = true;
    portEXIT_CRITICAL(&bleMux);

    Serial.printf("[BLE %s] Nhan tu C3: '%s' -> Temp=%.1f C | Hum=%.1f %%\n",
                  tag, buf, t, h);
  }
}

void bleNotifyCallback(NimBLERemoteCharacteristic* pChr, uint8_t* pData, size_t len, bool isNotify) {
  parseBlePayload(pData, len, isNotify ? "NOTIFY" : "INDICATE");
}

class EnvClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    Serial.printf("[BLE] >>> DA KET NOI (BIND) THANH CONG toi %s!\n",
                  pClient->getPeerAddress().toString().c_str());
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    bleConnected = false;
    portENTER_CRITICAL(&bleMux);
    bleOnline = false;
    portEXIT_CRITICAL(&bleMux);
    lastBleScanAttempt = millis();
    Serial.printf("[BLE] Mat ket noi voi C3 (reason=%d) -> ble_online=0\n", reason);
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

void initBleClient() {
  NimBLEDevice::init("SmartHome_WROOM");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setInterval(100);
  pScan->setWindow(45);
  pScan->setActiveScan(true);

  startBleScan();
}

// ==================== ĐIỀU KHIỂN RELAY & ÁNH SÁNG ====================
void applyRelay(bool turnOn) {
  relayState = turnOn;
  digitalWrite(PIN_RELAY, relayState ? RELAY_ON : RELAY_OFF);
  Serial.printf("[RELAY] Trang thai: %s (GPIO %d = %d)\n",
                relayState ? "BAT (ON)" : "TAT (OFF)",
                PIN_RELAY,
                relayState ? RELAY_ON : RELAY_OFF);
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

// ==================== ESP-NOW & TỰ SĂN KÊNH 1-13 ====================
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
  float t, h;
  bool online;
  portENTER_CRITICAL(&bleMux);
  t      = bleTemp;
  h      = bleHum;
  online = bleOnline;
  portEXIT_CRITICAL(&bleMux);

  EspNowPacket pkt = {};
  pkt.node_id     = NODE_WROOM_ROOM;
  pkt.msg_type    = MSG_TELEMETRY;
  pkt.seq         = ++packetSeq;
  pkt.temperature = t;
  pkt.humidity    = h;
  pkt.light_lux   = readLightLevel();
  pkt.switch_mask = relayState ? SW_BED_LIGHT_BIT : 0;
  pkt.away_mode   = awayMode ? 1 : 0;
  pkt.alert_code  = ALERT_NONE;
  pkt.ble_online  = online ? 1 : 0;
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
                relayState ? "ON" : "OFF",
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
  if (len != sizeof(EspNowPacket)) return;

  portENTER_CRITICAL_ISR(&cmdMux);
  memcpy(&incomingCmd, data, sizeof(EspNowPacket));
  hasPendingCmd = true;
  portEXIT_CRITICAL_ISR(&cmdMux);
}

void huntS3Channel() {
  if (isBroadcastMac(S3_HUB_MAC)) {
    Serial.println("[CANH BAO] S3_HUB_MAC dang la broadcast FF:FF... -> Khong the tu san kenh!");
    espNowFailCount = 0;
    return;
  }

  isHuntingChan = true;

  // Tạm dừng quét BLE để dành toàn bộ sóng 2.4GHz cho việc dò kênh (~250ms)
  if (bleScanning) {
    NimBLEDevice::getScan()->stop();
    bleScanning = false;
  }

  Serial.println("[ESP-NOW] Mat ACK 3 lan -> Bat dau tu do kenh 1..13 tim YoloUNO...");
  EspNowPacket probe = buildTelemetryPacket();

  for (uint8_t ch = 1; ch <= 13; ch++) {
    lockEspNowChannel(ch);
    delay(15);

    espNowSendDone = false;
    espNowLastOk   = false;
    esp_now_send(S3_HUB_MAC, (uint8_t*)&probe, sizeof(probe));

    unsigned long t0 = millis();
    while (!espNowSendDone && (millis() - t0 < 80)) {
      delay(5);
    }

    if (espNowLastOk) {
      Serial.printf("[ESP-NOW] >>> DA BAT LAI DUOC YOLOUNO O KENH %u!\n", ch);
      espNowFailCount = 0;
      isHuntingChan   = false;
      return;
    }
  }

  // Nếu cả YoloUNO cũng đang tắt nguồn -> lùi về kênh mặc định 1
  lockEspNowChannel(DEFAULT_WIFI_CHAN);
  espNowFailCount = 0;
  isHuntingChan   = false;
  Serial.printf("[ESP-NOW] Chua tim thay YoloUNO -> Tam dung o kenh %u.\n", DEFAULT_WIFI_CHAN);
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
  peerInfo.channel = 0; // Dùng đúng kênh vừa khóa ở lockEspNowChannel()
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ESP-NOW] Khong the them peer YoloUNO!");
  }
}

// ==================== NÚT BẤM THỦ CÔNG & LỆNH TỪ YOLOUNO ====================
void handleManualButton() {
  bool reading = digitalRead(PIN_BUTTON);
  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    if (reading != stableButtonState) {
      stableButtonState = reading;
      if (stableButtonState == LOW) {
        Serial.println("[BUTTON] Nhan nut thu cong -> Dao trang thai Relay");
        applyRelay(!relayState);
        sendTelemetryToHub();
      }
    }
  }
  lastButtonReading = reading;
}

void handleIncomingCommand() {
  if (!hasPendingCmd) return;

  EspNowPacket cmd;
  portENTER_CRITICAL(&cmdMux);
  memcpy(&cmd, &incomingCmd, sizeof(EspNowPacket));
  hasPendingCmd = false;
  portEXIT_CRITICAL(&cmdMux);

  if (cmd.msg_type == MSG_COMMAND) {
    awayMode = (cmd.away_mode != 0);
    if (awayMode) {
      applyRelay(false);
    } else {
      bool targetState = (cmd.switch_mask & SW_BED_LIGHT_BIT) != 0;
      applyRelay(targetState);
    }
    sendTelemetryToHub();
  }
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(500);

  digitalWrite(PIN_RELAY, RELAY_OFF);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BUTTON, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LIGHT_AO, ADC_11db);

  // Bắt buộc khởi tạo ESP-NOW trước rồi mới bật NimBLE
  initEspNow();
  initBleClient();

  Serial.println("[SYSTEM] ESP32-WROOM san sang (BLE + ESP-NOW Self-Healing + Relay)!");
}

void loop() {
  // 1. Tự săn kênh 1-13 nếu mất ACK 3 gói liên tiếp từ YoloUNO
  if (espNowFailCount >= 3) {
    huntS3Channel();
  }

  // 2. Quản lý kết nối BLE với ESP32-C3 Mini
  if (doBleConnect) {
    doBleConnect = false;
    if (!connectToBleServer()) {
      lastBleScanAttempt = millis();
    }
  } else if (!bleConnected && !bleScanning &&
             (millis() - lastBleScanAttempt >= BLE_RESCAN_INTERVAL_MS)) {
    startBleScan();
  }

  // 3. Xử lý nút bấm thủ công & lệnh từ YoloUNO
  handleManualButton();
  handleIncomingCommand();

  // 4. Đẩy dữ liệu định kỳ mỗi 3 giây
  if (millis() - lastTelemetryTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTime = millis();
    sendTelemetryToHub();
  }
}