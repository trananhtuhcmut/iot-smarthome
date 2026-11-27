#pragma once
#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>

typedef void (*RelayToggleCallback)(bool newState);
typedef void (*AwayModeToggleCallback)(bool awayMode);

enum NetStatus : uint8_t {
  NET_OFFLINE = 0,
  NET_ONLINE_NO_MQTT = 1,
  NET_ONLINE_MQTT = 2,
  NET_MOCK = 3
};

void ui_panel_init(RelayToggleCallback relayCb, AwayModeToggleCallback awayCb, bool showMockAlertBtn);
void ui_panel_set_net_status(NetStatus status);
void ui_panel_set_sys_info(const char *wifiSsid, uint8_t channel,
                           const char *panelMac, const char *hubMac);
void ui_panel_update_last_data(uint32_t secondsAgo, bool hasData);
void ui_panel_update_sensors(float temp, float humi, int lightLux,
                             bool relayState, bool awayMode,
                             bool bleOnline = true, bool wroomOnline = true);
void ui_panel_clear_sensors();
void ui_panel_show_alert(const char *title, const char *message);
void ui_panel_hide_alert();