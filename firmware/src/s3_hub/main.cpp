#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "espnow_protocol.h"

// ==================== CẤU HÌNH WI-FI & LOCAL MQTT ====================
const char* WIFI_SSID    = "TEN_WIFI_NHA_BAN";   // Trùng khớp với TARGET_WIFI_SSID trên WROOM
const char* WIFI_PASS    = "MAT_KHAU_WIFI";

const char* MQTT_BROKER  = "192.168.1.100";      // IP máy tính / Jetson / Pi chạy Mosquitto
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER    = "";                   // Để trống nếu Mosquitto không đặt mật khẩu
const char* MQTT_PASS    = "";

// Cấu hình chế độ cứu hộ khi router Wi-Fi lỗi / mất điện
#define FALLBACK_CHANNEL  1
#define FALLBACK_AP_SSID  "YoloUNO_Offline"
#define FALLBACK_AP_PASS  "12345678"

#define FW_VERSION "1.0.0"

// ==================== CÁC TOPIC LOCAL MQTT (HỢP ĐỒNG 2) ====================
const char* TOPIC_TELEMETRY  = "smarthome/bedroom/telemetry";
const char* TOPIC_HUB_STATUS = "smarthome/hub/status";
const char* TOPIC_CMD_SWITCH = "smarthome/cmd/switch";
const char* TOPIC_CMD_MODE   = "smarthome/cmd/mode";
const char* TOPIC_AI_ALERT   = "smarthome/ai/alert";

// ==================== QUẢN LÝ PEER ESP-NOW ====================
uint8_t wroomMac[6] = {0};
bool hasWroomMac    = false;

uint8_t lvglMac[6]  = {0};
bool hasLvglMac     = false;

struct EspNowRxItem {
  uint8_t mac[6];
  EspNowPacket pkt;
};

#define RX_QUEUE_SIZE 8
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
EspNowRxItem rxQueue[RX_QUEUE_SIZE];
volatile uint8_t rxHead = 0;
volatile uint8_t rxTail = 0;

// ==================== TRẠNG THÁI HỆ THỐNG ====================
struct SystemState {
  float    temperature   = 0.0f;
  float    humidity      = 0.0f;
  uint16_t light_lux     = 0;
  uint8_t  switch_mask   = 0;
  bool     away_mode     = false;
  bool     ble_online    = false;
  uint8_t  alert_code    = ALERT_NONE;
  char     alert_msg[32] = "OK";
  uint16_t last_rx_seq   = 0;
  unsigned long last_rx_ms = 0;
} sysState;

uint16_t hubSeq = 0;

// ==================== QUẢN LÝ WI-FI FALLBACK & MQTT ====================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WebServer webServer(80);

bool inFallbackMode           = false;
bool isRetryingWifi           = false;
unsigned long lastWifiRetryMs = 0;
unsigned long wifiRetryStartMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000; // 30s thử tìm lại router 1 lần
const unsigned long WIFI_RETRY_TIMEOUT_MS  = 5000;  // Quá 5s không thấy -> quay về kênh 1 ngay

unsigned long lastMqttRetryMs = 0;
unsigned long lastStatusPubMs = 0;
const unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
const unsigned long STATUS_PUB_INTERVAL_MS = 10000;

