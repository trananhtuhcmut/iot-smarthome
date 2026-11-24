TẠO FOLDER local_gateway_ai và coreiot_configs RỒI LÀM TRONG ĐÓ
# Autonomous Smart Home & Cloud Sync IoT System

Hệ thống Nhà thông minh & An ninh theo kiến trúc 3 tầng (**Local-First**), vận hành trên mạch **ESP32-S3 (YoloUNO)**, **Local IoT Gateway (PC + GPU)** và nền tảng **CoreIOT Cloud**. Khi mất kết nối Internet, toàn bộ cảm biến, công tắc, giao diện mạng LAN và Edge AI vẫn hoạt động bình thường; dữ liệu được lưu đệm cục bộ bằng SQLite và tự động đồng bộ bù lên CoreIOT khi có mạng trở lại.

---

## Layer 2: Local IoT Gateway, LAN Web UI & Edge AI (T08, T09, T10, T15, N04)

### 1. Cấu trúc thư mục `gateway/`

```text
gateway/
├── mosquitto/
│   └── mosquitto.conf          # Cấu hình Mosquitto MQTT Broker mở cổng LAN (0.0.0.0:1883)
├── core/
│   ├── offline_db.py           # Hàng đợi lưu đệm SQLite (WAL mode, FIFO) khi mất Internet
│   └── gateway_core.py         # Cầu nối 2 chiều Local MQTT <-> CoreIOT Gateway API (T08, T15)
├── web_ui/
│   ├── server.py               # Backend FastAPI + WebSocket + REST API điều khiển (T10)
│   └── static/index.html       # Giao diện Local LAN thuần offline (Zero-CDN)
├── edge_ai/
│   ├── ai_worker.py            # Worker nhận diện Người + Cháy/Khói trên GPU + MJPEG (T09, N04)
│   ├── train_fire.py           # Script Transfer Learning huấn luyện YOLO11n báo cháy trên GPU
│   ├── benchmark_ai.py         # Script đo so sánh hiệu năng CPU vs GPU (FPS, độ trễ ms)
│   └── weights/fire_best.pt    # Trọng số mô hình nhận diện cháy/khói đã huấn luyện
├── mock_tools/
│   └── mock_s3_hub.py          # Script giả lập ESP32-S3 (YoloUNO) để test không cần mạch thật
├── start_gateway.ps1           # Script khởi chạy nhanh toàn bộ dịch vụ Layer 2
├── .env.example                # Mẫu file cấu hình biến môi trường
├── requirements.txt            # Thư viện cốt lõi (Gateway Core & LAN Web UI)
└── requirements-ai.txt         # Thư viện Edge AI (Ultralytics YOLO, OpenCV, NVML)
```

---

### 2. Yêu cầu hệ thống

