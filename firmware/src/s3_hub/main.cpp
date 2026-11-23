#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "espnow_protocol.h"
#include <LittleFS.h>

// ==================== CẤU HÌNH PHẦN CỨNG ====================
#define NEO_PIN          45    // Đèn RGB NeoPixel tích hợp trên mạch YoloUNO (ESP32-S3)
#define LED_COUNT        1
#define NEO_BRIGHTNESS   60    // Độ sáng 0-255
#define PIN_BOOT_BTN     0     // Nút BOOT tích hợp trên YoloUNO (nhấn giữ 5s để reset NVS)

// Chế độ AP cứu hộ / cấu hình ban đầu (phát khi chưa có Wi-Fi hoặc mất router)
#define FALLBACK_CHANNEL  1
#define FALLBACK_AP_SSID  "YoloUNO_Offline"
#define FALLBACK_AP_PASS  "12345678"
#define DEFAULT_MQTT_PORT 1883

// Giám sát mất kết nối WROOM & bộ đệm đồ thị
#define ESPNOW_TIMEOUT_MS       10000 // Quá 10s không nhận gói từ WROOM -> Offline
#define HISTORY_MAX_POINTS      30    // Lưu 30 mẫu gần nhất (~90 giây) cho đồ thị
#define HISTORY_MIN_INTERVAL_MS 2000  // Chống ghi dồn mẫu khi bấm nút nhanh

#define FW_VERSION "1.2.0-chart"

// ==================== CÁC TOPIC LOCAL MQTT (HỢP ĐỒNG 2) ====================
const char* TOPIC_TELEMETRY  = "smarthome/bedroom/telemetry";
const char* TOPIC_HUB_STATUS = "smarthome/hub/status";
const char* TOPIC_CMD_SWITCH = "smarthome/cmd/switch";
const char* TOPIC_CMD_MODE   = "smarthome/cmd/mode";
const char* TOPIC_AI_ALERT   = "smarthome/ai/alert";

// ==================== FSM TRẠNG THÁI MẠNG (4 MÀU LED) ====================
enum NetState : uint8_t {
  NET_INIT             = 0, // Đỏ: Đang khởi tạo
  NET_ONLINE           = 1, // Xanh lá: Wi-Fi OK + MQTT OK (Web T07 trên LAN)
  NET_MQTT_OFFLINE_LAN = 2, // Tím: Wi-Fi OK, MQTT lỗi (Web T07 vẫn chạy trên LAN)
  NET_WIFI_OFFLINE_AP  = 3  // Xanh dương: Mất Wi-Fi / Chưa cấu hình -> Phát AP riêng kênh 1
};

volatile NetState currentNetState = NET_INIT;

const char* netStateToStr(NetState st) {
  switch (st) {
    case NET_INIT:             return "INIT (RED)";
    case NET_ONLINE:           return "ONLINE: WiFi+MQTT (GREEN)";
    case NET_MQTT_OFFLINE_LAN: return "LAN ONLY: MQTT Offline (PURPLE)";
    case NET_WIFI_OFFLINE_AP:  return "OFFLINE AP MODE (BLUE)";
    default:                   return "UNKNOWN";
  }
}

void setNetState(NetState nextState) {
  if (currentNetState != nextState) {
    Serial.printf("[FSM] %s  --->  %s\n",
                  netStateToStr(currentNetState),
                  netStateToStr(nextState));
    currentNetState = nextState;
  }
}

// ==================== CẤU HÌNH LƯU TRONG NVS (PREFERENCES) ====================
struct AppConfig {
  char     wifi_ssid[33];
  char     wifi_pass[65];
  char     mqtt_host[65];
  uint16_t mqtt_port;
  char     mqtt_user[33];
  char     mqtt_pass[65];
};

AppConfig appConfig = {"", "", "", DEFAULT_MQTT_PORT, "", ""};
Preferences prefs;

// ==================== IPC & ĐỒNG BỘ FREERTOS ====================
struct EspNowRxItem {
  uint8_t mac[6];
  EspNowPacket pkt;
};

enum EspNowTxAction : uint8_t {
  TX_CMD_WROOM  = 1,
  TX_SYNC_LVGL  = 2,
  TX_ALERT_LVGL = 3
};

SemaphoreHandle_t xStateMutex          = nullptr;
SemaphoreHandle_t xConfigMutex         = nullptr;
SemaphoreHandle_t xMqttPubTelemetrySem = nullptr;
QueueHandle_t     xEspNowRxQueue       = nullptr;
QueueHandle_t     xEspNowTxQueue       = nullptr;

// ==================== TRẠNG THÁI DÙNG CHUNG & LỊCH SỬ ĐỒ THỊ ====================
struct SystemState {
  float    temperature   = 0.0f;
  float    humidity      = 0.0f;
  uint16_t light_lux     = 0;
  uint8_t  switch_mask   = 0;
  bool     away_mode     = false;
  bool     espnow_online = false; // Trạng thái kết nối tới WROOM
  bool     ble_online    = false; // Trạng thái kết nối BLE từ C3 qua WROOM
  uint8_t  alert_code    = ALERT_NONE;
  char     alert_msg[32] = "OK";
  uint16_t last_rx_seq   = 0;
  uint32_t last_rx_ms    = 0;
} sysState;

struct HistoryPoint {
  float temp;
  float hum;
};

HistoryPoint envHistory[HISTORY_MAX_POINTS];
uint8_t  historyHead       = 0;
uint8_t  historyCount      = 0;
uint32_t lastHistoryPushMs = 0;

