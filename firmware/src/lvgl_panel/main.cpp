#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "espnow_protocol.h"
#include "ui_panel.h"

// =========================================================================
// 1 = Tu gia lap test man hinh | 0 = Chay ESP-NOW that voi ESP32-S3 (Mem1)
// =========================================================================
#define USE_MOCK_DATA 0

// Cau hinh phan cung ESP32-2432S028 (CYD 2.8")
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define TFT_BL_PIN    21

#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

// MAC that cua ESP32-S3 (YoloUNO - Mem1) & Kenh Wi-Fi dong bo
static uint8_t S3_HUB_MAC[6] = {0x80, 0xB5, 0x4E, 0xE2, 0x01, 0x14};
static uint8_t g_espnowChannel = 6;
static volatile bool g_sendDone = false;
static volatile bool g_lastSendOk = false;

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCREEN_WIDTH * 20];

// Dong bo du lieu giua callback ESP-NOW va loop() cua LVGL
static volatile bool g_hasNewPacket = false;
static EspNowPacket g_rxPacket = {};
static uint32_t g_lastRxMs = 0;
static uint16_t g_txSeq = 0;
static bool g_currentRelayState = false;
static uint8_t g_currentAwayMode = 0;

// -------------------- Driver Hien thi & Cam ung LVGL --------------------
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  LV_UNUSED(indev_driver);
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, 200, 3700, 0, SCREEN_WIDTH - 1);
    int y = map(p.y, 240, 3800, 0, SCREEN_HEIGHT - 1);
    data->point.x = constrain(x, 0, SCREEN_WIDTH - 1);
    data->point.y = constrain(y, 0, SCREEN_HEIGHT - 1);
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// -------------------- Xu ly ESP-NOW (T06 - chuan espnow_protocol.h) --------------------
static const char *getAlertTitle(uint8_t code) {
  switch (code) {
    case ALERT_INTRUSION:       return "INTRUSION ALERT";
    case ALERT_FIRE:            return "FIRE ALERT";
    case ALERT_TEMP_HIGH:       return "HIGH TEMPERATURE ALERT";
    case ALERT_GATEWAY_OFFLINE: return "GATEWAY OFFLINE ALERT";
    default:                    return "SYSTEM ALERT";
  }
}

void onEspNowRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  LV_UNUSED(mac);
  if (len == (int)sizeof(EspNowPacket)) {
    memcpy((void *)&g_rxPacket, incomingData, sizeof(EspNowPacket));
    g_rxPacket.alert_msg[sizeof(g_rxPacket.alert_msg) - 1] = '\0';
    g_hasNewPacket = true;
    g_lastRxMs = millis();
  }
}

void onEspNowSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  LV_UNUSED(mac_addr);
  g_lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
  g_sendDone = true;
  Serial.printf("[ESP-NOW] Send status (CH %d): %s\n",
                g_espnowChannel, g_lastSendOk ? "OK" : "FAIL");
}

static void sendCommandPacket() {
#if !USE_MOCK_DATA
  EspNowPacket cmd = {};
  cmd.node_id     = NODE_LVGL_PANEL;
  cmd.msg_type    = MSG_COMMAND;
  cmd.seq         = ++g_txSeq;
  cmd.switch_mask = g_currentRelayState ? SW_BED_LIGHT_BIT : 0;
  cmd.away_mode   = g_currentAwayMode;

  esp_now_send(S3_HUB_MAC, (uint8_t *)&cmd, sizeof(cmd));
#endif
}

void onUserToggleRelay(bool newState) {
  g_currentRelayState = newState;
  Serial.printf("[UI] Bed Light -> %s\n", newState ? "ON" : "OFF");
  sendCommandPacket();
}

void onUserToggleAwayMode(bool awayMode) {
  g_currentAwayMode = awayMode ? 1 : 0;
  Serial.printf("[UI] Away Mode -> %s\n", awayMode ? "AWAY" : "HOME");
  sendCommandPacket();
}

static void applyEspNowChannel(uint8_t ch) {
  g_espnowChannel = ch;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  esp_now_del_peer(S3_HUB_MAC);
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, S3_HUB_MAC, 6);
  peerInfo.channel = ch;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

static bool autoScanEspNowChannel() {
  Serial.println("[ESP-NOW] Auto-scanning Wi-Fi channel (1-13)...");
  EspNowPacket ping = {};
  ping.node_id  = NODE_LVGL_PANEL;
  ping.msg_type = MSG_STATE_SYNC; // Goi tham do, khong lam doi trang thai Relay

  for (uint8_t ch = 1; ch <= 13; ch++) {
    applyEspNowChannel(ch);

    g_sendDone = false;
    g_lastSendOk = false;
    esp_now_send(S3_HUB_MAC, (uint8_t *)&ping, sizeof(ping));

    uint32_t t0 = millis();
    while (!g_sendDone && (millis() - t0 < 80)) {
      delay(2);
    }

    if (g_lastSendOk) {
      Serial.printf("[ESP-NOW] Locked on S3 Hub channel: %d\n", ch);
      return true;
    }
  }

  Serial.println("[ESP-NOW] S3 Hub not found on channels 1-13.");
  return false;
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  Serial.print("[ESP-NOW] Panel MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed!");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);

  // Tu dong do kenh ngay luc khoi dong
  if (!autoScanEspNowChannel()) {
    applyEspNowChannel(g_espnowChannel); // Giu kenh mac dinh neu S3 chua bat
  }
}

