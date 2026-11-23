#pragma once
#include <stdint.h>

// Định danh từng node trong mạng ESP-NOW
enum NodeId : uint8_t {
  NODE_S3_HUB     = 1, // ESP32-S3 (YoloUNO) điều phối Layer 1
  NODE_WROOM_ROOM = 2, // ESP32-WROOM cảm biến ánh sáng, relay & đọc BLE
  NODE_LVGL_PANEL = 3  // ESP32 + Màn hình cảm ứng 2.8" (N03)
};

// Loại gói tin
enum MsgType : uint8_t {
  MSG_TELEMETRY  = 1, // WROOM -> S3 -> LVGL: dữ liệu cảm biến & trạng thái công tắc
  MSG_COMMAND    = 2, // LVGL/Gateway -> S3 -> WROOM: lệnh bật/tắt công tắc hoặc Away Mode
  MSG_STATE_SYNC = 3, // S3 -> LVGL: đồng bộ trạng thái tổng từ LAN UI / CoreIOT
  MSG_ALERT      = 4  // S3 -> LVGL & WROOM: cảnh báo khẩn từ Edge AI / RuleChain
};

// Mã cảnh báo hiển thị pop-up trên LVGL
enum AlertCode : uint8_t {
  ALERT_NONE            = 0,
  ALERT_INTRUSION       = 1, // AI phát hiện người lạ khi bật Away Mode
  ALERT_FIRE            = 2, // AI phát hiện cháy/khói
  ALERT_TEMP_HIGH       = 3, // Nhiệt độ vượt ngưỡng
  ALERT_GATEWAY_OFFLINE = 4  // Mất kết nối Local Gateway
};

// Bitmask công tắc (khớp với 4 key switches trong JSON MQTT)
#define SW_BED_LIGHT_BIT  (1 << 0) // sw1_bed_light (Relay phòng ngủ)
#define SW_HALL_LIGHT_BIT (1 << 1) // sw2_hall_light
#define SW_FAN_BIT        (1 << 2) // sw3_fan
#define SW_BUZZER_BIT     (1 << 3) // sw4_buzzer

typedef struct __attribute__((packed)) {
  uint8_t   node_id;        // Nguồn gửi (enum NodeId)
  uint8_t   msg_type;       // Loại gói tin (enum MsgType)
  uint16_t  seq;            // Số thứ tự gói tin (dùng đo packet loss cho Phần 4 báo cáo)

  // Dữ liệu môi trường
  float     temperature;    // °C (đọc qua BLE từ ESP32-C3 Mini)
  float     humidity;       // %RH (đọc qua BLE từ ESP32-C3 Mini)
  uint16_t  light_lux;      // Độ sáng quy đổi 0 - 1000 từ MH-Sensor

  // Trạng thái điều khiển
  uint8_t   switch_mask;    // Bitmask các công tắc
  uint8_t   away_mode;      // 0 = Home, 1 = Away

  // Cảnh báo từ Gateway / Edge AI truyền ngược về Layer 1
  uint8_t   alert_code;     // enum AlertCode
  char      alert_msg[32];  // Chuỗi ngắn không dấu hiển thị trên LVGL

  // Trạng thái kết nối nhánh BLE (T03)
  uint8_t   ble_online;     // 1 = BLE đang hoạt động, 0 = mất kết nối node C3
} EspNowPacket;             // Tổng kích thước: 51 bytes