// Chỉ gọi khi ĐÃ giữ xStateMutex
void pushEnvHistoryLocked(float temp, float hum) {
  uint32_t now = millis();
  if (historyCount > 0 && (now - lastHistoryPushMs < HISTORY_MIN_INTERVAL_MS)) {
    return;
  }
  envHistory[historyHead].temp = roundf(temp * 10.0f) / 10.0f;
  envHistory[historyHead].hum  = roundf(hum * 10.0f) / 10.0f;
  historyHead = (historyHead + 1) % HISTORY_MAX_POINTS;
  if (historyCount < HISTORY_MAX_POINTS) {
    historyCount++;
  }
  lastHistoryPushMs = now;
}

// ==================== BIẾN NỘI BỘ TỪNG TASK ====================
uint8_t wroomMac[6] = {0};
bool hasWroomMac    = false;
uint8_t lvglMac[6]  = {0};
bool hasLvglMac     = false;
uint16_t hubSeq     = 0;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
char     activeMqttHost[65] = "";
uint16_t activeMqttPort     = DEFAULT_MQTT_PORT;
char     activeMqttUser[33] = "";
char     activeMqttPass[65] = "";

volatile bool mqttConfigDirty = false;
volatile bool pendingReboot   = false;
uint32_t pendingRebootAtMs    = 0;

bool isRetryingWifi         = false;
bool forceMqttRetryNow      = false;
uint32_t lastWifiRetryMs    = 0;
uint32_t wifiRetryStartMs   = 0;
uint32_t lastMqttRetryMs    = 0;
uint32_t lastStatusPubMs    = 0;

const uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
const uint32_t WIFI_RETRY_TIMEOUT_MS  = 5000;
const uint32_t MQTT_RETRY_INTERVAL_MS = 5000;
const uint32_t STATUS_PUB_INTERVAL_MS = 10000;

WebServer webServer(80);
DNSServer dnsServer;
bool dnsRunning = false;

