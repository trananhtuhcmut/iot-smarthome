#include "task_network.h"
#include "hub_state.h"
#include <math.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);

static char     activeMqttHost[65] = "";
static uint16_t activeMqttPort     = DEFAULT_MQTT_PORT;
static char     activeMqttUser[33] = "";
static char     activeMqttPass[65] = "";

static bool     isRetryingWifi    = false;
static bool     forceMqttRetryNow = false;
static uint8_t  mqttFailStreak    = 0;
static uint32_t lastWifiRetryMs   = 0;
static uint32_t wifiRetryStartMs  = 0;
static uint32_t lastMqttRetryMs   = 0;
static uint32_t lastStatusPubMs   = 0;
static uint32_t provDeadlineMs    = 0;
static uint32_t provFailClearAtMs = 0;

static const uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
static const uint32_t WIFI_RETRY_TIMEOUT_MS  = 5000;
static const uint32_t MQTT_RETRY_FAST_MS     = 5000;
static const uint32_t MQTT_RETRY_SLOW_MS     = 15000;
static const uint32_t STATUS_PUB_INTERVAL_MS = 10000;

void startMdnsService() {
  MDNS.end();
  if (MDNS.begin("yolouno")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] San sang tai: http://yolouno.local/");
  }
}

void initWiFiInitial() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
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
      startMdnsService();
      Serial.printf("[WIFI] Ket noi router OK! Web T07 LAN: http://%s/ | Kenh: %ld\n",
                    WiFi.localIP().toString().c_str(), (long)WiFi.channel());
    } else {
      Serial.println("[WIFI] Khong vao duoc router -> FSM se chuyen sang AP cuu ho.");
    }
  } else {
    Serial.println("[WIFI] NVS chua co Wi-Fi -> Chuyen thang sang AP cau hinh.");
    delay(400);
  }
}

static void syncActiveMqttConfig() {
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

static void publishBedroomTelemetry() {
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
  sw["sw1_bed_light"] = (bool)(snap.switch_mask & SW_BED_LIGHT_BIT);

  doc["away_mode"] = snap.away_mode;

  char outBuf[384];
  size_t len = serializeJson(doc, outBuf, sizeof(outBuf));
  bool ok = mqttClient.publish(TOPIC_TELEMETRY, (const uint8_t*)outBuf, len, false);
  Serial.printf("[MQTT -> %s] %s (%s)\n", TOPIC_TELEMETRY, outBuf, ok ? "OK" : "FAIL");
}

static void publishHubStatus() {
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

static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
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
      notifyWebClients();
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
    notifyWebClients();
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
    notifyWebClients();
  }
}

