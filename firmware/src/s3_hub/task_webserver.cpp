#include "task_webserver.h"
#include "hub_state.h"
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>

static WebServer        webServer(80);
static WebSocketsServer webSocket(81);
static DNSServer        dnsServer;
static bool             dnsRunning = false;

static String buildStatusJson() {
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
  return out;
}

static void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[WS #%u] Client ket noi tu %s\n", num, ip.toString().c_str());
      String initJson = buildStatusJson();
      webSocket.sendTXT(num, initJson);
      break;
    }

    case WStype_DISCONNECTED:
      Serial.printf("[WS #%u] Client ngat ket noi\n", num);
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length)) return;

      const char* cmd = doc["cmd"] | "";
      if (strcmp(cmd, "toggle") == 0) {
        Serial.printf("[WS #%u] Lenh toggle relay\n", num);
        applyToggleFromUi();
      } else if (strcmp(cmd, "away") == 0) {
        Serial.printf("[WS #%u] Lenh away mode\n", num);
        applyAwayFromUi();
      } else if (strcmp(cmd, "sync") == 0) {
        String snapJson = buildStatusJson();
        webSocket.sendTXT(num, snapJson);
      }
      break;
    }

    default:
      break;
  }
}

static void serveHtmlFile(const char* path) {
  if (!LittleFS.exists(path)) {
    webServer.send(404, "text/plain",
                   "Chua nap file HTML! Hay chay 'Upload Filesystem Image' trong PlatformIO.");
    return;
  }
  File file = LittleFS.open(path, "r");
  webServer.streamFile(file, "text/html");
  file.close();
}

static void handleWebRoot()   { serveHtmlFile("/index.html");  }
static void handleWebConfig() { serveHtmlFile("/config.html"); }

static void handleApiStatus() {
  String out = buildStatusJson();
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", out);
}

static void handleApiConfig() {
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

static void handleApiScan() {
  Serial.println("[WEB] Dang quet cac mang Wi-Fi 2.4GHz xung quanh...");
  int n = WiFi.scanNetworks(false, false);

  struct WifiItem {
    String  ssid;
    int32_t rssi;
    int32_t ch;
    bool    enc;
  };

  WifiItem list[15];
  int count = 0;

  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;

    int32_t rssi = WiFi.RSSI(i);
    int32_t ch   = WiFi.channel(i);
    bool    enc  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

    int existIdx = -1;
    for (int j = 0; j < count; j++) {
      if (list[j].ssid == s) {
        existIdx = j;
        break;
      }
    }

    if (existIdx >= 0) {
      if (rssi > list[existIdx].rssi) {
        list[existIdx].rssi = rssi;
        list[existIdx].ch   = ch;
        list[existIdx].enc  = enc;
      }
    } else if (count < 15) {
      list[count++] = {s, rssi, ch, enc};
    }
  }

  WiFi.scanDelete();

  if (currentNetState == NET_WIFI_OFFLINE_AP) {
    esp_wifi_set_channel(FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
  }

  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (list[j].rssi > list[i].rssi) {
        WifiItem tmp = list[i];
        list[i] = list[j];
        list[j] = tmp;
      }
    }
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < count; i++) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = list[i].ssid;
    obj["rssi"] = list[i].rssi;
    obj["ch"]   = list[i].ch;
    obj["enc"]  = list[i].enc;
  }

  String out;
  serializeJson(doc, out);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", out);
}

static void handleApiToggle() {
  bool ok = applyToggleFromUi();
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"wroom_offline\"}");
}

static void handleApiAway() {
  bool ok = applyAwayFromUi();
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"wroom_offline\"}");
}

static void handleApiProvStatus() {
  JsonDocument doc;
  WifiProvState st = wifiProvState;

  if (st == PROV_PENDING || st == PROV_CONNECTING) {
    doc["state"] = "connecting";
  } else if (st == PROV_SUCCESS) {
    if (!provIpDelivered) {
      provIpDelivered = true;
      apShutdownAtMs  = millis() + 5000;
    }
    int32_t remMs = (int32_t)(apShutdownAtMs - millis());
    if (remMs < 0) remMs = 0;

    doc["state"]         = "connected";
    doc["ip"]            = provAssignedIp;
    doc["mdns"]          = "yolouno.local";
    doc["remaining_sec"] = (remMs + 999) / 1000;
  } else if (st == PROV_FAILED) {
    doc["state"] = "failed";
  } else if (WiFi.status() == WL_CONNECTED) {
    doc["state"]         = "mqtt_updated";
    doc["ip"]            = WiFi.localIP().toString();
    doc["mdns"]          = "yolouno.local";
    doc["remaining_sec"] = 3;
  } else {
    doc["state"] = "idle";
  }

  String out;
  serializeJson(doc, out);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", out);
}

static void handleWebSave() {
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

  bool needWifiReconnect = (ssidChanged || passChanged || WiFi.status() != WL_CONNECTED);
  if (needWifiReconnect) {
    provIpDelivered = false;
    provStartAtMs   = millis() + 500;
    wifiProvState   = PROV_PENDING;
  } else {
    wifiProvState   = PROV_IDLE;
    mqttConfigDirty = true;
  }
}

static void handleWebReset() {
  clearConfigNvs();
  serveHtmlFile("/saved.html");
  pendingReboot     = true;
  pendingRebootAtMs = millis() + 2000;
}

static void handleWebNotFound() {
  if (currentNetState == NET_WIFI_OFFLINE_AP) {
    webServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/config", true);
    webServer.send(302, "text/plain", "");
    return;
  }
  webServer.send(404, "text/plain", "404 Not Found");
}

void TaskWebServer(void* pvParameters) {
  webServer.on("/",                HTTP_GET,  handleWebRoot);
  webServer.on("/config",          HTTP_GET,  handleWebConfig);
  webServer.on("/api/status",      HTTP_GET,  handleApiStatus);
  webServer.on("/api/config",      HTTP_GET,  handleApiConfig);
  webServer.on("/api/scan",        HTTP_GET,  handleApiScan);
  webServer.on("/api/prov_status", HTTP_GET,  handleApiProvStatus);
  webServer.on("/api/toggle",      HTTP_ANY,  handleApiToggle);
  webServer.on("/api/away",        HTTP_ANY,  handleApiAway);
  webServer.on("/save",            HTTP_POST, handleWebSave);
  webServer.on("/reset",           HTTP_POST, handleWebReset);
  webServer.onNotFound(handleWebNotFound);
  webServer.begin();

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  Serial.println("[WEB T07] HTTP (port 80) + WebSocket (port 81) san sang!");

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
    webSocket.loop();

    if (xSemaphoreTake(xWsBroadcastSem, 0) == pdTRUE) {
      if (webSocket.connectedClients() > 0) {
        String out = buildStatusJson();
        webSocket.broadcastTXT(out);
      }
    }

    if (pendingReboot && (int32_t)(millis() - pendingRebootAtMs) >= 0) {
      ESP.restart();
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}