// ==================== HÀM ĐỌC / GHI / XÓA NVS ====================
AppConfig getConfigSnapshot() {
  AppConfig snap = {"", "", "", DEFAULT_MQTT_PORT, "", ""};
  if (xSemaphoreTake(xConfigMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snap = appConfig;
    xSemaphoreGive(xConfigMutex);
  }
  return snap;
}

void loadConfigFromNvs() {
  AppConfig cfg = {"", "", "", DEFAULT_MQTT_PORT, "", ""};
  if (prefs.begin("smarthome", true)) {
    String sSsid  = prefs.getString("wifi_ssid", "");
    String sPass  = prefs.getString("wifi_pass", "");
    String sHost  = prefs.getString("mqtt_host", "");
    uint16_t port = prefs.getUShort("mqtt_port", DEFAULT_MQTT_PORT);
    String sUser  = prefs.getString("mqtt_user", "");
    String sMpas  = prefs.getString("mqtt_pass", "");
    prefs.end();

    strncpy(cfg.wifi_ssid, sSsid.c_str(), sizeof(cfg.wifi_ssid) - 1);
    strncpy(cfg.wifi_pass, sPass.c_str(), sizeof(cfg.wifi_pass) - 1);
    strncpy(cfg.mqtt_host, sHost.c_str(), sizeof(cfg.mqtt_host) - 1);
    cfg.mqtt_port = (port == 0) ? DEFAULT_MQTT_PORT : port;
    strncpy(cfg.mqtt_user, sUser.c_str(), sizeof(cfg.mqtt_user) - 1);
    strncpy(cfg.mqtt_pass, sMpas.c_str(), sizeof(cfg.mqtt_pass) - 1);
  }

  if (xSemaphoreTake(xConfigMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    appConfig = cfg;
    xSemaphoreGive(xConfigMutex);
  }

  Serial.printf("[NVS] Da nap cau hinh | WiFi='%s' | MQTT='%s:%u'\n",
                strlen(cfg.wifi_ssid) ? cfg.wifi_ssid : "(CHUA CAI DAT)",
                strlen(cfg.mqtt_host) ? cfg.mqtt_host : "(CHUA CAI DAT)",
                cfg.mqtt_port);
}

void saveConfigToNvs(const AppConfig& cfg) {
  if (prefs.begin("smarthome", false)) {
    prefs.putString("wifi_ssid", cfg.wifi_ssid);
    prefs.putString("wifi_pass", cfg.wifi_pass);
    prefs.putString("mqtt_host", cfg.mqtt_host);
    prefs.putUShort("mqtt_port", cfg.mqtt_port);
    prefs.putString("mqtt_user", cfg.mqtt_user);
    prefs.putString("mqtt_pass", cfg.mqtt_pass);
    prefs.end();
  }

  if (xSemaphoreTake(xConfigMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    appConfig = cfg;
    xSemaphoreGive(xConfigMutex);
  }

  Serial.printf("[NVS] Da luu cau hinh moi | WiFi='%s' | MQTT='%s:%u'\n",
                cfg.wifi_ssid, cfg.mqtt_host, cfg.mqtt_port);
}

void clearConfigNvs() {
  if (prefs.begin("smarthome", false)) {
    prefs.clear();
    prefs.end();
  }
  Serial.println("[NVS] >>> DA XOA SACH CAU HINH (FACTORY RESET)!");
}

// ==================== HÀM TIỆN ÍCH CHUNG ====================
SystemState getStateSnapshot() {
  SystemState snap;
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snap = sysState;
    xSemaphoreGive(xStateMutex);
  }
  return snap;
}

void queueEspNowAction(EspNowTxAction action) {
  if (xEspNowTxQueue != nullptr) {
    xQueueSend(xEspNowTxQueue, &action, 0);
  }
}

void printMac(const uint8_t* mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ==================== TASK 4: NEOPIXEL + NÚT BOOT RESET 5S (CORE 1 - ƯU TIÊN 1) ====================
void TaskNeoPixelAndBootBtn(void* pvParameters) {
  Adafruit_NeoPixel strip(LED_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
  strip.begin();
  strip.setBrightness(NEO_BRIGHTNESS);
  strip.clear();
  strip.show();

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  uint8_t lastRenderedState = 255;
  uint32_t btnHoldStartMs   = 0;
  bool wasHoldingBtn        = false;
  bool blinkToggle          = false;

  for (;;) {
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
      if (!wasHoldingBtn) {
        wasHoldingBtn  = true;
        btnHoldStartMs = millis();
        Serial.println("[BOOT] Dang giu nut BOOT... Giu du 5 giay de xoa cau hinh NVS!");
      }

      uint32_t heldMs = millis() - btnHoldStartMs;

      blinkToggle = !blinkToggle;
      uint32_t c = blinkToggle ? strip.Color(255, 0, 0) : strip.Color(0, 0, 0);
      for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, c);
      strip.show();

      if (heldMs >= 5000) {
        clearConfigNvs();
        for (int k = 0; k < 4; k++) {
          uint32_t wc = (k % 2 == 0) ? strip.Color(255, 255, 255) : strip.Color(0, 0, 0);
          for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, wc);
          strip.show();
          vTaskDelay(pdMS_TO_TICKS(150));
        }
        ESP.restart();
      }

      vTaskDelay(pdMS_TO_TICKS(120));
      continue;
    } else if (wasHoldingBtn) {
      wasHoldingBtn     = false;
      lastRenderedState = 255;
      Serial.println("[BOOT] Da tha nut truoc 5 giay -> Huy Factory Reset.");
    }

    NetState st = currentNetState;
    if ((uint8_t)st != lastRenderedState) {
      lastRenderedState = (uint8_t)st;

      uint32_t color = 0;
      switch (st) {
        case NET_INIT:             color = strip.Color(255, 0, 0);   break; // Đỏ
        case NET_ONLINE:           color = strip.Color(0, 255, 0);   break; // Xanh lá
        case NET_MQTT_OFFLINE_LAN: color = strip.Color(180, 0, 255); break; // Tím
        case NET_WIFI_OFFLINE_AP:  color = strip.Color(0, 0, 255);   break; // Xanh dương
      }

      for (int i = 0; i < LED_COUNT; i++) {
        strip.setPixelColor(i, color);
      }
      strip.show();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ==================== LOGIC ESP-NOW ====================
bool ensureEspNowPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  esp_err_t res = esp_now_add_peer(&peerInfo);
  if (res == ESP_OK) {
    Serial.print("[ESP-NOW] Da dang ky peer moi: ");
    printMac(mac);
    Serial.println();
    return true;
  }
  return false;
}

void sendEspNowToPeer(const uint8_t* mac, bool hasPeer, const EspNowPacket& pkt, const char* peerName) {
  if (!hasPeer) {
    Serial.printf("[ESP-NOW] Chua biet MAC cua %s -> Bo qua gui.\n", peerName);
    return;
  }
  if (!ensureEspNowPeer(mac)) return;

  esp_err_t res = esp_now_send(mac, (const uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[ESP-NOW -> %s] type=%u | sw=0x%02X | away=%u | alert=%u | Send=%s\n",
                peerName, pkt.msg_type, pkt.switch_mask, pkt.away_mode, pkt.alert_code,
                (res == ESP_OK) ? "OK" : "ERR");
}

void executeEspNowTx(EspNowTxAction action) {
  SystemState snap = getStateSnapshot();
  EspNowPacket pkt = {};
  pkt.node_id     = NODE_S3_HUB;
  pkt.seq         = ++hubSeq;
  pkt.temperature = snap.temperature;
  pkt.humidity    = snap.humidity;
  pkt.light_lux   = snap.light_lux;
  pkt.switch_mask = snap.switch_mask;
  pkt.away_mode   = snap.away_mode ? 1 : 0;
  pkt.alert_code  = snap.alert_code;
  pkt.espnow_online = snap.espnow_online ? 1 : 0;
  pkt.ble_online  = (snap.espnow_online && snap.ble_online) ? 1 : 0;
  strncpy(pkt.alert_msg, snap.alert_msg, sizeof(pkt.alert_msg) - 1);
  pkt.alert_msg[sizeof(pkt.alert_msg) - 1] = '\0';

  if (action == TX_CMD_WROOM) {
    pkt.msg_type = MSG_COMMAND;
    sendEspNowToPeer(wroomMac, hasWroomMac, pkt, "WROOM");
  } else if (action == TX_SYNC_LVGL) {
    pkt.msg_type = MSG_STATE_SYNC;
    sendEspNowToPeer(lvglMac, hasLvglMac, pkt, "LVGL");
  } else if (action == TX_ALERT_LVGL) {
    pkt.msg_type = MSG_ALERT;
    sendEspNowToPeer(lvglMac, hasLvglMac, pkt, "LVGL");
  }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onDataSent(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
#else
void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
#endif
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("[ESP-NOW] Gui xuong node khong nhan duoc ACK!");
  }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac = info->src_addr;
#else
void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != sizeof(EspNowPacket) || xEspNowRxQueue == nullptr) return;

  EspNowRxItem item;
  memcpy(item.mac, mac, 6);
  memcpy(&item.pkt, data, sizeof(EspNowPacket));
  xQueueSend(xEspNowRxQueue, &item, 0);
}

// ==================== TASK 1: ĐIỀU PHỐI ESP-NOW + GIÁM SÁT TIMEOUT (CORE 1 - ƯU TIÊN 3) ====================
void TaskEspNow(void* pvParameters) {
  EspNowRxItem rxItem;
  EspNowTxAction txAction;

  for (;;) {
    // 1. Nhận gói tin ESP-NOW từ WROOM / LVGL
    while (xQueueReceive(xEspNowRxQueue, &rxItem, 0) == pdTRUE) {
      const EspNowPacket& p = rxItem.pkt;

      if (p.node_id == NODE_WROOM_ROOM) {
        if (!hasWroomMac || memcmp(wroomMac, rxItem.mac, 6) != 0) {
          memcpy(wroomMac, rxItem.mac, 6);
          hasWroomMac = true;
          ensureEspNowPeer(wroomMac);
        }

        if (p.msg_type == MSG_TELEMETRY) {
          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!sysState.espnow_online) {
              Serial.println("[ESP-NOW] >>> WROOM da ket noi tro lai!");
            }
            sysState.espnow_online = true;
            sysState.temperature   = p.temperature;
            sysState.humidity      = p.humidity;
            sysState.light_lux     = p.light_lux;
            sysState.switch_mask   = p.switch_mask;
            sysState.away_mode     = (p.away_mode != 0);
            sysState.ble_online    = (p.ble_online != 0);
            sysState.last_rx_seq   = p.seq;
            sysState.last_rx_ms    = millis();

            // Chỉ lưu vào đồ thị khi cảm biến BLE đang có số liệu hợp lệ
            if (sysState.ble_online) {
              pushEnvHistoryLocked(p.temperature, p.humidity);
            }
            xSemaphoreGive(xStateMutex);
          }

          Serial.printf("[ESP-NOW <- WROOM] seq=%u | BLE=%s (T=%.1fC, H=%.1f%%) | Light=%u | Relay=%s\n",
                        p.seq,
                        p.ble_online ? "ON" : "OFF",
                        p.temperature,
                        p.humidity,
                        p.light_lux,
                        (p.switch_mask & SW_BED_LIGHT_BIT) ? "ON" : "OFF");

          xSemaphoreGive(xMqttPubTelemetrySem);
          executeEspNowTx(TX_SYNC_LVGL);
        }
      } else if (p.node_id == NODE_LVGL_PANEL) {
        if (!hasLvglMac || memcmp(lvglMac, rxItem.mac, 6) != 0) {
          memcpy(lvglMac, rxItem.mac, 6);
          hasLvglMac = true;
          ensureEspNowPeer(lvglMac);
        }

        if (p.msg_type == MSG_COMMAND) {
          uint8_t newMask = p.switch_mask;
          bool newAway    = (p.away_mode != 0);
          if (newAway) {
            newMask &= ~SW_BED_LIGHT_BIT;
          }

          if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            sysState.switch_mask = newMask;
            sysState.away_mode   = newAway;
            xSemaphoreGive(xStateMutex);
          }

          Serial.printf("[ESP-NOW <- LVGL] Lenh sw=0x%02X | away=%d -> Chuyen tiep xuong WROOM\n",
                        newMask, newAway);
          executeEspNowTx(TX_CMD_WROOM);
          xSemaphoreGive(xMqttPubTelemetrySem);
        }
      }
    }

    // 2. Kiểm tra mất kết nối WROOM (quá 10 giây không nhận gói ESP-NOW)
    bool wroomTimedOut = false;
    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (sysState.espnow_online && (millis() - sysState.last_rx_ms > ESPNOW_TIMEOUT_MS)) {
        sysState.espnow_online = false;
        sysState.ble_online    = false;
        wroomTimedOut          = true;
      }
      xSemaphoreGive(xStateMutex);
    }

    if (wroomTimedOut) {
      Serial.println("[ESP-NOW] Mat ket noi WROOM (>10s khong co goi tin) -> espnow_online=0, ble_online=0");
      xSemaphoreGive(xMqttPubTelemetrySem);
      executeEspNowTx(TX_SYNC_LVGL);
    }

    // 3. Gửi các lệnh ESP-NOW đang chờ
    while (xQueueReceive(xEspNowTxQueue, &txAction, 0) == pdTRUE) {
      executeEspNowTx(txAction);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ==================== LOGIC MQTT & FSM MẠNG ====================
void syncActiveMqttConfig() {
  AppConfig cfg = getConfigSnapshot();
  strncpy(activeMqttHost, cfg.mqtt_host, sizeof(activeMqttHost) - 1);
  activeMqttHost[sizeof(activeMqttHost) - 1] = '\0';
  activeMqttPort = cfg.mqtt_port;
  strncpy(activeMqttUser, cfg.mqtt_user, sizeof(activeMqttUser) - 1);
  activeMqttUser[sizeof(activeMqttUser) - 1] = '\0';
  strncpy(activeMqttPass, cfg.mqtt_pass, sizeof(activeMqttPass) - 1);
  activeMqttPass[sizeof(activeMqttPass) - 1] = '\0';

  if (strlen(activeMqttHost) > 0) {
    mqttClient.setServer(activeMqttHost, activeMqttPort);
  }
}

void publishBedroomTelemetry() {
  if (!mqttClient.connected()) return;

  SystemState snap = getStateSnapshot();
  JsonDocument doc;
  doc["device"]        = "Bedroom_Node";
  doc["ts"]            = (uint32_t)(millis() / 1000);
  doc["espnow_online"] = snap.espnow_online;
  doc["ble_online"]    = snap.espnow_online && snap.ble_online;
  doc["temperature"]   = roundf(snap.temperature * 10.0f) / 10.0f;
  doc["humidity"]      = roundf(snap.humidity * 10.0f) / 10.0f;
  doc["light"]         = snap.light_lux;

  JsonObject sw = doc["switches"].to<JsonObject>();
  sw["sw1_bed_light"]  = (bool)(snap.switch_mask & SW_BED_LIGHT_BIT);

  doc["away_mode"]     = snap.away_mode;

  char outBuf[384];
  size_t len = serializeJson(doc, outBuf, sizeof(outBuf));
  bool ok = mqttClient.publish(TOPIC_TELEMETRY, (const uint8_t*)outBuf, len, false);
  Serial.printf("[MQTT -> %s] %s (%s)\n", TOPIC_TELEMETRY, outBuf, ok ? "OK" : "FAIL");
}

void publishHubStatus() {
  if (!mqttClient.connected()) return;

  SystemState snap = getStateSnapshot();
  JsonDocument doc;
  doc["device"]       = "YoloUNO_Hub";
  doc["online"]       = true;
  doc["away_mode"]    = snap.away_mode;
  doc["fw_version"]   = FW_VERSION;
  doc["wifi_channel"] = (long)WiFi.channel();
  doc["rssi"]         = WiFi.RSSI();
  doc["ip"]           = WiFi.localIP().toString();

  char outBuf[256];
  serializeJson(doc, outBuf, sizeof(outBuf));
  mqttClient.publish(TOPIC_HUB_STATUS, outBuf, true);
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT <- %s] ", topic);
  for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
  Serial.println();

  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;

  if (strcmp(topic, TOPIC_CMD_SWITCH) == 0) {
    const char* swName = doc["switch"] | "";
    bool targetState   = doc["state"]  | false;
    const char* src    = doc["source"] | "unknown";

    if (strcmp(swName, "sw1_bed_light") == 0) {
      Serial.printf("[MQTT CMD] sw1_bed_light=%s (source=%s)\n",
                    targetState ? "ON" : "OFF", src);

      if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (targetState) {
          sysState.switch_mask |= SW_BED_LIGHT_BIT;
          sysState.away_mode = false;
        } else {
          sysState.switch_mask &= ~SW_BED_LIGHT_BIT;
        }
        xSemaphoreGive(xStateMutex);
      }
      queueEspNowAction(TX_CMD_WROOM);
      queueEspNowAction(TX_SYNC_LVGL);
    }
  } else if (strcmp(topic, TOPIC_CMD_MODE) == 0) {
    bool away     = doc["away_mode"]      | false;
    bool offLight = doc["turn_off_light"] | true;

    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      sysState.away_mode = away;
      if (away && offLight) {
        sysState.switch_mask &= ~SW_BED_LIGHT_BIT;
      }
      xSemaphoreGive(xStateMutex);
    }
    queueEspNowAction(TX_CMD_WROOM);
    queueEspNowAction(TX_SYNC_LVGL);
    publishHubStatus();
  } else if (strcmp(topic, TOPIC_AI_ALERT) == 0) {
    uint8_t code    = doc["alert_code"] | ALERT_NONE;
    const char* msg = doc["message"]    | (code == ALERT_NONE ? "OK" : "CANH BAO AN NINH");

    if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      sysState.alert_code = code;
      strncpy(sysState.alert_msg, msg, sizeof(sysState.alert_msg) - 1);
      sysState.alert_msg[sizeof(sysState.alert_msg) - 1] = '\0';
      xSemaphoreGive(xStateMutex);
    }
    queueEspNowAction(TX_ALERT_LVGL);
  }
}

