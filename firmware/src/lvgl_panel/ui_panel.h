#pragma once
#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>

typedef void (*RelayToggleCallback)(bool newState);

void ui_panel_init(RelayToggleCallback relayCb, bool showMockAlertBtn);
void ui_panel_set_connection(bool connected, bool isMock);
void ui_panel_update_sensors(float temp, float humi, int lightLux, bool relayState);
void ui_panel_show_alert(const char *title, const char *message);
void ui_panel_hide_alert();