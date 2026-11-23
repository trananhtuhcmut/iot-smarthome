#pragma once

// Tên thiết bị phát quảng bá BLE (dùng để lọc trên WROOM và app nRF Connect)
#define BLE_DEVICE_NAME   "SmartHome_BLE_Env"

// Service UUID chuẩn Bluetooth SIG cho Environmental Sensing (0x181A)
#define BLE_SERVICE_UUID  "181A"

// Characteristic UUID gửi chuỗi "temp,hum" (ví dụ: "28.5,65.2")
// Định dạng ASCII giúp app nRF Connect đọc thẳng số liệu khi chụp ảnh làm bằng chứng T03
#define BLE_CHAR_ENV_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"