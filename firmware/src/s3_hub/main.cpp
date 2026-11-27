#include <Arduino.h>
#include <LittleFS.h>
#include "hub_state.h"
#include "task_neopixel.h"
#include "task_espnow.h"
#include "task_network.h"
#include "task_webserver.h"

void setup() {
  Serial.begin(115200);
  delay(1000);

  initHubIpc();

  currentNetState = NET_INIT;
  xTaskCreatePinnedToCore(TaskNeoPixelAndBootBtn, "NeoBoot", 2560, nullptr, 1, nullptr, 1);

  loadConfigFromNvs();
  initWiFiInitial();
  initEspNow();

  xTaskCreatePinnedToCore(TaskEspNow, "EspNowHub", 4096, nullptr, 3, nullptr, 1);

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Loi khoi tao LittleFS!");
  } else {
    Serial.println("[FS] Da mount LittleFS thanh cong!");
  }

  xTaskCreatePinnedToCore(TaskWebServer,   "WebServer", 8192, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(TaskNetworkMqtt, "NetMqtt",   6144, nullptr, 1, nullptr, 0);

  Serial.println("[SYSTEM] YoloUNO S3 Hub (Modular FreeRTOS) san sang!");
}

void loop() {
  vTaskDelete(NULL);
}