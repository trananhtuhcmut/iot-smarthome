#include "ui_panel.h"
#include <stdio.h>

#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
  #define FONT_VALUE (&lv_font_montserrat_20)
#else
  #define FONT_VALUE LV_FONT_DEFAULT
#endif

static lv_obj_t *lbl_conn = NULL;
static lv_obj_t *lbl_temp = NULL;
static lv_obj_t *lbl_humi = NULL;
static lv_obj_t *lbl_light = NULL;
static lv_obj_t *sw_relay = NULL;
static lv_obj_t *lbl_relay_state = NULL;

static lv_obj_t *alert_backdrop = NULL;
static lv_obj_t *lbl_alert_title = NULL;
static lv_obj_t *lbl_alert_msg = NULL;

static RelayToggleCallback g_relayCb = NULL;
static bool g_ignoreRelayEvent = false;

static void on_relay_changed(lv_event_t *e) {
  if (g_ignoreRelayEvent) return;
  lv_obj_t *sw = lv_event_get_target(e);
  bool state = lv_obj_has_state(sw, LV_STATE_CHECKED);

  lv_label_set_text(lbl_relay_state, state ? "STATE: ON" : "STATE: OFF");
  lv_obj_set_style_text_color(lbl_relay_state,
                              lv_color_hex(state ? 0x22C55E : 0x94A3B8), 0);

  if (g_relayCb) g_relayCb(state);
}

static void on_alert_close(lv_event_t *e) {
  LV_UNUSED(e);
  ui_panel_hide_alert();
}

static void on_test_alert_btn(lv_event_t *e) {
  LV_UNUSED(e);
  ui_panel_show_alert("Temperature alert", "Temperature exceeded threshold 35.0 C!\nRelay/fan has been activated.");
}

static lv_obj_t *create_sensor_card(lv_obj_t *parent, int y, const char *title,
                                    uint32_t accentHex, lv_obj_t **outValueLbl) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 162, 60);
  lv_obj_set_pos(card, 10, y);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(accentHex), 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_all(card, 6, 0);

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

void ui_panel_init(RelayToggleCallback relayCb, bool showMockAlertBtn) {
  g_relayCb = relayCb;

  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F172A), 0);

  // 1. Thanh trang thai tren cung (320x32)
  lv_obj_t *header = lv_obj_create(scr);
  lv_obj_set_size(header, 320, 32);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_pad_hor(header, 10, 0);

  lv_obj_t *lblTitle = lv_label_create(header);
  lv_label_set_text(lblTitle, "SMARTHOME N03");
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(lblTitle, LV_ALIGN_LEFT_MID, 0, 0);

  lbl_conn = lv_label_create(header);
  lv_label_set_text(lbl_conn, "INIT...");
  lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xFACC15), 0);
  lv_obj_align(lbl_conn, LV_ALIGN_RIGHT_MID, 0, 0);

  // 2. Cot trai: 3 the cam bien
  create_sensor_card(scr, 40,  "TEMP",  0xF97316, &lbl_temp);
  create_sensor_card(scr, 106, "HUMI",  0x06B6D4, &lbl_humi);
  create_sensor_card(scr, 172, "LIGHT", 0xEAB308, &lbl_light);

  // 3. Cot phai: The dieu khien Relay
  lv_obj_t *ctrlCard = lv_obj_create(scr);
  lv_obj_set_size(ctrlCard, 130, showMockAlertBtn ? 136 : 192);
  lv_obj_set_pos(ctrlCard, 180, 40);
  lv_obj_clear_flag(ctrlCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(ctrlCard, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_border_color(ctrlCard, lv_color_hex(0x334155), 0);
  lv_obj_set_style_border_width(ctrlCard, 2, 0);
  lv_obj_set_style_radius(ctrlCard, 10, 0);

  lv_obj_t *lblCtrl = lv_label_create(ctrlCard);
  lv_label_set_text(lblCtrl, "RELAY CONTROL");
  lv_obj_set_style_text_color(lblCtrl, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(lblCtrl, LV_ALIGN_TOP_MID, 0, 4);

  sw_relay = lv_switch_create(ctrlCard);
  lv_obj_set_size(sw_relay, 76, 38);
  lv_obj_align(sw_relay, LV_ALIGN_CENTER, 0, 2);
  lv_obj_set_style_bg_color(sw_relay, lv_color_hex(0x22C55E), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw_relay, on_relay_changed, LV_EVENT_VALUE_CHANGED, NULL);

  lbl_relay_state = lv_label_create(ctrlCard);
  lv_label_set_text(lbl_relay_state, "STATE: OFF");
  lv_obj_set_style_text_color(lbl_relay_state, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(lbl_relay_state, LV_ALIGN_BOTTOM_MID, 0, -4);

  // Nut test Pop-up (chi hien khi bat che do Mock)
  if (showMockAlertBtn) {
    lv_obj_t *btnTest = lv_btn_create(scr);
    lv_obj_set_size(btnTest, 130, 46);
    lv_obj_set_pos(btnTest, 180, 186);
    lv_obj_set_style_bg_color(btnTest, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_radius(btnTest, 10, 0);
    lv_obj_add_event_cb(btnTest, on_test_alert_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lblBtn = lv_label_create(btnTest);
    lv_label_set_text(lblBtn, "TEST ALERT");
    lv_obj_center(lblBtn);
  }

  // 4. Pop-up canh bao (an mac dinh, phu len toan man hinh khi kich hoat)
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
  lv_label_set_text(lblClose, "DONG / OK");
  lv_obj_center(lblClose);

  lv_obj_add_flag(alert_backdrop, LV_OBJ_FLAG_HIDDEN);
}

void ui_panel_set_connection(bool connected, bool isMock) {
  if (isMock) {
    lv_label_set_text(lbl_conn, "MOCK MODE");
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xFACC15), 0);
  } else if (connected) {
    lv_label_set_text(lbl_conn, "ESP-NOW OK");
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0x22C55E), 0);
  } else {
    lv_label_set_text(lbl_conn, "OFFLINE");
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0xEF4444), 0);
  }
}

void ui_panel_update_sensors(float temp, float humi, int lightLux, bool relayState) {
  char buf[32];

  snprintf(buf, sizeof(buf), "%.1f C", temp);
  lv_label_set_text(lbl_temp, buf);

  snprintf(buf, sizeof(buf), "%.1f %%", humi);
  lv_label_set_text(lbl_humi, buf);

  snprintf(buf, sizeof(buf), "%d lux", lightLux);
  lv_label_set_text(lbl_light, buf);

  g_ignoreRelayEvent = true;
  if (relayState) lv_obj_add_state(sw_relay, LV_STATE_CHECKED);
  else            lv_obj_clear_state(sw_relay, LV_STATE_CHECKED);
  lv_label_set_text(lbl_relay_state, relayState ? "STATE: ON" : "STATE: OFF");
  lv_obj_set_style_text_color(lbl_relay_state,
                              lv_color_hex(relayState ? 0x22C55E : 0x94A3B8), 0);
  g_ignoreRelayEvent = false;
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