#include <Arduino.h>
#include <esp_random.h>
#include <NimBLEDevice.h>
#include <DHT.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "ble_protocol.h"

// ==================== CẤU HÌNH PHẦN CỨNG (ESP32-S3 N16R8) ====================
// 1 = Sinh dữ liệu ngẫu nhiên (chưa có cảm biến)
// 0 = Đọc từ cảm biến DHT11 thật ở GPIO 4
#define USE_MOCK_SENSOR  0

#define PIN_DHT          4      // Chân DATA của DHT11 nối vào GPIO 4
#define DHT_TYPE         DHT11
#define PIN_RGB_LED      48     // LED RGB tích hợp trên ESP32-S3 N16R8 (nếu không sáng đổi thành 38)
#define PIN_OUT_LOW      5      // Chân xuất mức LOW (chuyển từ GPIO 3 sang GPIO 5)

const uint32_t UPDATE_INTERVAL_MS = 3000;

// ==================== CẤU TRÚC DỮ LIỆU & IPC FREERTOS ====================
struct EnvReading {
  float temp;
  float hum;
};

QueueHandle_t     xSensorQueue = nullptr;
SemaphoreHandle_t xLedPulseSem = nullptr;

// ==================== ĐỐI TƯỢNG TOÀN CỤC ====================
DHT dht(PIN_DHT, DHT_TYPE);
NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pEnvChar = nullptr;

volatile bool deviceConnected = false;

float simTemp = 28.5f;
float simHum  = 65.0f;

void setStatusLed(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(PIN_RGB_LED, r, g, b);
}

// ==================== CALLBACK KẾT NỐI BLE (NimBLE 2.x) ====================
class EnvServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.printf("[BLE] Client %s da ket noi!\n", connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.printf("[BLE] Client ngat ket noi (reason=%d) -> Phat lai Advertising...\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

static EnvServerCallbacks serverCallbacks;

// ==================== KHỞI TẠO BLE SERVER ====================
void initBleServer() {
  NimBLEDevice::init(BLE_DEVICE_NAME);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);

  NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);
  pEnvChar = pService->createCharacteristic(
    BLE_CHAR_ENV_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  pEnvChar->setValue("28.5,65.0");
  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName(BLE_DEVICE_NAME);
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->enableScanResponse(true);
  pAdvertising->start();

  Serial.printf("[BLE] Dang quang ba '%s' (MAC: %s)\n",
                BLE_DEVICE_NAME,
                NimBLEDevice::getAddress().toString().c_str());
}

// ==================== ĐỌC / SINH DỮ LIỆU CẢM BIẾN ====================
bool readEnvData(EnvReading& out) {
#if USE_MOCK_SENSOR
  float deltaTemp = random(-6, 7) / 10.0f;
  float deltaHum  = random(-15, 16) / 10.0f;

  simTemp = constrain(simTemp + deltaTemp, 25.0f, 36.0f);
  simHum  = constrain(simHum + deltaHum, 50.0f, 85.0f);

  out.temp = simTemp;
  out.hum  = simHum;
  return true;
#else
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    Serial.println("[DHT11] Loi doc cam bien! Kiem tra day DATA (GPIO 4) va nguon 3.3V.");
    return false;
  }
  out.temp = t;
  out.hum  = h;
  return true;
#endif
}

// ==================== TASK 1: ĐỌC CẢM BIẾN ĐỊNH KỲ (ƯU TIÊN 2) ====================
void TaskSensor(void* pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  EnvReading reading;

  for (;;) {
    if (readEnvData(reading)) {
      xQueueOverwrite(xSensorQueue, &reading);
    }
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(UPDATE_INTERVAL_MS));
  }
}

// ==================== TASK 2: CẬP NHẬT & NOTIFY BLE (ƯU TIÊN 3) ====================
void TaskBleNotify(void* pvParameters) {
  EnvReading reading;

  for (;;) {
    if (xQueueReceive(xSensorQueue, &reading, portMAX_DELAY) == pdTRUE) {
      char payload[24];
      snprintf(payload, sizeof(payload), "%.1f,%.1f", reading.temp, reading.hum);

      pEnvChar->setValue((const uint8_t*)payload, strlen(payload));

      bool connected = deviceConnected;
      if (connected) {
        pEnvChar->notify();
        xSemaphoreGive(xLedPulseSem);
      }

      Serial.printf("[BLE %s] Temp: %.1f C | Hum: %.1f %% | Payload: '%s' (%s)\n",
                    USE_MOCK_SENSOR ? "MOCK" : "DHT11",
                    reading.temp, reading.hum, payload,
                    connected ? "Da Notify" : "Cho ket noi");
    }
  }
}

// ==================== TASK 3: LED TRẠNG THÁI KHÔNG CHẶN (ƯU TIÊN 1) ====================
void TaskStatusLed(void* pvParameters) {
  for (;;) {
    if (deviceConnected) {
      if (xSemaphoreTake(xLedPulseSem, pdMS_TO_TICKS(200)) == pdTRUE) {
        setStatusLed(0, 25, 0);
        vTaskDelay(pdMS_TO_TICKS(40));
        setStatusLed(0, 0, 0);
      }
    } else {
      setStatusLed(0, 0, 25);
      vTaskDelay(pdMS_TO_TICKS(80));
      setStatusLed(0, 0, 0);

      for (int i = 0; i < 14 && !deviceConnected; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      xSemaphoreTake(xLedPulseSem, 0);
    }
  }
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(1500);

  randomSeed(esp_random());

  setStatusLed(0, 0, 0);

  pinMode(PIN_OUT_LOW, OUTPUT);
  digitalWrite(PIN_OUT_LOW, LOW);

#if !USE_MOCK_SENSOR
  dht.begin();
#endif

  xSensorQueue = xQueueCreate(1, sizeof(EnvReading));
  xLedPulseSem = xSemaphoreCreateBinary();

  initBleServer();

  xTaskCreate(TaskBleNotify, "BleNotify", 4096, nullptr, 3, nullptr);
  xTaskCreate(TaskSensor,    "Sensor",    3072, nullptr, 2, nullptr);
  xTaskCreate(TaskStatusLed, "StatusLed", 2048, nullptr, 1, nullptr);

  Serial.printf("[SYSTEM] ESP32-S3 FreeRTOS BLE Server san sang! (Che do: %s)\n",
                USE_MOCK_SENSOR ? "MOCK RANDOM" : "DHT11 THAT");
}

void loop() {
  vTaskDelete(NULL);
}