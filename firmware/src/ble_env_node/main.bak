#include <Arduino.h>
#include <esp_random.h>
#include <NimBLEDevice.h>
#include <DHT.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "ble_protocol.h"

// ==================== CẤU HÌNH PHẦN CỨNG ====================
// 1 = Sinh dữ liệu ngẫu nhiên (chưa có cảm biến)
// 0 = Đọc từ cảm biến DHT11 thật ở GPIO 4
#define USE_MOCK_SENSOR  0

#define PIN_DHT          4      // Chân DATA của DHT11 nối vào GPIO 4
#define DHT_TYPE         DHT11
#define PIN_LED_STATUS   8      // LED tích hợp trên ESP32-C3 SuperMini (Active LOW)

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

// Giá trị nền cho bộ sinh ngẫu nhiên (random walk)
float simTemp = 28.5f;
float simHum  = 65.0f;

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
      // Ghi đè luôn giữ mẫu mới nhất trong Queue
      xQueueOverwrite(xSensorQueue, &reading);
    }

    // Giữ chu kỳ chuẩn xác 3000ms không bị trôi thời gian
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(UPDATE_INTERVAL_MS));
  }
}

// ==================== TASK 2: CẬP NHẬT & NOTIFY BLE (ƯU TIÊN 3) ====================
void TaskBleNotify(void* pvParameters) {
  EnvReading reading;

  for (;;) {
    // Ngủ hoàn toàn cho tới khi TaskSensor gửi mẫu đo mới
    if (xQueueReceive(xSensorQueue, &reading, portMAX_DELAY) == pdTRUE) {
      char payload[24];
      snprintf(payload, sizeof(payload), "%.1f,%.1f", reading.temp, reading.hum);

      pEnvChar->setValue((const uint8_t*)payload, strlen(payload));

      bool connected = deviceConnected;
      if (connected) {
        pEnvChar->notify();
        xSemaphoreGive(xLedPulseSem); // Nháy LED báo đã gửi Notify
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
      // Khi đã kết nối WROOM: chỉ nháy ngắn 40ms mỗi lần gửi Notify thành công
      if (xSemaphoreTake(xLedPulseSem, pdMS_TO_TICKS(200)) == pdTRUE) {
        digitalWrite(PIN_LED_STATUS, LOW);
        vTaskDelay(pdMS_TO_TICKS(40));
        digitalWrite(PIN_LED_STATUS, HIGH);
      }
    } else {
      // Khi đang chờ kết nối (Advertising): nháy nhịp chậm 1.5s/lần để dễ nhận biết bằng mắt
      digitalWrite(PIN_LED_STATUS, LOW);
      vTaskDelay(pdMS_TO_TICKS(80));
      digitalWrite(PIN_LED_STATUS, HIGH);

      // Chia nhỏ thời gian ngủ để phản ứng nhanh ngay khi WROOM vừa kết nối lại
      for (int i = 0; i < 14 && !deviceConnected; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      xSemaphoreTake(xLedPulseSem, 0); // Xóa cờ thừa nếu có
    }
  }
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(1500);

  randomSeed(esp_random());

  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_LED_STATUS, HIGH); // Tắt LED (Active LOW)

  pinMode(3, OUTPUT);
  digitalWrite(3, LOW); // Tắt LED (Active LOW)


#if !USE_MOCK_SENSOR
  dht.begin();
#endif

  // Tạo IPC trước khi khởi chạy BLE và Task
  xSensorQueue = xQueueCreate(1, sizeof(EnvReading));
  xLedPulseSem = xSemaphoreCreateBinary();

  // Khởi tạo BLE Server trước để đảm bảo pEnvChar đã sẵn sàng
  initBleServer();

  // ESP32-C3 là Single-Core -> dùng xTaskCreate (tự chạy trên Core 0)
  xTaskCreate(TaskBleNotify, "BleNotify", 4096, nullptr, 3, nullptr);
  xTaskCreate(TaskSensor,    "Sensor",    3072, nullptr, 2, nullptr);
  xTaskCreate(TaskStatusLed, "StatusLed", 2048, nullptr, 1, nullptr);

  Serial.printf("[SYSTEM] ESP32-C3 FreeRTOS BLE Server san sang! (Che do: %s)\n",
                USE_MOCK_SENSOR ? "MOCK RANDOM" : "DHT11 THAT");
}

void loop() {
  // Thu hồi ~8KB RAM của loopTask mặc định
  vTaskDelete(NULL);
}