static bool connectMqttOnce() {
  if (strlen(activeMqttHost) == 0) return false;

  String clientId = "YoloUNO_Hub_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  const char* lwtPayload = "{\"device\":\"YoloUNO_Hub\",\"online\":false}";

  Serial.printf("[MQTT] Thu ket noi Broker %s:%u ...\n", activeMqttHost, activeMqttPort);
  bool ok = (strlen(activeMqttUser) > 0)
    ? mqttClient.connect(clientId.c_str(), activeMqttUser, activeMqttPass, TOPIC_HUB_STATUS, 1, true, lwtPayload)
    : mqttClient.connect(clientId.c_str(), TOPIC_HUB_STATUS, 1, true, lwtPayload);

  if (ok) {
    mqttFailStreak = 0;
    Serial.println("[MQTT] >>> KET NOI LOCAL BROKER THANH CONG!");
    mqttClient.subscribe(TOPIC_CMD_SWITCH, 1);
    mqttClient.subscribe(TOPIC_CMD_MODE, 1);
    mqttClient.subscribe(TOPIC_AI_ALERT, 1);
    publishHubStatus();
    lastStatusPubMs = millis();
    return true;
  }

  if (mqttFailStreak < 10) mqttFailStreak++;
  uint32_t nextWaitSec = (mqttFailStreak >= 3) ? (MQTT_RETRY_SLOW_MS / 1000) : (MQTT_RETRY_FAST_MS / 1000);
  Serial.printf("[MQTT] Broker chua phan hoi (rc=%d) -> Giu Web LAN + WS, thu lai sau %lus.\n",
                mqttClient.state(), (unsigned long)nextWaitSec);
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

  Serial.printf("[WIFI] Phat AP '%s' (Pass: %s | Kenh %d | Web: http://%s/)\n",
                FALLBACK_AP_SSID, FALLBACK_AP_PASS, FALLBACK_CHANNEL,
                WiFi.softAPIP().toString().c_str());
}

void TaskNetworkMqtt(void* pvParameters) {
  wifiClient.setTimeout(2);
  mqttClient.setSocketTimeout(2);
  mqttClient.setBufferSize(512);
  mqttClient.setCallback(onMqttMessage);
  syncActiveMqttConfig();

  for (;;) {
    if (wifiProvState != PROV_IDLE) {
      if (wifiProvState == PROV_PENDING) {
        if ((int32_t)(millis() - provStartAtMs) >= 0) {
          AppConfig cfg = getConfigSnapshot();
          if (mqttClient.connected()) mqttClient.disconnect();

          setNetState(NET_INIT);
          WiFi.disconnect(false);
          WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

          provDeadlineMs = millis() + 12000;
          wifiProvState  = PROV_CONNECTING;
          Serial.printf("[PROV] Dang ket noi router '%s' (giu song song AP)...\n", cfg.wifi_ssid);
        }
      } else if (wifiProvState == PROV_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
          snprintf(provAssignedIp, sizeof(provAssignedIp), "%s",
                   WiFi.localIP().toString().c_str());
          startMdnsService();

          provIpDelivered = false;
          apShutdownAtMs  = millis() + 15000;
          wifiProvState   = PROV_SUCCESS;
          Serial.printf("[PROV] >>> Router da cap IP: %s (cho hien len web 5s truoc khi tat AP)\n",
                        provAssignedIp);
        } else if ((int32_t)(millis() - provDeadlineMs) >= 0) {
          Serial.println("[PROV] Ket noi Wi-Fi that bai!");
          wifiProvState     = PROV_FAILED;
          provFailClearAtMs = millis() + 8000;
          enterWifiOfflineAp();
        }
      } else if (wifiProvState == PROV_SUCCESS) {
        if ((int32_t)(millis() - apShutdownAtMs) >= 0) {
          Serial.printf("[PROV] Het 5s hien thi IP -> Tat SoftAP, chuyen sang LAN (%s)\n",
                        provAssignedIp);
          WiFi.softAPdisconnect(true);
          wifiProvState     = PROV_IDLE;
          syncActiveMqttConfig();
          mqttFailStreak    = 0;
          forceMqttRetryNow = true;
          setNetState(NET_MQTT_OFFLINE_LAN);
        }
      } else if (wifiProvState == PROV_FAILED) {
        if ((int32_t)(millis() - provFailClearAtMs) >= 0) {
          wifiProvState = PROV_IDLE;
        }
      }

      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    if (mqttConfigDirty) {
      mqttConfigDirty = false;
      mqttFailStreak  = 0;
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
          mqttFailStreak  = 0;
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
        if (strlen(activeMqttHost) == 0) break;

        uint32_t retryGap = (mqttFailStreak >= 3) ? MQTT_RETRY_SLOW_MS : MQTT_RETRY_FAST_MS;
        if (forceMqttRetryNow || (millis() - lastMqttRetryMs >= retryGap)) {
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
          startMdnsService();
          isRetryingWifi    = false;
          forceMqttRetryNow = true;
          mqttFailStreak    = 0;
          Serial.printf("[WIFI] >>> Router da song lai! Kenh %ld | Web LAN: http://%s/\n",
                        (long)WiFi.channel(), WiFi.localIP().toString().c_str());
          setNetState(NET_MQTT_OFFLINE_LAN);
          break;
        }

        AppConfig cfg = getConfigSnapshot();
        if (strlen(cfg.wifi_ssid) == 0) break;

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