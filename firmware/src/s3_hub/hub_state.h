#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "espnow_protocol.h"

// ==================== CẤU HÌNH PHẦN CỨNG & MẠNG ====================
#define NEO_PIN          48    // Đèn RGB NeoPixel tích hợp trên YoloUNO (ESP32-S3)
#define LED_COUNT        1
#define NEO_BRIGHTNESS   60    // Độ sáng 0-255
#define PIN_BOOT_BTN     0     // Nút BOOT tích hợp (nhấn giữ 5s để reset NVS)

#define FALLBACK_CHANNEL  1
#define FALLBACK_AP_SSID  "YoloUNO_Offline"
#define FALLBACK_AP_PASS  "12345678"
#define DEFAULT_MQTT_PORT 1883

#define ESPNOW_TIMEOUT_MS       10000 // Quá 10s không có gói từ WROOM -> Offline
#define HISTORY_MAX_POINTS      30    // Lưu 30 mẫu gần nhất cho đồ thị offline
#define HISTORY_MIN_INTERVAL_MS 2000

#define FW_VERSION "1.3.1-ws-prov"

// ==================== 5 TOPIC LOCAL MQTT (HỢP ĐỒNG 2) ====================
extern const char* TOPIC_TELEMETRY;
extern const char* TOPIC_HUB_STATUS;
extern const char* TOPIC_CMD_SWITCH;
extern const char* TOPIC_CMD_MODE;
extern const char* TOPIC_AI_ALERT;

// ==================== ENUMS & STRUCTS DÙNG CHUNG ====================
enum NetState : uint8_t {
  NET_INIT             = 0, // Đỏ: Đang khởi tạo / đang kết nối Wi-Fi
  NET_ONLINE           = 1, // Xanh lá: Wi-Fi OK + MQTT OK
  NET_MQTT_OFFLINE_LAN = 2, // Tím: Wi-Fi LAN OK, MQTT chưa bật/lỗi
  NET_WIFI_OFFLINE_AP  = 3  // Xanh dương: Mất Wi-Fi / Chưa cấu hình -> AP kênh 1
};

enum EspNowTxAction : uint8_t {
  TX_CMD_WROOM  = 1,
  TX_SYNC_LVGL  = 2,
  TX_ALERT_LVGL = 3
};

enum WifiProvState : uint8_t {
  PROV_IDLE       = 0,
  PROV_PENDING    = 1, // Chờ 500ms để trình duyệt tải xong saved.html
  PROV_CONNECTING = 2, // Đang nối router (giữ song song SoftAP)
  PROV_SUCCESS    = 3, // Đã có IP -> giữ AP thêm 5s cho người dùng xem IP
  PROV_FAILED     = 4  // Sai mật khẩu -> báo lỗi 8s rồi trả về FSM thường
};

struct AppConfig {
  char     wifi_ssid[33];
  char     wifi_pass[65];
  char     mqtt_host[65];
  uint16_t mqtt_port;
  char     mqtt_user[33];
  char     mqtt_pass[65];
};

struct EspNowRxItem {
  uint8_t mac[6];
  EspNowPacket pkt;
};

struct SystemState {
  float    temperature   = 0.0f;
  float    humidity      = 0.0f;
  uint16_t light_lux     = 0;
  uint8_t  switch_mask   = 0;
  bool     away_mode     = false;
  bool     espnow_online = false;
  bool     ble_online    = false;
  uint8_t  alert_code    = ALERT_NONE;
  char     alert_msg[32] = "OK";
  uint16_t last_rx_seq   = 0;
  uint32_t last_rx_ms    = 0;
};

struct HistoryPoint {
  float temp;
  float hum;
};

// ==================== BIẾN DÙNG CHUNG GIỮA CÁC MODULE ====================
extern SemaphoreHandle_t xStateMutex;
extern SemaphoreHandle_t xConfigMutex;
extern SemaphoreHandle_t xMqttPubTelemetrySem;
extern SemaphoreHandle_t xWsBroadcastSem;
extern QueueHandle_t     xEspNowRxQueue;
extern QueueHandle_t     xEspNowTxQueue;

extern volatile NetState currentNetState;
extern SystemState       sysState;
extern HistoryPoint      envHistory[HISTORY_MAX_POINTS];
extern uint8_t           historyHead;
extern uint8_t           historyCount;

extern volatile bool          mqttConfigDirty;
extern volatile bool          pendingReboot;
extern uint32_t               pendingRebootAtMs;
extern volatile WifiProvState wifiProvState;
extern volatile bool          provIpDelivered;
extern volatile uint32_t      apShutdownAtMs;
extern uint32_t               provStartAtMs;
extern char                   provAssignedIp[20];

// ==================== HÀM TIỆN ÍCH & QUẢN LÝ NVS ====================
void        initHubIpc();
const char* netStateToStr(NetState st);
void        setNetState(NetState nextState);
void        notifyWebClients();
void        queueEspNowAction(EspNowTxAction action);
void        printMac(const uint8_t* mac);

// Chỉ gọi khi ĐÃ giữ xStateMutex
void        pushEnvHistoryLocked(float temp, float hum);
SystemState getStateSnapshot();

AppConfig   getConfigSnapshot();
void        loadConfigFromNvs();
void        saveConfigToNvs(const AppConfig& cfg);
void        clearConfigNvs();

bool        applyToggleFromUi();
bool        applyAwayFromUi();