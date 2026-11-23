TẠO FOLDER local_gateway_ai và coreiot_configs RỒI LÀM TRONG ĐÓ


ESP32C3 gắn module DHT11, truyền qua ESP32 WROOM qua BLE
ESP32 WROOM nhận data BLE từ C3, gắn thêm cảm biến ánh sáng, relay 1 kênh và button điều khiển relay này, WROOM truyền qua yolo uno thông qua ESPNOW
YOLO Uno chạy website, kết nối wifi nhà + truyền lên broker (đã thêm trang config) - Cơ chế gần giống Tasmota, khi chạy sẽ phát ra một wifi (pass từ 1-8), vô trang này để kết nối WIfi (2.4Ghz), sau khi kết nối sẽ dùng mạng nhà để chạy server (192.168.1.70), nếu có broker sẽ nhảy data lên broker ko thì thôi
Tất cả đều dùng RTOS