* **Hệ điều hành:** Windows 10/11 (64-bit).
* **Python:** `3.10` – `3.12` (đã thêm vào `PATH`).
* **MQTT Broker:** [Eclipse Mosquitto](https://mosquitto.org/download/) cài vào mặc định `C:\Program Files\mosquitto`.
* **GPU (khuyến nghị cho N04):** Card NVIDIA hỗ trợ CUDA (đã kiểm thử trên **RTX 4050 Laptop GPU 6GB VRAM** với PyTorch `cu124`). Máy không có GPU vẫn chạy được ở chế độ `AI_DEVICE=cpu`.

---

### 3. Hướng dẫn cài đặt

#### Bước 1: Cài đặt thư viện Python
Mở PowerShell tại thư mục `gateway/` và chạy:

```powershell
# 1. Cài đặt thư viện bắt buộc cho Gateway Core & LAN Web UI
pip install -r requirements.txt

# 2. (Chỉ cài trên máy chạy Edge AI) Cài PyTorch bản CUDA trước, rồi cài thư viện AI
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu124
pip install -r requirements-ai.txt
```

Kiểm tra máy đã nhận GPU CUDA thành công:

```powershell
python -c "import torch; print('CUDA:', torch.cuda.is_available(), '| GPU:', torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'CPU')"
```

#### Bước 2: Tắt Mosquitto Service mặc định & Mở Windows Firewall
1. Nhấn `Win + R` → gõ `services.msc` → tìm **Mosquitto Broker** → bấm **Stop** và đổi Startup type sang **Manual** (tránh tranh chấp cổng `1883` với file cấu hình LAN của dự án).
2. Mở PowerShell với quyền **Administrator** và mở 3 cổng TCP cho mạch ESP32 và điện thoại chung mạng LAN:

```powershell
New-NetFirewallRule -DisplayName "Mosquitto MQTT Broker 1883" -Direction Inbound -Protocol TCP -LocalPort 1883 -Action Allow
New-NetFirewallRule -DisplayName "SmartHome LAN Web UI 8000" -Direction Inbound -Protocol TCP -LocalPort 8000 -Action Allow
New-NetFirewallRule -DisplayName "SmartHome Edge AI MJPEG 8001" -Direction Inbound -Protocol TCP -LocalPort 8001 -Action Allow
```

#### Bước 3: Tạo file cấu hình `.env`
Tại thư mục `gateway/`, sao chép file mẫu thành `.env`:

```powershell
copy .env.example .env
```

Mở `gateway/.env` và điền các tham số:
* `COREIOT_TOKEN`: Access Token của thiết bị loại **Gateway** trên CoreIOT (giữ mặc định nếu muốn chạy test chế độ lưu đệm offline).
* `CAMERA_SOURCE`: `0` (Webcam laptop) hoặc URL IP Camera / đường dẫn file video `.mp4`.
* `AI_DEVICE`: `cuda` (chạy trên GPU NVIDIA) hoặc `cpu`.
* `FIRE_MODEL_PATH`: `weights/fire_best.pt` (để trống nếu chỉ chạy nhận diện người).

---

### 4. Hướng dẫn khởi chạy

#### Cách 1: Khởi chạy nhanh 1-Click (Khuyên dùng)
Mở PowerShell tại thư mục `gateway/`:

```powershell
# Chạy thực tế khi ghép với mạch ESP32-S3 (YoloUNO)
.\start_gateway.ps1

# Hoặc chạy kèm mạch ESP32-S3 giả lập để test độc lập không cần phần cứng
.\start_gateway.ps1 -WithMock
```

#### Cách 2: Khởi chạy thủ công từng dịch vụ (mở 4 cửa sổ PowerShell tại `gateway/`)
1. **Mosquitto MQTT Broker:**
   ```powershell
   & "C:\Program Files\mosquitto\mosquitto.exe" -c .\mosquitto\mosquitto.conf -v
   ```
2. **Local IoT Gateway Core (T08 + T15):**
   ```powershell
   python .\core\gateway_core.py
   ```
3. **Local LAN Web UI (T10):**
   ```powershell
   python .\web_ui\server.py
   ```
4. **Edge AI GPU Worker (T09 + N04):**
   ```powershell
   python .\edge_ai\ai_worker.py
   ```

#### Truy cập giao diện & Kết nối phần cứng
* **Trên PC Gateway:** Truy cập `[http://127.0.0.1:8000](http://127.0.0.1:8000)`.
* **Trên điện thoại / máy khác cùng mạng LAN:** Chạy `ipconfig` trên PC lấy `IPv4 Address` (ví dụ `192.168.1.15`), sau đó mở `[http://192.168.1.15:8000](http://192.168.1.15:8000)`.
* **Kết nối ESP32-S3 (YoloUNO):** Kết nối PC và mạch vào cùng mạng Wi-Fi **2.4GHz**, nhập `IPv4 Address` của PC vào cấu hình MQTT Broker IP của mạch (cổng `1883`).

---

### 5. Kiểm thử & Đo đạc hiệu năng (Benchmark)

* **Kiểm thử lưu đệm khi mất Internet (T08 + T15):** Ngắt kết nối Internet (giữ nguyên mạng LAN nội bộ). Hệ thống nội bộ vẫn điều khiển đèn và phát cảnh báo AI bình thường, đồng thời bộ đếm hàng đợi SQLite tăng dần trên LAN Web UI. Khi kết nối lại Internet, `gateway_core.py` tự động xả hàng đợi theo thứ tự FIFO lên `v1/gateway/telemetry` để khôi phục đầy đủ biểu đồ trên CoreIOT.
* **Đo so sánh tốc độ AI trên CPU vs GPU (N04):**
  ```powershell
  python .\edge_ai\benchmark_ai.py
  ```
* **Huấn luyện lại mô hình báo cháy/khói (tùy chọn):** Giải nén dataset chuẩn YOLO vào `gateway/edge_ai/datasets/fire_smoke/` và chạy:
  ```powershell
  python .\edge_ai\train_fire.py
  ```