bool connectMqttOnce() {
  if (strlen(activeMqttHost) == 0) {
    Serial.printf("[MQTT] Chua cai dat IP Broker -> Mo http://%s/config de nhap.\n",
                  WiFi.localIP().toString().c_str());
    return false;
  }

  String clientId = "YoloUNO_Hub_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  const char* lwtPayload = "{\"device\":\"YoloUNO_Hub\",\"online\":false}";

  Serial.printf("[MQTT] Dang ket noi Broker %s:%u ...\n", activeMqttHost, activeMqttPort);
  bool ok = (strlen(activeMqttUser) > 0)
    ? mqttClient.connect(clientId.c_str(), activeMqttUser, activeMqttPass, TOPIC_HUB_STATUS, 1, true, lwtPayload)
    : mqttClient.connect(clientId.c_str(), TOPIC_HUB_STATUS, 1, true, lwtPayload);

  if (ok) {
    Serial.println("[MQTT] >>> KET NOI LOCAL BROKER THANH CONG!");
    mqttClient.subscribe(TOPIC_CMD_SWITCH, 1);
    mqttClient.subscribe(TOPIC_CMD_MODE, 1);
    mqttClient.subscribe(TOPIC_AI_ALERT, 1);
    publishHubStatus();
    lastStatusPubMs = millis();
    return true;
  }

  Serial.printf("[MQTT] Ket noi that bai (rc=%d) -> Giu Web T07 tren mang LAN (http://%s/), thu lai sau 5s.\n",
                mqttClient.state(), WiFi.localIP().toString().c_str());
  return false;
}

void enterWifiOfflineAp() {
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }
  WiFi.disconnect(false);
  WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASS, FALLBACK_CHANNEL);
  esp_wifi_set_channel(FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);

  setNetState(NET_WIFI_OFFLINE_AP);
  lastWifiRetryMs = millis();
  isRetryingWifi  = false;

  Serial.printf("[WIFI] Phat AP '%s' (Pass: %s | Kenh %d | Cau hinh: http://%s/config)\n",
                FALLBACK_AP_SSID, FALLBACK_AP_PASS, FALLBACK_CHANNEL,
                WiFi.softAPIP().toString().c_str());
}