// ==================== TIỆN ÍCH ESP-NOW ====================
void printMac(const uint8_t* mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool ensureEspNowPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0; // Theo đúng kênh radio hiện tại
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  esp_err_t res = esp_now_add_peer(&peerInfo);
  if (res == ESP_OK) {
    Serial.print("[ESP-NOW] Da dang ky peer moi: ");
    printMac(mac);
    Serial.println();
    return true;
  }
  Serial.printf("[ESP-NOW] Loi dang ky peer (err=%d)\n", res);
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

void sendCommandToWroom() {
  EspNowPacket pkt = {};
  pkt.node_id     = NODE_S3_HUB;
  pkt.msg_type    = MSG_COMMAND;
  pkt.seq         = ++hubSeq;
  pkt.switch_mask = sysState.switch_mask;
  pkt.away_mode   = sysState.away_mode ? 1 : 0;
  pkt.alert_code  = sysState.alert_code;
  strncpy(pkt.alert_msg, sysState.alert_msg, sizeof(pkt.alert_msg) - 1);

  sendEspNowToPeer(wroomMac, hasWroomMac, pkt, "WROOM");
}

void syncStateToLvgl(uint8_t msgType = MSG_STATE_SYNC) {
  EspNowPacket pkt = {};
  pkt.node_id     = NODE_S3_HUB;
  pkt.msg_type    = msgType;
  pkt.seq         = ++hubSeq;
  pkt.temperature = sysState.temperature;
  pkt.humidity    = sysState.humidity;
  pkt.light_lux   = sysState.light_lux;
  pkt.switch_mask = sysState.switch_mask;
  pkt.away_mode   = sysState.away_mode ? 1 : 0;
  pkt.alert_code  = sysState.alert_code;
  pkt.ble_online  = sysState.ble_online ? 1 : 0;
  strncpy(pkt.alert_msg, sysState.alert_msg, sizeof(pkt.alert_msg) - 1);

  sendEspNowToPeer(lvglMac, hasLvglMac, pkt, "LVGL");
}

// ==================== CALLBACKS ESP-NOW (ESP-IDF 5.5) ====================
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
  if (len != sizeof(EspNowPacket)) return;

  portENTER_CRITICAL_ISR(&rxMux);
  uint8_t nextHead = (rxHead + 1) % RX_QUEUE_SIZE;
  if (nextHead != rxTail) {
    memcpy(rxQueue[rxHead].mac, mac, 6);
    memcpy(&rxQueue[rxHead].pkt, data, sizeof(EspNowPacket));
    rxHead = nextHead;
  }
  portEXIT_CRITICAL_ISR(&rxMux);
}

// ==================== ĐẨY & NHẬN LOCAL MQTT ====================
void publishBedroomTelemetry() {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["device"]      = "Bedroom_Node";
  doc["ts"]          = (uint32_t)(millis() / 1000);
  doc["temperature"] = round(sysState.temperature * 10.0f) / 10.0f;
  doc["humidity"]    = round(sysState.humidity * 10.0f) / 10.0f;
  doc["ble_online"]  = sysState.ble_online;
  doc["light"]       = sysState.light_lux;

  JsonObject sw = doc["switches"].to<JsonObject>();
  sw["sw1_bed_light"]  = (bool)(sysState.switch_mask & SW_BED_LIGHT_BIT);
  sw["sw2_hall_light"] = (bool)(sysState.switch_mask & SW_HALL_LIGHT_BIT);
  sw["sw3_fan"]        = (bool)(sysState.switch_mask & SW_FAN_BIT);
  sw["sw4_buzzer"]     = (bool)(sysState.switch_mask & SW_BUZZER_BIT);

  doc["away_mode"]   = sysState.away_mode;

  char outBuf[384];
  size_t len = serializeJson(doc, outBuf, sizeof(outBuf));
  bool ok = mqttClient.publish(TOPIC_TELEMETRY, (const uint8_t*)outBuf, len, false);
  Serial.printf("[MQTT -> %s] %s (%s)\n", TOPIC_TELEMETRY, outBuf, ok ? "OK" : "FAIL");
}

