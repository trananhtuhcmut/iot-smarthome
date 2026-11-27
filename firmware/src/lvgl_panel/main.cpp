#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "ui_panel.h"

// =========================================================================
// CHUYEN CHE DO: 1 = Tu gia lap du lieu test UI | 0 = Chay ESP-NOW voi TV1
// =========================================================================
#define USE_MOCK_DATA 1

// Cau hinh phan cung ESP32-2432S028 (CYD 2.8")
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define TFT_BL_PIN    21

#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

// Cau hinh ESP-NOW (T06) - Thay dung MAC va Kenh Wi-Fi cua ESP32-S3 (TV1)
static uint8_t S3_HUB_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
static const uint8_t ESPNOW_WIFI_CHANNEL = 1;

// Struct giao tiep tam (khi merge se thay bang #include "espnow_protocol.h")
typedef struct __attribute__((packed)) {
  float temperature;
  float humidity;
  int32_t lightLux;
  bool relayState;
  bool hasAlert;
  char alertMsg[64];
} TelemetryPacket;

typedef struct __attribute__((packed)) {
  uint8_t cmdType;   // 1 = SET_RELAY
  bool relayTarget;
} ControlPacket;

TFT_eSPI tft = TFT_eSPI();

XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCREEN_WIDTH * 20];

// Co dong bo giua callback ESP-NOW va loop() cua LVGL
static volatile bool g_hasNewTelemetry = false;
static TelemetryPacket g_rxPacket = {};
static uint32_t g_lastRxMs = 0;

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

// -------------------- Xu ly ESP-NOW (T06 - Core 2.x) --------------------
void onEspNowRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  LV_UNUSED(mac);
  if (len == (int)sizeof(TelemetryPacket)) {
    memcpy((void *)&g_rxPacket, incomingData, sizeof(TelemetryPacket));
    g_rxPacket.alertMsg[sizeof(g_rxPacket.alertMsg) - 1] = '\0';
    g_hasNewTelemetry = true;
    g_lastRxMs = millis();
  }
}

void onEspNowSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  LV_UNUSED(mac_addr);
  Serial.printf("[ESP-NOW] Send status: %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void onUserToggleRelay(bool newState) {
  Serial.printf("[UI] Relay toggled -> %s\n", newState ? "ON" : "OFF");
#if !USE_MOCK_DATA
  ControlPacket cmd = {};
  cmd.cmdType = 1;
  cmd.relayTarget = newState;
  esp_now_send(S3_HUB_MAC, (uint8_t *)&cmd, sizeof(cmd));
#endif
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  Serial.print("[ESP-NOW] Panel MAC: ");
  Serial.println(WiFi.macAddress());

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed!");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, S3_HUB_MAC, 6);
  peerInfo.channel = ESPNOW_WIFI_CHANNEL;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

// -------------------- Setup & Loop --------------------
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

  ui_panel_init(onUserToggleRelay, USE_MOCK_DATA == 1);

#if USE_MOCK_DATA
  ui_panel_set_connection(true, true);
  ui_panel_update_sensors(28.5f, 65.0f, 420, false);
  Serial.println("LVGL Panel Ready (MOCK MODE)");
#else
  initEspNow();
  ui_panel_set_connection(false, false);
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
    ui_panel_update_sensors(mockTemp, 62.5f, 380 + (int)(mockTemp * 5), mockTemp > 33.0f);
  }
#else
  if (g_hasNewTelemetry) {
    g_hasNewTelemetry = false;
    TelemetryPacket pkt;
    memcpy(&pkt, (const void *)&g_rxPacket, sizeof(pkt));

    ui_panel_set_connection(true, false);
    ui_panel_update_sensors(pkt.temperature, pkt.humidity, pkt.lightLux, pkt.relayState);
    if (pkt.hasAlert && pkt.alertMsg[0] != '\0') {
      ui_panel_show_alert("SYSTEM ALERT", pkt.alertMsg);
    }
  }
  if (g_lastRxMs > 0 && (millis() - g_lastRxMs > 10000)) {
    ui_panel_set_connection(false, false);
  }
#endif

  lv_timer_handler();
  delay(5);
}