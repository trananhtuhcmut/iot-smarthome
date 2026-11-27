#include "ui_panel.h"
#include <stdio.h>
#include <math.h>

#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
  #define FONT_VALUE (&lv_font_montserrat_20)
#else
  #define FONT_VALUE LV_FONT_DEFAULT
#endif

static lv_obj_t *lbl_conn = NULL;
static lv_obj_t *lbl_temp = NULL;
static lv_obj_t *lbl_humi = NULL;
static lv_obj_t *lbl_light = NULL;

// 2 the trang thai pill (ESP-NOW & BLE C3)
static lv_obj_t *dot_espnow = NULL;
static lv_obj_t *lbl_espnow_st = NULL;
static lv_obj_t *dot_ble = NULL;
static lv_obj_t *lbl_ble_st = NULL;

static lv_obj_t *sw_relay = NULL;
static lv_obj_t *lbl_relay_state = NULL;
static lv_obj_t *sw_away = NULL;
static lv_obj_t *lbl_away_state = NULL;

// Pop-up canh bao
static lv_obj_t *alert_backdrop = NULL;
static lv_obj_t *lbl_alert_title = NULL;
static lv_obj_t *lbl_alert_msg = NULL;

// Pop-up Network & System Info
static lv_obj_t *info_backdrop = NULL;
static lv_obj_t *lbl_info_net = NULL;
static lv_obj_t *lbl_info_wifi = NULL;
static lv_obj_t *lbl_info_mac = NULL;
static lv_obj_t *lbl_info_hub = NULL;
static lv_obj_t *lbl_info_last = NULL;

static RelayToggleCallback g_relayCb = NULL;
static AwayModeToggleCallback g_awayCb = NULL;
static bool g_ignoreUiEvent = false;

static void update_relay_label(bool state) {
  lv_label_set_text(lbl_relay_state, state ? "STATE: ON" : "STATE: OFF");
  lv_obj_set_style_text_color(lbl_relay_state,
                              lv_color_hex(state ? 0x22C55E : 0x94A3B8), 0);
}

static void update_away_label(bool away) {
  lv_label_set_text(lbl_away_state, away ? "MODE: AWAY" : "MODE: HOME");
  lv_obj_set_style_text_color(lbl_away_state,
                              lv_color_hex(away ? 0xF59E0B : 0x94A3B8), 0);
}

static void set_status_pill(lv_obj_t *dot, lv_obj_t *lbl, bool online) {
  lv_obj_set_style_bg_color(dot, lv_color_hex(online ? 0x22C55E : 0xEF4444), 0);
  lv_label_set_text(lbl, online ? "Online" : "Offline");
  lv_obj_set_style_text_color(lbl, lv_color_hex(online ? 0xF8FAFC : 0x94A3B8), 0);
}

static void on_relay_changed(lv_event_t *e) {
  if (g_ignoreUiEvent) return;
  lv_obj_t *sw = lv_event_get_target(e);
  bool state = lv_obj_has_state(sw, LV_STATE_CHECKED);
  update_relay_label(state);
  if (g_relayCb) g_relayCb(state);
}

static void on_away_changed(lv_event_t *e) {
  if (g_ignoreUiEvent) return;
  lv_obj_t *sw = lv_event_get_target(e);
  bool away = lv_obj_has_state(sw, LV_STATE_CHECKED);
  update_away_label(away);
  if (g_awayCb) g_awayCb(away);
}

static void on_alert_close(lv_event_t *e) {
  LV_UNUSED(e);
  ui_panel_hide_alert();
}

