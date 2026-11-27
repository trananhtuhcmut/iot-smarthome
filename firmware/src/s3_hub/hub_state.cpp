#include "hub_state.h"
#include <math.h>
#include <Preferences.h>

const char* TOPIC_TELEMETRY  = "smarthome/bedroom/telemetry";
const char* TOPIC_HUB_STATUS = "smarthome/hub/status";
const char* TOPIC_CMD_SWITCH = "smarthome/cmd/switch";
const char* TOPIC_CMD_MODE   = "smarthome/cmd/mode";
const char* TOPIC_AI_ALERT   = "smarthome/ai/alert";

SemaphoreHandle_t xStateMutex          = nullptr;
SemaphoreHandle_t xConfigMutex         = nullptr;
SemaphoreHandle_t xMqttPubTelemetrySem = nullptr;
SemaphoreHandle_t xWsBroadcastSem      = nullptr;
QueueHandle_t     xEspNowRxQueue       = nullptr;
QueueHandle_t     xEspNowTxQueue       = nullptr;

volatile NetState currentNetState = NET_INIT;
SystemState       sysState;
HistoryPoint      envHistory[HISTORY_MAX_POINTS];
uint8_t           historyHead       = 0;
uint8_t           historyCount      = 0;
static uint32_t   lastHistoryPushMs = 0;

volatile bool          mqttConfigDirty    = false;
volatile bool          pendingReboot      = false;
uint32_t               pendingRebootAtMs  = 0;
volatile WifiProvState wifiProvState      = PROV_IDLE;
volatile bool          provIpDelivered    = false;
volatile uint32_t      apShutdownAtMs     = 0;
uint32_t               provStartAtMs      = 0;
char                   provAssignedIp[20] = "";

static AppConfig   appConfig = {"", "", "", DEFAULT_MQTT_PORT, "", ""};
static Preferences prefs;

void initHubIpc() {
  xStateMutex          = xSemaphoreCreateMutex();
  xConfigMutex         = xSemaphoreCreateMutex();
  xMqttPubTelemetrySem = xSemaphoreCreateBinary();
  xWsBroadcastSem      = xSemaphoreCreateBinary();
  xEspNowRxQueue       = xQueueCreate(8, sizeof(EspNowRxItem));
  xEspNowTxQueue       = xQueueCreate(8, sizeof(EspNowTxAction));
}

const char* netStateToStr(NetState st) {
  switch (st) {
    case NET_INIT:             return "INIT (RED)";
    case NET_ONLINE:           return "ONLINE: WiFi+MQTT (GREEN)";
    case NET_MQTT_OFFLINE_LAN: return "LAN ONLY: MQTT Offline (PURPLE)";
    case NET_WIFI_OFFLINE_AP:  return "OFFLINE AP MODE (BLUE)";
    default:                   return "UNKNOWN";
  }
}

void notifyWebClients() {
  if (xWsBroadcastSem != nullptr) {
    xSemaphoreGive(xWsBroadcastSem);
  }
}

void setNetState(NetState nextState) {
  if (currentNetState != nextState) {
    Serial.printf("[FSM] %s  --->  %s\n",
                  netStateToStr(currentNetState),
                  netStateToStr(nextState));
    currentNetState = nextState;
    notifyWebClients();
    queueEspNowAction(TX_SYNC_LVGL);
  }
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

SystemState getStateSnapshot() {
  SystemState snap;
  if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snap = sysState;
    xSemaphoreGive(xStateMutex);
  }
  return snap;
}

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

bool applyToggleFromUi() {
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

  if (canControl) {
    queueEspNowAction(TX_CMD_WROOM);
    queueEspNowAction(TX_SYNC_LVGL);
    xSemaphoreGive(xMqttPubTelemetrySem);
    notifyWebClients();
  }
  return canControl;
}

bool applyAwayFromUi() {
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

  if (canControl) {
    queueEspNowAction(TX_CMD_WROOM);
    queueEspNowAction(TX_SYNC_LVGL);
    xSemaphoreGive(xMqttPubTelemetrySem);
    notifyWebClients();
  }
  return canControl;
}