// -------------------- Setup & Loop --------------------
static void refreshSysInfoUi() {
  char hubMacStr[18];
  snprintf(hubMacStr, sizeof(hubMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           S3_HUB_MAC[0], S3_HUB_MAC[1], S3_HUB_MAC[2],
           S3_HUB_MAC[3], S3_HUB_MAC[4], S3_HUB_MAC[5]);
  ui_panel_set_sys_info("--", g_espnowChannel, WiFi.macAddress().c_str(), hubMacStr);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin();
  ts.setRotation(1);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCREEN_WIDTH * 20);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  ui_panel_init(onUserToggleRelay, onUserToggleAwayMode , USE_MOCK_DATA == 1);

#if USE_MOCK_DATA
  ui_panel_set_net_status(NET_MOCK);
  refreshSysInfoUi();
  ui_panel_update_sensors(28.5f, 65.0f, 420, false, false);
  Serial.println("LVGL Panel Ready (MOCK MODE)");
#else
  initEspNow();
  ui_panel_set_net_status(NET_OFFLINE);
  refreshSysInfoUi();
  Serial.println("LVGL Panel Ready (ESP-NOW MODE)");
#endif
}

void loop() {
#if !LV_TICK_CUSTOM
  static uint32_t lastTick = millis();
  uint32_t nowTick = millis();
  lv_tick_inc(nowTick - lastTick);
  lastTick = nowTick;
#endif

#if USE_MOCK_DATA
  static uint32_t lastMockMs = 0;
  static float mockTemp = 28.0f;
  if (millis() - lastMockMs >= 2000) {
    lastMockMs = millis();
    mockTemp += 0.4f;
    if (mockTemp > 36.0f) mockTemp = 28.0f;
    ui_panel_update_sensors(mockTemp, 62.5f, 380 + (int)(mockTemp * 5),
                            g_currentRelayState, g_currentAwayMode != 0);
  }
#else
  if (g_hasNewPacket) {
    g_hasNewPacket = false;
    EspNowPacket pkt;
    memcpy(&pkt, (const void *)&g_rxPacket, sizeof(pkt));

    bool relayOn = (pkt.switch_mask & SW_BED_LIGHT_BIT) != 0;
    g_currentRelayState = relayOn;
    g_currentAwayMode   = pkt.away_mode;

    // Phan biet Online + MQTT bat vs Online + MQTT tat
    if (pkt.alert_code == ALERT_GATEWAY_OFFLINE) {
      ui_panel_set_net_status(NET_ONLINE_NO_MQTT);
    } else {
      ui_panel_set_net_status(NET_ONLINE_MQTT);
    }

    if (pkt.msg_type == MSG_STATE_SYNC || pkt.msg_type == MSG_TELEMETRY) {
      ui_panel_update_sensors(pkt.temperature, pkt.humidity, pkt.light_lux,
                              relayOn, pkt.away_mode != 0,
                              pkt.ble_online != 0, pkt.espnow_online != 0);
    }

    if (pkt.msg_type == MSG_ALERT || pkt.alert_code != ALERT_NONE && pkt.alert_code != ALERT_GATEWAY_OFFLINE) {
      const char *msg = (pkt.alert_msg[0] != '\0') ? pkt.alert_msg : "Check system immediately!";
      ui_panel_show_alert(getAlertTitle(pkt.alert_code), msg);
    }
  }

  // Cap nhat Last data timestamp moi 1 giay
  static uint32_t lastTsUiMs = 0;
  if (millis() - lastTsUiMs >= 1000) {
    lastTsUiMs = millis();
    if (g_lastRxMs > 0) {
      ui_panel_update_last_data((millis() - g_lastRxMs) / 1000, true);
    } else {
      ui_panel_update_last_data(0, false);
    }
  }

  static uint32_t lastRescanMs = 0;
  bool isOffline = (g_lastRxMs == 0) || (millis() - g_lastRxMs > 10000);

  if (isOffline) {
    ui_panel_set_net_status(NET_OFFLINE);
    ui_panel_clear_sensors();

    if (millis() - lastRescanMs > 8000) {
      lastRescanMs = millis();
      if (autoScanEspNowChannel()) {
        g_lastRxMs = millis();
        refreshSysInfoUi(); // Cap nhat lai so Channel moi tim duoc
      }
    }
  }
#endif

  lv_timer_handler();
  delay(5);
}