static void on_info_open(lv_event_t *e) {
  LV_UNUSED(e);
  lv_obj_clear_flag(info_backdrop, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(info_backdrop);
}

static void on_info_close(lv_event_t *e) {
  LV_UNUSED(e);
  lv_obj_add_flag(info_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void on_test_alert_btn(lv_event_t *e) {
  LV_UNUSED(e);
  ui_panel_show_alert("HIGH TEMP ALERT", "Temperature exceeded 35.0 C!\nCooling relay activated.");
}

static lv_obj_t *create_status_pill(lv_obj_t *parent, int x, int y, const char *name,
                                    lv_obj_t **outDot, lv_obj_t **outLbl) {
  lv_obj_t *pill = lv_obj_create(parent);
  lv_obj_set_size(pill, 148, 24);
  lv_obj_set_pos(pill, x, y);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(0x334155), 0);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_set_style_radius(pill, 12, 0);
  lv_obj_set_style_pad_hor(pill, 8, 0);
  lv_obj_set_style_pad_ver(pill, 0, 0);

  lv_obj_t *lblName = lv_label_create(pill);
  lv_label_set_text(lblName, name);
  lv_obj_set_style_text_color(lblName, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(lblName, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *lblSt = lv_label_create(pill);
  lv_label_set_text(lblSt, "Offline");
  lv_obj_set_style_text_color(lblSt, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(lblSt, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_obj_t *dot = lv_obj_create(pill);
  lv_obj_set_size(dot, 8, 8);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0xEF4444), 0);
  lv_obj_align_to(dot, lblSt, LV_ALIGN_OUT_LEFT_MID, -5, 0);

  *outDot = dot;
  *outLbl = lblSt;
  return pill;
}

static lv_obj_t *create_sensor_card(lv_obj_t *parent, int y, const char *title,
                                    uint32_t accentHex, lv_obj_t **outValueLbl) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 162, 54);
  lv_obj_set_pos(card, 10, y);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(accentHex), 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_all(card, 5, 0);

  lv_obj_t *lblTitle = lv_label_create(card);
  lv_label_set_text(lblTitle, title);
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(lblTitle, LV_ALIGN_TOP_LEFT, 4, 0);

  lv_obj_t *lblVal = lv_label_create(card);
  lv_label_set_text(lblVal, "--");
  lv_obj_set_style_text_font(lblVal, FONT_VALUE, 0);
  lv_obj_set_style_text_color(lblVal, lv_color_hex(0xF8FAFC), 0);
  lv_obj_align(lblVal, LV_ALIGN_BOTTOM_LEFT, 4, 0);

  *outValueLbl = lblVal;
  return card;
}

static lv_obj_t *create_switch_card(lv_obj_t *parent, int y, int h, const char *title,
                                    const char *initStateText, uint32_t checkedColorHex,
                                    lv_event_cb_t cb, lv_obj_t **outSw, lv_obj_t **outStateLbl) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 130, h);
  lv_obj_set_pos(card, 180, y);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x334155), 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_all(card, 4, 0);

  lv_obj_t *lblTitle = lv_label_create(card);
  lv_label_set_text(lblTitle, title);
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(lblTitle, LV_ALIGN_TOP_MID, 0, 0);

  lv_obj_t *sw = lv_switch_create(card);
  lv_obj_set_size(sw, h > 70 ? 64 : 54, h > 70 ? 28 : 24);
  lv_obj_align(sw, LV_ALIGN_CENTER, 0, 1);
  lv_obj_set_style_bg_color(sw, lv_color_hex(checkedColorHex), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *lblState = lv_label_create(card);
  lv_label_set_text(lblState, initStateText);
  lv_obj_set_style_text_color(lblState, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(lblState, LV_ALIGN_BOTTOM_MID, 0, 0);

  *outSw = sw;
  *outStateLbl = lblState;
  return card;
}

void ui_panel_init(RelayToggleCallback relayCb, AwayModeToggleCallback awayCb, bool showMockAlertBtn) {
  g_relayCb = relayCb;
  g_awayCb  = awayCb;

  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F172A), 0);

  // 1. Thanh tieu de (320x28) + Nut INFO
  lv_obj_t *header = lv_obj_create(scr);
  lv_obj_set_size(header, 320, 28);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_pad_hor(header, 8, 0);

  lv_obj_t *lblTitle = lv_label_create(header);
  lv_label_set_text(lblTitle, "SMARTHOME");
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(lblTitle, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *btnInfo = lv_btn_create(header);
  lv_obj_set_size(btnInfo, 44, 22);
  lv_obj_align(btnInfo, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(btnInfo, lv_color_hex(0x334155), 0);
  lv_obj_set_style_radius(btnInfo, 6, 0);
  lv_obj_set_style_pad_all(btnInfo, 0, 0);
  lv_obj_add_event_cb(btnInfo, on_info_open, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lblInfoBtn = lv_label_create(btnInfo);
  lv_label_set_text(lblInfoBtn, "INFO");
  lv_obj_center(lblInfoBtn);

  lbl_conn = lv_label_create(header);
  lv_label_set_text(lbl_conn, "OFFLINE");
  lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xEF4444), 0);
  lv_obj_align_to(lbl_conn, btnInfo, LV_ALIGN_OUT_LEFT_MID, -52, 0);

  // 2. Hang pill trang thai YoloUNO: ESP-NOW & BLE (C3)
  create_status_pill(scr, 10,  32, "ESP-NOW",  &dot_espnow, &lbl_espnow_st);
  create_status_pill(scr, 162, 32, "BLE (C3)", &dot_ble,    &lbl_ble_st);

  // 3. Cot trai: 3 the cam bien
  create_sensor_card(scr, 60,  "TEMP",  0xF97316, &lbl_temp);
  create_sensor_card(scr, 120, "HUMI",  0x06B6D4, &lbl_humi);
  create_sensor_card(scr, 180, "LIGHT", 0xEAB308, &lbl_light);

  // 4. Cot phai: The Relay + The Away Mode
  int cardH = showMockAlertBtn ? 64 : 84;
  int awayY = showMockAlertBtn ? 128 : 150;

  create_switch_card(scr, 60, cardH, "BED LIGHT", "STATE: OFF", 0x22C55E,
                     on_relay_changed, &sw_relay, &lbl_relay_state);

  create_switch_card(scr, awayY, cardH, "AWAY MODE", "MODE: HOME", 0xF59E0B,
                     on_away_changed, &sw_away, &lbl_away_state);

  if (showMockAlertBtn) {
    lv_obj_t *btnTest = lv_btn_create(scr);
    lv_obj_set_size(btnTest, 130, 38);
    lv_obj_set_pos(btnTest, 180, 196);
    lv_obj_set_style_bg_color(btnTest, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_radius(btnTest, 10, 0);
    lv_obj_add_event_cb(btnTest, on_test_alert_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lblBtn = lv_label_create(btnTest);
    lv_label_set_text(lblBtn, "TEST ALERT");
    lv_obj_center(lblBtn);
  }

  // 5. Pop-up Network & System Info
  info_backdrop = lv_obj_create(scr);
  lv_obj_set_size(info_backdrop, 320, 240);
  lv_obj_set_pos(info_backdrop, 0, 0);
  lv_obj_clear_flag(info_backdrop, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(info_backdrop, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(info_backdrop, LV_OPA_70, 0);
  lv_obj_set_style_border_width(info_backdrop, 0, 0);
  lv_obj_set_style_radius(info_backdrop, 0, 0);

  lv_obj_t *infoModal = lv_obj_create(info_backdrop);
  lv_obj_set_size(infoModal, 290, 200);
  lv_obj_center(infoModal);
  lv_obj_clear_flag(infoModal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(infoModal, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_color(infoModal, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_border_width(infoModal, 2, 0);
  lv_obj_set_style_radius(infoModal, 12, 0);
  lv_obj_set_style_pad_all(infoModal, 10, 0);

  lv_obj_t *lblInfoTitle = lv_label_create(infoModal);
  lv_label_set_text(lblInfoTitle, "NETWORK & SYSTEM INFO");
  lv_obj_set_style_text_color(lblInfoTitle, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(lblInfoTitle, LV_ALIGN_TOP_MID, 0, 0);

  lbl_info_net = lv_label_create(infoModal);
  lv_label_set_text(lbl_info_net, "Net: Offline");
  lv_obj_set_style_text_color(lbl_info_net, lv_color_hex(0xF8FAFC), 0);
  lv_obj_set_pos(lbl_info_net, 0, 26);

  lbl_info_wifi = lv_label_create(infoModal);
  lv_label_set_text(lbl_info_wifi, "Wi-Fi: --  |  CH: --");
  lv_obj_set_style_text_color(lbl_info_wifi, lv_color_hex(0xCBD5E1), 0);
  lv_obj_set_pos(lbl_info_wifi, 0, 48);

  lbl_info_mac = lv_label_create(infoModal);
  lv_label_set_text(lbl_info_mac, "Panel MAC: --");
  lv_obj_set_style_text_color(lbl_info_mac, lv_color_hex(0xCBD5E1), 0);
  lv_obj_set_pos(lbl_info_mac, 0, 70);

  lbl_info_hub = lv_label_create(infoModal);
  lv_label_set_text(lbl_info_hub, "Hub MAC: --");
  lv_obj_set_style_text_color(lbl_info_hub, lv_color_hex(0xCBD5E1), 0);
  lv_obj_set_pos(lbl_info_hub, 0, 92);

  lbl_info_last = lv_label_create(infoModal);
  lv_label_set_text(lbl_info_last, "Last Data: --");
  lv_obj_set_style_text_color(lbl_info_last, lv_color_hex(0xFACC15), 0);
  lv_obj_set_pos(lbl_info_last, 0, 114);

  lv_obj_t *btnCloseInfo = lv_btn_create(infoModal);
  lv_obj_set_size(btnCloseInfo, 100, 32);
  lv_obj_align(btnCloseInfo, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(btnCloseInfo, lv_color_hex(0x0284C7), 0);
  lv_obj_add_event_cb(btnCloseInfo, on_info_close, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lblCloseInfo = lv_label_create(btnCloseInfo);
  lv_label_set_text(lblCloseInfo, "CLOSE");
  lv_obj_center(lblCloseInfo);

  lv_obj_add_flag(info_backdrop, LV_OBJ_FLAG_HIDDEN);

  // 6. Pop-up canh bao
  alert_backdrop = lv_obj_create(scr);
  lv_obj_set_size(alert_backdrop, 320, 240);
  lv_obj_set_pos(alert_backdrop, 0, 0);
  lv_obj_clear_flag(alert_backdrop, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(alert_backdrop, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(alert_backdrop, LV_OPA_70, 0);
  lv_obj_set_style_border_width(alert_backdrop, 0, 0);
  lv_obj_set_style_radius(alert_backdrop, 0, 0);

  lv_obj_t *modal = lv_obj_create(alert_backdrop);
  lv_obj_set_size(modal, 270, 165);
  lv_obj_center(modal);
  lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(modal, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_color(modal, lv_color_hex(0xEF4444), 0);
  lv_obj_set_style_border_width(modal, 3, 0);
  lv_obj_set_style_radius(modal, 12, 0);

  lbl_alert_title = lv_label_create(modal);
  lv_label_set_text(lbl_alert_title, "ALERT");
  lv_obj_set_style_text_color(lbl_alert_title, lv_color_hex(0xF87171), 0);
  lv_obj_align(lbl_alert_title, LV_ALIGN_TOP_MID, 0, 2);

  lbl_alert_msg = lv_label_create(modal);
  lv_label_set_long_mode(lbl_alert_msg, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl_alert_msg, 236);
  lv_obj_set_style_text_align(lbl_alert_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(lbl_alert_msg, lv_color_hex(0xF8FAFC), 0);
  lv_obj_align(lbl_alert_msg, LV_ALIGN_CENTER, 0, -4);

  lv_obj_t *btnClose = lv_btn_create(modal);
  lv_obj_set_size(btnClose, 110, 36);
  lv_obj_align(btnClose, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(btnClose, lv_color_hex(0xEF4444), 0);
  lv_obj_add_event_cb(btnClose, on_alert_close, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lblClose = lv_label_create(btnClose);
  lv_label_set_text(lblClose, "CLOSE / OK");
  lv_obj_center(lblClose);

  lv_obj_add_flag(alert_backdrop, LV_OBJ_FLAG_HIDDEN);
}

void ui_panel_set_net_status(NetStatus status) {
  switch (status) {
    case NET_ONLINE_MQTT:
      lv_label_set_text(lbl_conn, "ONLINE+MQTT");
      lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0x22C55E), 0);
      lv_label_set_text(lbl_info_net, "Net: Online + MQTT ON");
      lv_obj_set_style_text_color(lbl_info_net, lv_color_hex(0x22C55E), 0);
      break;
    case NET_ONLINE_NO_MQTT:
      lv_label_set_text(lbl_conn, "NO MQTT");
      lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xF59E0B), 0);
      lv_label_set_text(lbl_info_net, "Net: Online (MQTT OFF)");
      lv_obj_set_style_text_color(lbl_info_net, lv_color_hex(0xF59E0B), 0);
      break;
    case NET_MOCK:
      lv_label_set_text(lbl_conn, "MOCK MODE");
      lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xFACC15), 0);
      lv_label_set_text(lbl_info_net, "Net: Mock Mode");
      lv_obj_set_style_text_color(lbl_info_net, lv_color_hex(0xFACC15), 0);
      break;
    default:
      lv_label_set_text(lbl_conn, "OFFLINE");
      lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xEF4444), 0);
      lv_label_set_text(lbl_info_net, "Net: Offline");
      lv_obj_set_style_text_color(lbl_info_net, lv_color_hex(0xEF4444), 0);
      break;
  }
}

void ui_panel_set_sys_info(const char *wifiSsid, uint8_t channel,
                           const char *panelMac, const char *hubMac) {
  char buf[64];
  snprintf(buf, sizeof(buf), "Wi-Fi: %s  |  CH: %u",
           (wifiSsid && wifiSsid[0]) ? wifiSsid : "--", channel);
  lv_label_set_text(lbl_info_wifi, buf);

  snprintf(buf, sizeof(buf), "Panel: %s", panelMac ? panelMac : "--");
  lv_label_set_text(lbl_info_mac, buf);

  snprintf(buf, sizeof(buf), "Hub:   %s", hubMac ? hubMac : "--");
  lv_label_set_text(lbl_info_hub, buf);
}

void ui_panel_update_last_data(uint32_t secondsAgo, bool hasData) {
  if (!hasData) {
    lv_label_set_text(lbl_info_last, "Last Data: --");
    return;
  }
  char buf[40];
  if (secondsAgo < 60) {
    snprintf(buf, sizeof(buf), "Last Data: %lus ago", (unsigned long)secondsAgo);
  } else {
    snprintf(buf, sizeof(buf), "Last Data: %lum %lus ago",
             (unsigned long)(secondsAgo / 60), (unsigned long)(secondsAgo % 60));
  }
  lv_label_set_text(lbl_info_last, buf);
}

void ui_panel_clear_sensors() {
  lv_label_set_text(lbl_temp, "--");
  lv_label_set_text(lbl_humi, "--");
  lv_label_set_text(lbl_light, "--");
  set_status_pill(dot_espnow, lbl_espnow_st, false);
  set_status_pill(dot_ble, lbl_ble_st, false);
}

void ui_panel_update_sensors(float temp, float humi, int lightLux,
                             bool relayState, bool awayMode,
                             bool bleOnline, bool wroomOnline) {
  char buf[32];

  set_status_pill(dot_espnow, lbl_espnow_st, wroomOnline);
  set_status_pill(dot_ble, lbl_ble_st, bleOnline);

  if (bleOnline && !isnan(temp)) {
    snprintf(buf, sizeof(buf), "%.1f C", temp);
    lv_label_set_text(lbl_temp, buf);
  } else {
    lv_label_set_text(lbl_temp, "--");
  }

  if (bleOnline && !isnan(humi)) {
    snprintf(buf, sizeof(buf), "%.1f %%", humi);
    lv_label_set_text(lbl_humi, buf);
  } else {
    lv_label_set_text(lbl_humi, "--");
  }

  if (wroomOnline && lightLux >= 0) {
    snprintf(buf, sizeof(buf), "%d lux", lightLux);
    lv_label_set_text(lbl_light, buf);
  } else {
    lv_label_set_text(lbl_light, "--");
  }

  g_ignoreUiEvent = true;

  if (relayState) lv_obj_add_state(sw_relay, LV_STATE_CHECKED);
  else            lv_obj_clear_state(sw_relay, LV_STATE_CHECKED);
  update_relay_label(relayState);

  if (awayMode)   lv_obj_add_state(sw_away, LV_STATE_CHECKED);
  else            lv_obj_clear_state(sw_away, LV_STATE_CHECKED);
  update_away_label(awayMode);

  g_ignoreUiEvent = false;
}

void ui_panel_show_alert(const char *title, const char *message) {
  lv_label_set_text(lbl_alert_title, title);
  lv_label_set_text(lbl_alert_msg, message);
  lv_obj_clear_flag(alert_backdrop, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(alert_backdrop);
}

void ui_panel_hide_alert() {
  lv_obj_add_flag(alert_backdrop, LV_OBJ_FLAG_HIDDEN);
}