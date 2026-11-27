#include "task_neopixel.h"
#include "hub_state.h"
#include <Adafruit_NeoPixel.h>

void TaskNeoPixelAndBootBtn(void* pvParameters) {
  Adafruit_NeoPixel strip(LED_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
  strip.begin();
  strip.setBrightness(NEO_BRIGHTNESS);
  strip.clear();
  strip.show();

  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  uint8_t  lastRenderedState = 255;
  uint32_t btnHoldStartMs    = 0;
  bool     wasHoldingBtn     = false;
  bool     blinkToggle       = false;

  for (;;) {
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
      if (!wasHoldingBtn) {
        wasHoldingBtn  = true;
        btnHoldStartMs = millis();
        Serial.println("[BOOT] Dang giu nut BOOT... Giu du 5 giay de xoa cau hinh NVS!");
      }

      uint32_t heldMs = millis() - btnHoldStartMs;

      blinkToggle = !blinkToggle;
      uint32_t c = blinkToggle ? strip.Color(255, 0, 0) : strip.Color(0, 0, 0);
      for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, c);
      strip.show();

      if (heldMs >= 5000) {
        clearConfigNvs();
        for (int k = 0; k < 4; k++) {
          uint32_t wc = (k % 2 == 0) ? strip.Color(255, 255, 255) : strip.Color(0, 0, 0);
          for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, wc);
          strip.show();
          vTaskDelay(pdMS_TO_TICKS(150));
        }
        ESP.restart();
      }

      vTaskDelay(pdMS_TO_TICKS(120));
      continue;
    } else if (wasHoldingBtn) {
      wasHoldingBtn     = false;
      lastRenderedState = 255;
      Serial.println("[BOOT] Da tha nut truoc 5 giay -> Huy Factory Reset.");
    }

    NetState st = currentNetState;
    if ((uint8_t)st != lastRenderedState) {
      lastRenderedState = (uint8_t)st;

      uint32_t color = 0;
      switch (st) {
        case NET_INIT:             color = strip.Color(255, 0, 0);   break; // Đỏ
        case NET_ONLINE:           color = strip.Color(0, 255, 0);   break; // Xanh lá
        case NET_MQTT_OFFLINE_LAN: color = strip.Color(180, 0, 255); break; // Tím
        case NET_WIFI_OFFLINE_AP:  color = strip.Color(0, 0, 255);   break; // Xanh dương
      }

      for (int i = 0; i < LED_COUNT; i++) {
        strip.setPixelColor(i, color);
      }
      strip.show();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}