void publishHubStatus() {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["device"]       = "YoloUNO_Hub";
  doc["online"]       = true;
  doc["fw_version"]   = FW_VERSION;
  doc["wifi_channel"] = WiFi.channel();
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
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[MQTT] Loi parse JSON: %s\n", err.c_str());
    return;
  }

  if (strcmp(topic, TOPIC_CMD_SWITCH) == 0) {
    const char* swName = doc["switch"] | "";
    bool targetState   = doc["state"]  | false;

    uint8_t bit = 0;
    if      (strcmp(swName, "sw1_bed_light") == 0)  bit = SW_BED_LIGHT_BIT;
    else if (strcmp(swName, "sw2_hall_light") == 0) bit = SW_HALL_LIGHT_BIT;
    else if (strcmp(swName, "sw3_fan") == 0)        bit = SW_FAN_BIT;
    else if (strcmp(swName, "sw4_buzzer") == 0)     bit = SW_BUZZER_BIT;

    if (bit != 0) {
      if (targetState) sysState.switch_mask |= bit;
      else             sysState.switch_mask &= ~bit;

      if (targetState && bit == SW_BED_LIGHT_BIT) sysState.away_mode = false;

      sendCommandToWroom();
      syncStateToLvgl(MSG_STATE_SYNC);
    }
  } else if (strcmp(topic, TOPIC_CMD_MODE) == 0) {
    bool away      = doc["away_mode"]           | false;
    bool offLights = doc["turn_off_all_lights"] | true;

    sysState.away_mode = away;
    if (away && offLights) {
      sysState.switch_mask &= ~(SW_BED_LIGHT_BIT | SW_HALL_LIGHT_BIT);
    }
    sendCommandToWroom();
    syncStateToLvgl(MSG_STATE_SYNC);
  } else if (strcmp(topic, TOPIC_AI_ALERT) == 0) {
    uint8_t code    = doc["alert_code"] | ALERT_NONE;
    bool buzzerOn   = doc["buzzer"]     | false;
    const char* msg = doc["message"]    | (code == ALERT_NONE ? "OK" : "CANH BAO AN NINH");

    sysState.alert_code = code;
    strncpy(sysState.alert_msg, msg, sizeof(sysState.alert_msg) - 1);
    sysState.alert_msg[sizeof(sysState.alert_msg) - 1] = '\0';

    if (buzzerOn) sysState.switch_mask |= SW_BUZZER_BIT;
    else          sysState.switch_mask &= ~SW_BUZZER_BIT;

    sendCommandToWroom();
    syncStateToLvgl(MSG_ALERT);
  }
}

void maintainMqttConnection() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  if (millis() - lastMqttRetryMs < MQTT_RETRY_INTERVAL_MS) return;
  lastMqttRetryMs = millis();

  String clientId = "YoloUNO_Hub_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  const char* lwtPayload = "{\"device\":\"YoloUNO_Hub\",\"online\":false}";

  Serial.printf("[MQTT] Dang ket noi Broker %s:%u ...\n", MQTT_BROKER, MQTT_PORT);
  bool ok = (strlen(MQTT_USER) > 0)
    ? mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, TOPIC_HUB_STATUS, 1, true, lwtPayload)
    : mqttClient.connect(clientId.c_str(), TOPIC_HUB_STATUS, 1, true, lwtPayload);

  if (ok) {
    Serial.println("[MQTT] >>> KET NOI LOCAL BROKER THANH CONG!");
    mqttClient.subscribe(TOPIC_CMD_SWITCH, 1);
    mqttClient.subscribe(TOPIC_CMD_MODE, 1);
    mqttClient.subscribe(TOPIC_AI_ALERT, 1);
    publishHubStatus();
  } else {
    Serial.printf("[MQTT] Ket noi that bai (rc=%d), thu lai sau 5s.\n", mqttClient.state());
  }
}

// ==================== XỬ LÝ GÓI TIN ESP-NOW ====================
void processEspNowQueue() {
  while (true) {
    EspNowRxItem item;
    portENTER_CRITICAL(&rxMux);
    if (rxHead == rxTail) {
      portEXIT_CRITICAL(&rxMux);
      break;
    }
    item = rxQueue[rxTail];
    rxTail = (rxTail + 1) % RX_QUEUE_SIZE;
    portEXIT_CRITICAL(&rxMux);

    const EspNowPacket& p = item.pkt;

    if (p.node_id == NODE_WROOM_ROOM) {
      if (!hasWroomMac || memcmp(wroomMac, item.mac, 6) != 0) {
        memcpy(wroomMac, item.mac, 6);
        hasWroomMac = true;
        ensureEspNowPeer(wroomMac);
      }

      if (p.msg_type == MSG_TELEMETRY) {
        sysState.temperature = p.temperature;
        sysState.humidity    = p.humidity;
        sysState.light_lux   = p.light_lux;
        sysState.switch_mask = p.switch_mask;
        sysState.away_mode   = (p.away_mode != 0);
        sysState.ble_online  = (p.ble_online != 0);
        sysState.last_rx_seq = p.seq;
        sysState.last_rx_ms  = millis();

        Serial.printf("[ESP-NOW <- WROOM] seq=%u | BLE=%s (T=%.1fC, H=%.1f%%) | Light=%u | Relay=%s\n",
                      p.seq,
                      sysState.ble_online ? "ON" : "OFF",
                      sysState.temperature,
                      sysState.humidity,
                      sysState.light_lux,
                      (sysState.switch_mask & SW_BED_LIGHT_BIT) ? "ON" : "OFF");

        publishBedroomTelemetry();
        syncStateToLvgl(MSG_STATE_SYNC);
      }
    } else if (p.node_id == NODE_LVGL_PANEL) {
      if (!hasLvglMac || memcmp(lvglMac, item.mac, 6) != 0) {
        memcpy(lvglMac, item.mac, 6);
        hasLvglMac = true;
        ensureEspNowPeer(lvglMac);
      }

      if (p.msg_type == MSG_COMMAND) {
        sysState.switch_mask = p.switch_mask;
        sysState.away_mode   = (p.away_mode != 0);
        if (sysState.away_mode) {
          sysState.switch_mask &= ~(SW_BED_LIGHT_BIT | SW_HALL_LIGHT_BIT);
        }
        Serial.printf("[ESP-NOW <- LVGL] Lenh sw=0x%02X | away=%d -> Chuyen tiep xuong WROOM\n",
                      sysState.switch_mask, sysState.away_mode);
        sendCommandToWroom();
        publishBedroomTelemetry();
      }
    }
  }
}

