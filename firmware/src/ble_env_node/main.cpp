#include <Arduino.h>
#include <esp_random.h>
#include <NimBLEDevice.h>
#include <DHT.h>
#include "ble_protocol.h"

// ==================== CẤU HÌNH PHẦN CỨNG ====================
// 1 = Sinh dữ liệu ngẫu nhiên (chưa có cảm biến)
// 0 = Đọc từ cảm biến DHT11 thật ở GPIO 4
#define USE_MOCK_SENSOR  1

#define PIN_DHT          4      // Chân DATA của DHT11 nối vào GPIO 4
#define DHT_TYPE         DHT11
#define PIN_LED_STATUS   8      // LED tích hợp trên ESP32-C3 SuperMini (Active LOW)

const unsigned long UPDATE_INTERVAL_MS = 3000;

// ==================== BIẾN TOÀN CỤC ====================
DHT dht(PIN_DHT, DHT_TYPE);
NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pEnvChar = nullptr;

bool deviceConnected = false;
unsigned long lastUpdateTime = 0;

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

// ==================== ĐỌC / SINH DỮ LIỆU CẢM BIẾN ====================
bool getEnvData(float& temp, float& hum) {
#if USE_MOCK_SENSOR
  // Mỗi chu kỳ dao động ngẫu nhiên trong khoảng [-0.6, +0.6] °C và [-1.5, +1.5] %RH
  float deltaTemp = random(-6, 7) / 10.0f;
  float deltaHum  = random(-15, 16) / 10.0f;

  simTemp = constrain(simTemp + deltaTemp, 25.0f, 36.0f);
  simHum  = constrain(simHum + deltaHum, 50.0f, 85.0f);

  temp = simTemp;
  hum  = simHum;
  return true;
#else
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    Serial.println("[DHT11] Loi doc cam bien! Kiem tra day DATA (GPIO 4) va nguon 3.3V.");
    return false;
  }
  temp = t;
  hum  = h;
  return true;
#endif
}

// ==================== KHỞI TẠO BLE SERVER ====================
void initBleServer() {
  NimBLEDevice::init(BLE_DEVICE_NAME);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new EnvServerCallbacks());

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

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  delay(1500);

  // Khởi tạo seed ngẫu nhiên bằng bộ sinh số phần cứng của ESP32-C3
  randomSeed(esp_random());

  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_LED_STATUS, HIGH); // Tắt LED (Active LOW)

#if !USE_MOCK_SENSOR
  dht.begin();
#endif

  initBleServer();
  Serial.printf("[SYSTEM] ESP32-C3 Mini BLE Server san sang! (Che do: %s)\n",
                USE_MOCK_SENSOR ? "MOCK RANDOM" : "DHT11 THAT");
}

void loop() {
  if (millis() - lastUpdateTime >= UPDATE_INTERVAL_MS) {
    lastUpdateTime = millis();

    float temp = 0.0f, hum = 0.0f;
    if (!getEnvData(temp, hum)) {
      return;
    }

    char payload[24];
    snprintf(payload, sizeof(payload), "%.1f,%.1f", temp, hum);

    pEnvChar->setValue((const uint8_t*)payload, strlen(payload));

    if (deviceConnected) {
      pEnvChar->notify();
      digitalWrite(PIN_LED_STATUS, LOW);
      delay(30);
      digitalWrite(PIN_LED_STATUS, HIGH);
    }

    Serial.printf("[BLE %s] Temp: %.1f C | Hum: %.1f %% | Payload: '%s' (%s)\n",
                  USE_MOCK_SENSOR ? "MOCK" : "DHT11",
                  temp, hum, payload,
                  deviceConnected ? "Da Notify" : "Cho ket noi");
  }
}