#pragma once
#include <stdint.h>

// Định danh từng node trong mạng ESP-NOW
enum NodeId : uint8_t {
  NODE_S3_HUB     = 1, // ESP32-S3 (YoloUNO) điều phối Layer 1
  NODE_WROOM_ROOM = 2, // ESP32-WROOM cảm biến ánh sáng, 1 relay đèn, 1 nút bấm & đọc BLE
  NODE_LVGL_PANEL = 3  // ESP32 + Màn hình cảm ứng 2.8" (N03)
};

// Loại gói tin
enum MsgType : uint8_t {
  MSG_TELEMETRY  = 1, // WROOM -> S3: dữ liệu cảm biến, kết nối BLE & trạng thái đèn
  MSG_COMMAND    = 2, // LVGL/Gateway -> S3 -> WROOM: lệnh bật/tắt đèn hoặc Away Mode
  MSG_STATE_SYNC = 3, // S3 -> LVGL: đồng bộ trạng thái tổng từ WROOM / LAN UI / CoreIOT
  MSG_ALERT      = 4  // S3 -> LVGL: cảnh báo khẩn từ Edge AI / RuleChain
};

// Mã cảnh báo hiển thị pop-up trên LVGL và Local Web T07
enum AlertCode : uint8_t {
  ALERT_NONE            = 0,
  ALERT_INTRUSION       = 1, // AI phát hiện người lạ khi bật Away Mode
  ALERT_FIRE            = 2, // AI phát hiện cháy/khói
  ALERT_TEMP_HIGH       = 3, // Nhiệt độ vượt ngưỡng
  ALERT_GATEWAY_OFFLINE = 4  // Mất kết nối Local Gateway
};

// Bitmask công tắc (chỉ có 1 relay đèn phòng ngủ, vẫn giữ dạng bitmask để dễ mở rộng)
#define SW_BED_LIGHT_BIT  (1 << 0) // sw1_bed_light (Relay đèn phòng ngủ)

typedef struct __attribute__((packed)) {
  uint8_t   node_id;        // Nguồn gửi (enum NodeId) - 1 byte
  uint8_t   msg_type;       // Loại gói tin (enum MsgType) - 1 byte
  uint16_t  seq;            // Số thứ tự gói tin (đo packet loss) - 2 bytes

  // Dữ liệu môi trường
  float     temperature;    // °C (đọc qua BLE từ ESP32-C3 Mini) - 4 bytes
  float     humidity;       // %RH (đọc qua BLE từ ESP32-C3 Mini) - 4 bytes
  uint16_t  light_lux;      // Độ sáng quy đổi 0 - 1000 từ MH-Sensor - 2 bytes

  // Trạng thái điều khiển
  uint8_t   switch_mask;    // Bitmask công tắc (dùng SW_BED_LIGHT_BIT) - 1 byte
  uint8_t   away_mode;      // 0 = Home, 1 = Away - 1 byte

  // Cảnh báo từ Gateway / Edge AI truyền ngược về Layer 1
  uint8_t   alert_code;     // enum AlertCode - 1 byte
  char      alert_msg[32];  // Chuỗi ngắn không dấu hiển thị trên LVGL - 32 bytes

  // Trạng thái kết nối 2 tầng không dây Layer 1
  uint8_t   espnow_online;  // 1 = WROOM đang online, 0 = S3 báo WROOM mất kết nối (>10s) - 1 byte
  uint8_t   ble_online;     // 1 = node C3 đang online, 0 = mất kết nối BLE - 1 byte
} EspNowPacket;             // Tổng kích thước thực tế: 51 bytes