// ==================== LOCAL WEB SERVER (T07) ====================
void handleWebRoot() {
  bool relayOn = (sysState.switch_mask & SW_BED_LIGHT_BIT) != 0;
  String netStatus = (WiFi.status() == WL_CONNECTED)
    ? ("Router (Ch " + String(WiFi.channel()) + " | " + WiFi.localIP().toString() + ")")
    : ("AP Cuu Ho (Ch " + String(FALLBACK_CHANNEL) + " | " + WiFi.softAPIP().toString() + ")");

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<meta http-equiv='refresh' content='3'>"
                "<title>YoloUNO Local Hub (T07)</title>"
                "<style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 16px}"
                ".card{border:1px solid #ddd;border-radius:10px;padding:16px;margin-bottom:12px}"
                "button{padding:10px 16px;border:none;border-radius:6px;font-size:15px;cursor:pointer;margin-right:8px}"
                ".on{background:#16a34a;color:#fff}.off{background:#dc2626;color:#fff}"
                ".alert{background:#fee2e2;color:#991b1b;padding:10px;border-radius:6px;font-weight:bold}</style></head><body>"
                "<h2>YoloUNO Local Web Server (T07)</h2>";

  if (sysState.alert_code != ALERT_NONE) {
    html += "<div class='alert'>CANH BAO: " + String(sysState.alert_msg) + "</div><br>";
  }

  html += "<div class='card'><b>Cam bien phong ngu</b><br>"
          "Nhiet do (BLE): " + (sysState.ble_online ? String(sysState.temperature, 1) + " °C" : String("--.- °C (Offline)")) + "<br>"
          "Do am (BLE): " + (sysState.ble_online ? String(sysState.humidity, 1) + " %" : String("--.- %")) + "<br>"
          "Anh sang: " + String(sysState.light_lux) + " / 1000<br>"
          "Goi ESP-NOW gan nhat: #" + String(sysState.last_rx_seq) + "</div>";

  html += "<div class='card'><b>Dieu khien cuc bo (ESP-NOW)</b><br><br>"
          "Den phong ngu: <b>" + String(relayOn ? "DANG BAT" : "DANG TAT") + "</b><br><br>"
          "<a href='/toggle'><button class='" + String(relayOn ? "off" : "on") + "'>"
          + String(relayOn ? "Tat Den" : "Bat Den") + "</button></a>"
          "<a href='/away'><button style='background:#2563eb;color:#fff'>"
          + String(sysState.away_mode ? "Tat Away Mode" : "Bat Away Mode") + "</button></a></div>";

  html += "<small>FW: " FW_VERSION " | Mang: " + netStatus +
          " | MQTT: " + String(mqttClient.connected() ? "Connected" : "Offline") + "</small></body></html>";

  webServer.send(200, "text/html", html);
}