// ==================== TASK 2: FSM MẠNG & MQTT (CORE 0 - ƯU TIÊN 1) ====================
void TaskNetworkMqtt(void* pvParameters) {
  mqttClient.setBufferSize(512);
  mqttClient.setCallback(onMqttMessage);
  syncActiveMqttConfig();

  for (;;) {
    if (mqttConfigDirty) {
      mqttConfigDirty = false;
      if (mqttClient.connected()) {
        mqttClient.disconnect();
      }
      syncActiveMqttConfig();
      forceMqttRetryNow = true;
      if (currentNetState == NET_ONLINE) {
        setNetState(NET_MQTT_OFFLINE_LAN);
      }
    }

    switch (currentNetState) {
      case NET_INIT: {
        if (WiFi.status() != WL_CONNECTED) {
          enterWifiOfflineAp();
        } else {
          WiFi.softAPdisconnect(true);
          if (connectMqttOnce()) {
            setNetState(NET_ONLINE);
          } else {
            lastMqttRetryMs = millis();
            setNetState(NET_MQTT_OFFLINE_LAN);
          }
        }
        break;
      }

      case NET_ONLINE: {
        if (WiFi.status() != WL_CONNECTED) {
          enterWifiOfflineAp();
          break;
        }
        if (!mqttClient.connected()) {
          Serial.println("[MQTT] Mat ket noi Broker -> Chuyen sang che do LAN (Tim)!");
          lastMqttRetryMs = millis();
          setNetState(NET_MQTT_OFFLINE_LAN);
          break;
        }

        mqttClient.loop();

        if (xSemaphoreTake(xMqttPubTelemetrySem, 0) == pdTRUE) {
          publishBedroomTelemetry();
        }
        if (millis() - lastStatusPubMs >= STATUS_PUB_INTERVAL_MS) {
          lastStatusPubMs = millis();
          publishHubStatus();
        }
        break;
      }

      case NET_MQTT_OFFLINE_LAN: {
        if (WiFi.status() != WL_CONNECTED) {
          enterWifiOfflineAp();
          break;
        }

        if (forceMqttRetryNow || (millis() - lastMqttRetryMs >= MQTT_RETRY_INTERVAL_MS)) {
          forceMqttRetryNow = false;
          lastMqttRetryMs   = millis();
          if (connectMqttOnce()) {
            setNetState(NET_ONLINE);
          }
        }
        break;
      }

      case NET_WIFI_OFFLINE_AP: {
        if (WiFi.status() == WL_CONNECTED) {
          WiFi.softAPdisconnect(true);
          isRetryingWifi    = false;
          forceMqttRetryNow = true;
          Serial.printf("[WIFI] >>> Router da song lai! Kenh %ld | Web LAN: http://%s/\n",
                        (long)WiFi.channel(), WiFi.localIP().toString().c_str());
          setNetState(NET_MQTT_OFFLINE_LAN);
          break;
        }

        AppConfig cfg = getConfigSnapshot();
        if (strlen(cfg.wifi_ssid) == 0) {
          break;
        }

        if (isRetryingWifi) {
          if (millis() - wifiRetryStartMs >= WIFI_RETRY_TIMEOUT_MS) {
            Serial.println("[WIFI] Router van offline -> Khoa lai kenh 1.");
            enterWifiOfflineAp();
          }
        } else if (millis() - lastWifiRetryMs >= WIFI_RETRY_INTERVAL_MS) {
          Serial.printf("[WIFI] Thu ket noi lai router '%s'...\n", cfg.wifi_ssid);
          isRetryingWifi   = true;
          wifiRetryStartMs = millis();
          WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
        }
        break;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ==================== LOCAL WEB SERVER (T07 - LITTLEFS + JSON API) ====================
void serveHtmlFile(const char* path) {
  if (!LittleFS.exists(path)) {
    webServer.send(404, "text/plain",
                   "Chua nap file HTML! Hay chay 'Upload Filesystem Image' trong PlatformIO.");
    return;
  }
  File file = LittleFS.open(path, "r");
  webServer.streamFile(file, "text/html");
  file.close();
}

void handleWebRoot()   { serveHtmlFile("/index.html");  }
void handleWebConfig() { serveHtmlFile("/config.html"); }

void handleApiStatus() {
  SystemState snap;
  float histTempCopy[HISTORY_MAX_POINTS];
  float histHumCopy[HISTORY_MAX_POINTS];
  uint8_t histCountCopy = 0;

  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snap = sysState;
    histCountCopy = historyCount;
    uint8_t startIdx = (historyCount == HISTORY_MAX_POINTS) ? historyHead : 0;
    for (uint8_t i = 0; i < historyCount; i++) {
      uint8_t idx     = (startIdx + i) % HISTORY_MAX_POINTS;
      histTempCopy[i] = envHistory[idx].temp;
      histHumCopy[i]  = envHistory[idx].hum;
    }
    xSemaphoreGive(xStateMutex);
  }

  NetState st = currentNetState;
  String netBanner, badgeColor;
  if (st == NET_ONLINE) {
    netBanner  = "ONLINE (Wi-Fi + MQTT) | LAN: " + WiFi.localIP().toString();
    badgeColor = "#16a34a";
  } else if (st == NET_MQTT_OFFLINE_LAN) {
    netBanner  = "LAN MODE (MQTT Offline) | LAN: " + WiFi.localIP().toString();
    badgeColor = "#9333ea";
  } else if (st == NET_WIFI_OFFLINE_AP) {
    netBanner  = "OFFLINE AP MODE (Ch " + String(FALLBACK_CHANNEL) + ") | AP: " + WiFi.softAPIP().toString();
    badgeColor = "#2563eb";
  } else {
    netBanner  = "INITIALIZING...";
    badgeColor = "#dc2626";
  }

  bool espNowOk = snap.espnow_online;
  bool bleOk    = espNowOk && snap.ble_online;

  JsonDocument doc;
  doc["net_banner"]    = netBanner;
  doc["badge_color"]   = badgeColor;
  doc["espnow_online"] = espNowOk;
  doc["ble_online"]    = bleOk;
  doc["temperature"]   = roundf(snap.temperature * 10.0f) / 10.0f;
  doc["humidity"]      = roundf(snap.humidity * 10.0f) / 10.0f;
  doc["light_lux"]     = snap.light_lux;
  doc["relay_on"]      = (bool)(snap.switch_mask & SW_BED_LIGHT_BIT);
  doc["away_mode"]     = snap.away_mode;
  doc["alert_code"]    = snap.alert_code;
  doc["alert_msg"]     = snap.alert_msg;
  doc["last_rx_seq"]   = snap.last_rx_seq;
  doc["fw_version"]    = FW_VERSION;
  doc["channel"]       = (long)WiFi.channel();
  doc["mac"]           = WiFi.macAddress();

  JsonArray arrT = doc["hist_temp"].to<JsonArray>();
  JsonArray arrH = doc["hist_hum"].to<JsonArray>();
  for (uint8_t i = 0; i < histCountCopy; i++) {
    arrT.add(histTempCopy[i]);
    arrH.add(histHumCopy[i]);
  }

  String out;
  serializeJson(doc, out);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", out);
}

void handleApiConfig() {
  AppConfig cfg = getConfigSnapshot();

  JsonDocument doc;
  doc["ssid"]      = cfg.wifi_ssid;
  doc["has_pass"]  = (strlen(cfg.wifi_pass) > 0);
  doc["mqtt_host"] = cfg.mqtt_host;
  doc["mqtt_port"] = cfg.mqtt_port;
  doc["mqtt_user"] = cfg.mqtt_user;

  String out;
  serializeJson(doc, out);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", out);
}

void handleApiToggle() {
  bool canControl = false;
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (sysState.espnow_online) {
      canControl = true;
      bool relayOn = (sysState.switch_mask & SW_BED_LIGHT_BIT) != 0;
      if (relayOn) {
        sysState.switch_mask &= ~SW_BED_LIGHT_BIT;
      } else {
        sysState.switch_mask |= SW_BED_LIGHT_BIT;
        sysState.away_mode = false;
      }
    }
    xSemaphoreGive(xStateMutex);
  }

  Serial.printf("[WEB] /api/toggle | espnow_online=%d\n", canControl ? 1 : 0);

  if (canControl) {
    queueEspNowAction(TX_CMD_WROOM);
    queueEspNowAction(TX_SYNC_LVGL);
    xSemaphoreGive(xMqttPubTelemetrySem);
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json", "{\"ok\":true}");
  } else {
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json", "{\"ok\":false,\"error\":\"wroom_offline\"}");
  }
}

void handleApiAway() {
  bool canControl = false;
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (sysState.espnow_online) {
      canControl = true;
      sysState.away_mode = !sysState.away_mode;
      if (sysState.away_mode) {
        sysState.switch_mask &= ~SW_BED_LIGHT_BIT;
      }
    }
    xSemaphoreGive(xStateMutex);
  }

  Serial.printf("[WEB] /api/away | espnow_online=%d\n", canControl ? 1 : 0);

  if (canControl) {
    queueEspNowAction(TX_CMD_WROOM);
    queueEspNowAction(TX_SYNC_LVGL);
    xSemaphoreGive(xMqttPubTelemetrySem);
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json", "{\"ok\":true}");
  } else {
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json", "{\"ok\":false,\"error\":\"wroom_offline\"}");
  }
}

void handleWebSave() {
  AppConfig oldCfg = getConfigSnapshot();
  AppConfig newCfg = oldCfg;

  String inSsid = webServer.arg("ssid");      inSsid.trim();
  String inPass = webServer.arg("pass");
  String inHost = webServer.arg("mqtt_host"); inHost.trim();
  String inPort = webServer.arg("mqtt_port"); inPort.trim();
  String inUser = webServer.arg("mqtt_user"); inUser.trim();
  String inMpas = webServer.arg("mqtt_pass");

  if (inSsid.length() == 0) {
    webServer.send(400, "text/plain", "Missing Wi-Fi SSID!");
    return;
  }

  bool ssidChanged = (strcmp(oldCfg.wifi_ssid, inSsid.c_str()) != 0);
  bool passChanged = false;

  strncpy(newCfg.wifi_ssid, inSsid.c_str(), sizeof(newCfg.wifi_ssid) - 1);
  newCfg.wifi_ssid[sizeof(newCfg.wifi_ssid) - 1] = '\0';

  if (inPass.length() > 0) {
    passChanged = (strcmp(oldCfg.wifi_pass, inPass.c_str()) != 0);
    strncpy(newCfg.wifi_pass, inPass.c_str(), sizeof(newCfg.wifi_pass) - 1);
    newCfg.wifi_pass[sizeof(newCfg.wifi_pass) - 1] = '\0';
  } else if (ssidChanged) {
    newCfg.wifi_pass[0] = '\0';
  }

  strncpy(newCfg.mqtt_host, inHost.c_str(), sizeof(newCfg.mqtt_host) - 1);
  newCfg.mqtt_host[sizeof(newCfg.mqtt_host) - 1] = '\0';

  int portVal = inPort.toInt();
  newCfg.mqtt_port = (portVal > 0 && portVal <= 65535) ? (uint16_t)portVal : DEFAULT_MQTT_PORT;

  strncpy(newCfg.mqtt_user, inUser.c_str(), sizeof(newCfg.mqtt_user) - 1);
  newCfg.mqtt_user[sizeof(newCfg.mqtt_user) - 1] = '\0';

  if (inMpas.length() > 0) {
    strncpy(newCfg.mqtt_pass, inMpas.c_str(), sizeof(newCfg.mqtt_pass) - 1);
    newCfg.mqtt_pass[sizeof(newCfg.mqtt_pass) - 1] = '\0';
  } else if (strlen(newCfg.mqtt_user) == 0) {
    newCfg.mqtt_pass[0] = '\0';
  }

  saveConfigToNvs(newCfg);
  serveHtmlFile("/saved.html");

  bool needReboot = (ssidChanged || passChanged || WiFi.status() != WL_CONNECTED);
  if (needReboot) {
    pendingReboot     = true;
    pendingRebootAtMs = millis() + 2000;
  } else {
    mqttConfigDirty = true;
  }
}

void handleWebReset() {
  clearConfigNvs();
  serveHtmlFile("/saved.html");
  pendingReboot     = true;
  pendingRebootAtMs = millis() + 2000;
}

void handleWebNotFound() {
  if (currentNetState == NET_WIFI_OFFLINE_AP) {
    webServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/config", true);
    webServer.send(302, "text/plain", "");
    return;
  }
  webServer.send(404, "text/plain", "404 Not Found");
}

void TaskWebServer(void* pvParameters) {
  webServer.on("/",           HTTP_GET,  handleWebRoot);
  webServer.on("/config",     HTTP_GET,  handleWebConfig);
  webServer.on("/api/status", HTTP_GET,  handleApiStatus);
  webServer.on("/api/config", HTTP_GET,  handleApiConfig);
  webServer.on("/api/toggle", HTTP_ANY,  handleApiToggle);
  webServer.on("/api/away",   HTTP_ANY,  handleApiAway);
  webServer.on("/save",       HTTP_POST, handleWebSave);
  webServer.on("/reset",      HTTP_POST, handleWebReset);
  webServer.onNotFound(handleWebNotFound);
  webServer.begin();
  Serial.println("[WEB T07] Local Web Server (LittleFS) dang chay tren cong 80!");

  for (;;) {
    if (currentNetState == NET_WIFI_OFFLINE_AP && !dnsRunning) {
      dnsServer.start(53, "*", WiFi.softAPIP());
      dnsRunning = true;
    } else if (currentNetState != NET_WIFI_OFFLINE_AP && dnsRunning) {
      dnsServer.stop();
      dnsRunning = false;
    }

    if (dnsRunning) {
      dnsServer.processNextRequest();
    }

    webServer.handleClient();

    if (pendingReboot && (int32_t)(millis() - pendingRebootAtMs) >= 0) {
      ESP.restart();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ==================== KHỞI TẠO WI-FI (TỪ NVS) & ESP-NOW ====================
void initWiFiAndEspNow() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);

  AppConfig cfg = getConfigSnapshot();
  if (strlen(cfg.wifi_ssid) > 0) {
    WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
    Serial.printf("[WIFI] Dang ket noi router '%s'", cfg.wifi_ssid);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0 < 10000)) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      WiFi.softAPdisconnect(true);
      Serial.printf("[WIFI] Ket noi router OK! Web T07 LAN: http://%s/ | Kenh: %ld\n",
                    WiFi.localIP().toString().c_str(), (long)WiFi.channel());
    } else {
      Serial.println("[WIFI] Khong vao duoc router -> FSM se chuyen sang AP cuu ho.");
    }
  } else {
    Serial.println("[WIFI] NVS chua co Wi-Fi -> Chuyen thang sang AP cau hinh.");
    delay(400);
  }

  Serial.println("===================================================");
  Serial.print("[QUAN TRONG] Copy MAC nay dan vao WROOM: ");
  Serial.println(WiFi.macAddress());
  Serial.println("===================================================");

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Khoi tao that bai!");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  xStateMutex          = xSemaphoreCreateMutex();
  xConfigMutex         = xSemaphoreCreateMutex();
  xMqttPubTelemetrySem = xSemaphoreCreateBinary();
  xEspNowRxQueue       = xQueueCreate(8, sizeof(EspNowRxItem));
  xEspNowTxQueue       = xQueueCreate(8, sizeof(EspNowTxAction));

  currentNetState = NET_INIT;
  xTaskCreatePinnedToCore(TaskNeoPixelAndBootBtn, "NeoBoot", 2560, nullptr, 1, nullptr, 1);

  loadConfigFromNvs();
  initWiFiAndEspNow();

  xTaskCreatePinnedToCore(TaskEspNow, "EspNowHub", 4096, nullptr, 3, nullptr, 1);

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Loi khoi tao LittleFS!");
  } else {
    Serial.println("[FS] Da mount LittleFS thanh cong!");
  }

  xTaskCreatePinnedToCore(TaskWebServer,   "WebServer", 8192, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(TaskNetworkMqtt, "NetMqtt",   6144, nullptr, 1, nullptr, 0);

  Serial.println("[SYSTEM] YoloUNO S3 Hub (FreeRTOS + FSM + NVS + Chart) san sang!");
}

void loop() {
  vTaskDelete(NULL);
}