void handleWebToggle() {
  bool relayOn = (sysState.switch_mask & SW_BED_LIGHT_BIT) != 0;
  if (relayOn) {
    sysState.switch_mask &= ~SW_BED_LIGHT_BIT;
  } else {
    sysState.switch_mask |= SW_BED_LIGHT_BIT;
    sysState.away_mode = false;
  }
  sendCommandToWroom();
  syncStateToLvgl(MSG_STATE_SYNC);
  publishBedroomTelemetry();
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void handleWebAway() {
  sysState.away_mode = !sysState.away_mode;
  if (sysState.away_mode) {
    sysState.switch_mask &= ~(SW_BED_LIGHT_BIT | SW_HALL_LIGHT_BIT);
  }
  sendCommandToWroom();
  syncStateToLvgl(MSG_STATE_SYNC);
  publishBedroomTelemetry();
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void initWebServer() {
  webServer.on("/", handleWebRoot);
  webServer.on("/toggle", handleWebToggle);
  webServer.on("/away", handleWebAway);
  webServer.begin();
}

// ==================== KHỞI TẠO WI-FI, FALLBACK & ESP-NOW ====================
void enterFallbackMode(bool forceRelock) {
  if (inFallbackMode && !forceRelock) return;
  WiFi.disconnect(false); // Dừng STA quét ngầm để không nhảy kênh
  WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASS, FALLBACK_CHANNEL);
  esp_wifi_set_channel(FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
  inFallbackMode = true;
  Serial.printf("[WIFI] Router offline -> Khoa kenh %d & bat AP cuu ho '%s' (http://%s/)\n",
                FALLBACK_CHANNEL, FALLBACK_AP_SSID, WiFi.softAPIP().toString().c_str());
}

void initWiFiAndEspNow() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false); // Tắt tự quét ngầm để không làm rớt sóng ESP-NOW
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("[WIFI] Dang ket noi router '%s'", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(true);
    inFallbackMode = false;
    Serial.printf("[WIFI] Ket noi router OK! Web T07: http://%s/ | Kenh: %ld\n",
              WiFi.localIP().toString().c_str(), (long)WiFi.channel());
  } else {
    enterFallbackMode(true);
    lastWifiRetryMs = millis();
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
  Serial.println("[ESP-NOW] San sang nhan goi tin tu WROOM & LVGL!");
}

void maintainWiFiFallback() {
  // Trường hợp 1: Đang kết nối router bình thường
  if (WiFi.status() == WL_CONNECTED) {
    if (inFallbackMode || isRetryingWifi) {
      WiFi.softAPdisconnect(true);
      inFallbackMode = false;
      isRetryingWifi = false;
      Serial.printf("[WIFI] >>> Router da song lai! Chuyen sang kenh %ld | Web: http://%s/\n",
              (long)WiFi.channel(), WiFi.localIP().toString().c_str());
    }
    return;
  }

  // Trường hợp 2: Đang chờ kết nối lại router (tối đa 5s không chặn loop)
  if (isRetryingWifi) {
    if (millis() - wifiRetryStartMs >= WIFI_RETRY_TIMEOUT_MS) {
      isRetryingWifi = false;
      lastWifiRetryMs = millis();
      enterFallbackMode(true);
    }
    return;
  }

  // Trường hợp 3: Router vừa mới mất kết nối -> khóa ngay về kênh cứu hộ
  if (!inFallbackMode) {
    enterFallbackMode(true);
    lastWifiRetryMs = millis();
    return;
  }

  // Trường hợp 4: Đang ở chế độ cứu hộ -> mỗi 30s thử gọi lại router 1 lần
  if (millis() - lastWifiRetryMs >= WIFI_RETRY_INTERVAL_MS) {
    Serial.println("[WIFI] Thu ket noi lai router chinh...");
    isRetryingWifi   = true;
    wifiRetryStartMs = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(1500);

  initWiFiAndEspNow();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setCallback(onMqttMessage);

  initWebServer();
  Serial.println("[SYSTEM] YoloUNO S3 Hub san sang!");
}

void loop() {
  maintainWiFiFallback();
  processEspNowQueue();
  maintainMqttConnection();
  webServer.handleClient();

  if (mqttClient.connected() && (millis() - lastStatusPubMs >= STATUS_PUB_INTERVAL_MS)) {
    lastStatusPubMs = millis();
    publishHubStatus();
  }
}