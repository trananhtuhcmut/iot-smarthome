import argparse
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import threading
import time

import cv2
from dotenv import load_dotenv
import paho.mqtt.client as mqtt
import torch
from ultralytics import YOLO

try:
    import pynvml
    PYNVML_AVAILABLE = True
except ImportError:
    PYNVML_AVAILABLE = False

# Nạp biến môi trường từ gateway/.env
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
load_dotenv(os.path.join(BASE_DIR, "..", ".env"))

LOCAL_MQTT_HOST = os.getenv("LOCAL_MQTT_HOST", "127.0.0.1")
LOCAL_MQTT_PORT = int(os.getenv("LOCAL_MQTT_PORT", "1883"))

CAMERA_SOURCE_RAW = os.getenv("CAMERA_SOURCE", "0").strip()
DEFAULT_DEVICE    = os.getenv("AI_DEVICE", "cuda").strip()
PERSON_MODEL_PATH = os.getenv("PERSON_MODEL_PATH", "yolo11n.pt").strip()
FIRE_MODEL_RAW    = os.getenv("FIRE_MODEL_PATH", "").strip()
AI_STREAM_PORT    = int(os.getenv("AI_STREAM_PORT", "8001"))

# Topics theo Hợp đồng 2 (bản mới nhất)
TOPIC_TELEMETRY    = "smarthome/bedroom/telemetry"
TOPIC_HUB_STATUS   = "smarthome/hub/status"
TOPIC_CMD_MODE     = "smarthome/cmd/mode"
TOPIC_AI_ALERT     = "smarthome/ai/alert"
TOPIC_AI_TELEMETRY = "smarthome/ai/telemetry"

# Ngưỡng nhận diện & chống báo động giả
CONF_PERSON     = 0.60
CONF_FIRE       = 0.45
STREAK_REQUIRED = 6     # Số frame phát hiện liên tiếp trước khi báo động
ALERT_COOLDOWN  = 10.0  # Giây nghỉ tối thiểu giữa 2 lần bắn gói cảnh báo

# Biến chia sẻ khung hình JPEG cho HTTP MJPEG Server (an toàn đa luồng)
latest_jpeg = None
frame_condition = threading.Condition()


class MJPEGHandler(BaseHTTPRequestHandler):
    """HTTP Server siêu nhẹ phát luồng video MJPEG sang LAN Web UI."""

    def do_GET(self):
        if self.path not in ("/video_feed", "/"):
            self.send_error(404)
            return

        self.send_response(200)
        self.send_header("Age", "0")
        self.send_header("Cache-Control", "no-cache, private")
        self.send_header("Pragma", "no-cache")
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()

        try:
            while True:
                with frame_condition:
                    frame_condition.wait(timeout=2.0)
                    jpeg = latest_jpeg
                if jpeg is None:
                    continue
                self.wfile.write(b"--frame\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                self.wfile.write(f"Content-Length: {len(jpeg)}\r\n\r\n".encode("ascii"))
                self.wfile.write(jpeg)
                self.wfile.write(b"\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass  # Trình duyệt đóng tab

    def log_message(self, format, *args):
        # Tắt log HTTP mặc định để terminal sạch sẽ dễ nhìn log AI/MQTT
        pass


def start_mjpeg_server(port: int):
    server = ThreadingHTTPServer(("0.0.0.0", port), MJPEGHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    print(f"[STREAM] Live Camera MJPEG đang mở tại http://0.0.0.0:{port}/video_feed")
    return server


class GPUMonitor:
    """Đọc % tải GPU NVIDIA (RTX 4050) bằng thư viện NVML."""

    def __init__(self, enabled: bool):
        self.handle = None
        if enabled and PYNVML_AVAILABLE:
            try:
                pynvml.nvmlInit()
                self.handle = pynvml.nvmlDeviceGetHandleByIndex(0)
                name = pynvml.nvmlDeviceGetName(self.handle)
                print(f"[GPU] NVML sẵn sàng theo dõi: {name}")
            except Exception as e:
                print(f"[GPU] Không khởi tạo được NVML: {e}")

    def get_util(self) -> float:
        if not self.handle:
            return 0.0
        try:
            util = pynvml.nvmlDeviceGetUtilizationRates(self.handle)
            return float(util.gpu)
        except Exception:
            return 0.0

    def close(self):
        if self.handle:
            try:
                pynvml.nvmlShutdown()
            except Exception:
                pass


class EdgeAIWorker:
    def __init__(self, device: str, source_raw: str):
        # Tự động fallback về CPU nếu máy không có CUDA
        if device.startswith("cuda") and not torch.cuda.is_available():
            print("[AI] Cảnh báo: Không phát hiện CUDA -> Tự động chuyển sang CPU!")
            device = "cpu"
        self.device = device
        self.use_half = self.device.startswith("cuda")  # FP16 chỉ bật trên GPU
        self.source = int(source_raw) if source_raw.isdigit() else source_raw

        # Trạng thái đồng bộ từ hệ thống
        self.away_mode = False
        self.person_streak = 0
        self.fire_streak = 0
        self.last_alert_time = 0.0
        self.active_event = "none"
        self.active_conf = 0.0

        # 1. Nạp Model A: Nhận diện người (Pretrained COCO, lọc classes=[0])
        person_path = self._resolve_weight_path(PERSON_MODEL_PATH)
        print(f"[AI] Đang nạp Model Người: {person_path} trên [{self.device.upper()}] (FP16={self.use_half})...")
        self.person_model = YOLO(person_path)

        # 2. Khe cắm Plug-and-Play cho Model B: Nhận diện cháy/khói (tùy chọn)
        self.fire_model = None
        self.fire_class_ids = []
        if FIRE_MODEL_RAW:
            fire_path = self._resolve_weight_path(FIRE_MODEL_RAW)
            if os.path.exists(fire_path):
                print(f"[AI] Đã cắm Model Báo Cháy (Plug-and-Play): {fire_path}")
                self.fire_model = YOLO(fire_path)
                self.fire_class_ids = [
                    cid for cid, cname in self.fire_model.names.items()
                    if any(k in str(cname).lower() for k in ("fire", "smoke", "flame"))
                ]
                print(f"[AI] Đã kích hoạt các nhãn cháy/khói: {[(i, self.fire_model.names[i]) for i in self.fire_class_ids]}")
            else:
                print(f"[AI] Cảnh báo: Không tìm thấy file model cháy '{fire_path}' -> Chỉ chạy nhận diện người.")
        else:
            print("[AI] FIRE_MODEL_PATH đang để trống -> Chế độ nhận diện người (sẵn sàng cắm model cháy sau).")

        self.gpu_mon = GPUMonitor(enabled=self.use_half)

        # 3. Kết nối Local Mosquitto Broker
        self.mqtt_client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="edge_ai_gpu_worker",
        )
        self.mqtt_client.on_connect = self._on_mqtt_connect
        self.mqtt_client.on_message = self._on_mqtt_message

    def _resolve_weight_path(self, path_str: str) -> str:
        """Nếu truyền tên file/đường dẫn tương đối thì tìm bên trong gateway/edge_ai/."""
        if os.path.isabs(path_str) or os.path.exists(path_str):
            return path_str
        local_candidate = os.path.join(BASE_DIR, path_str)
        if os.path.exists(local_candidate):
            return local_candidate
        # Nếu là tên model chuẩn như yolo11n.pt -> lưu vào edge_ai/weights/ cho gọn repo
        return os.path.join(BASE_DIR, "weights", os.path.basename(path_str))

    def _on_mqtt_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print(f"[MQTT] Edge AI đã kết nối Mosquitto ({LOCAL_MQTT_HOST}:{LOCAL_MQTT_PORT})")
            client.subscribe([
                (TOPIC_TELEMETRY, 0),
                (TOPIC_HUB_STATUS, 1),
                (TOPIC_CMD_MODE, 1),
                (TOPIC_AI_ALERT, 1),
            ])

    def _on_mqtt_message(self, client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except json.JSONDecodeError:
            return

        if msg.topic in (TOPIC_TELEMETRY, TOPIC_HUB_STATUS, TOPIC_CMD_MODE):
            if "away_mode" in data:
                new_away = bool(data["away_mode"])
                if new_away != self.away_mode:
                    self.away_mode = new_away
                    self.person_streak = 0
                    print(f"[AI] Đồng bộ chế độ Away Mode = {self.away_mode}")

        elif msg.topic == TOPIC_AI_ALERT:
            # Khi người dùng bấm xác nhận tắt cảnh báo từ Web UI / CoreIOT
            if int(data.get("alert_code", 0)) == 0 or data.get("event") == "clear":
                self.active_event = "none"
                self.active_conf = 0.0
                self.person_streak = 0
                self.fire_streak = 0
                self.last_alert_time = time.time()  # Nghỉ cooldown để không lập tức hú lại
                print("[AI] Đã nhận lệnh xóa cảnh báo từ người dùng.")

    def _publish_alert(self, alert_code: int, event_name: str, conf: float, message: str):
        """Gửi gói cảnh báo khẩn đúng chuẩn Hợp đồng 2 (không còn key buzzer)."""
        payload = {
            "alert_code": alert_code,
            "event": event_name,
            "confidence": round(float(conf), 2),
            "message": message,
            "ts": int(time.time()),
        }
        self.mqtt_client.publish(TOPIC_AI_ALERT, json.dumps(payload), qos=1)
        print(f"[AI-ALERT] >>> Đã bắn cảnh báo: {payload}")

    def _publish_telemetry(self, fps: float, gpu_util: float):
        """Đẩy chỉ số FPS, GPU% và sự kiện hiện tại lên LAN UI & CoreIOT (Edge_AI_Cam)."""
        payload = {
            "ai_event": self.active_event,
            "ai_confidence": round(float(self.active_conf), 2),
            "fps": round(float(fps), 1),
            "gpu_util": round(float(gpu_util), 1),
            "ts": int(time.time()),
        }
        self.mqtt_client.publish(TOPIC_AI_TELEMETRY, json.dumps(payload), qos=0)

    def run(self, show_window: bool = True):
        global latest_jpeg

        self.mqtt_client.connect_async(LOCAL_MQTT_HOST, LOCAL_MQTT_PORT, keepalive=15)
        self.mqtt_client.loop_start()

        mjpeg_server = start_mjpeg_server(AI_STREAM_PORT)

        # Mở Webcam (dùng CAP_DSHOW trên Windows để mở webcam nhanh và ổn định hơn)
        if isinstance(self.source, int) and os.name == "nt":
            cap = cv2.VideoCapture(self.source, cv2.CAP_DSHOW)
        else:
            cap = cv2.VideoCapture(self.source)

        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)

        if not cap.isOpened():
            print(f"[AI] LỖI: Không mở được camera nguồn '{self.source}'!")
            return

        print("[AI] Bắt đầu nhận diện thời gian thực (Nhấn 'q' trên cửa sổ video hoặc Ctrl+C để dừng)...")

        infer_times = deque(maxlen=30)  # Tính tốc độ suy luận trung bình 30 frame gần nhất
        loop_times = deque(maxlen=30)   # Tính FPS tổng thực tế
        last_tele_send = 0.0
        prev_loop_ts = time.perf_counter()

        try:
            while True:
                ok, frame = cap.read()
                if not ok:
                    # Nếu đang chạy video .mp4 benchmark -> tua lại từ đầu
                    if isinstance(self.source, str) and os.path.isfile(self.source):
                        cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                        continue
                    print("[AI] Không đọc được khung hình từ camera, thử lại...")
                    time.sleep(0.5)
                    continue

                t0 = time.perf_counter()

                # --- 1. Suy luận Model A: Nhận diện người (classes=[0] = person) ---
                person_max_conf = 0.0
                person_count = 0
                res_person = self.person_model.predict(
                    frame,
                    classes=[0],
                    conf=CONF_PERSON,
                    imgsz=640,
                    device=self.device,
                    half=self.use_half,
                    verbose=False,
                )[0]

                for box in res_person.boxes:
                    conf = float(box.conf[0])
                    person_count += 1
                    if conf > person_max_conf:
                        person_max_conf = conf
                    x1, y1, x2, y2 = map(int, box.xyxy[0])
                    # Đỏ nếu đang bật Away Mode (xâm nhập), xanh lá nếu đang ở nhà
                    color = (0, 0, 255) if self.away_mode else (0, 220, 0)
                    label = f"INTRUDER {conf:.2f}" if self.away_mode else f"Person {conf:.2f}"
                    cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
                    cv2.putText(frame, label, (x1, max(20, y1 - 8)),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)

                # --- 2. Suy luận Model B (Plug-and-Play): Nhận diện cháy/khói nếu đã cắm ---
                fire_max_conf = 0.0
                if self.fire_model is not None and self.fire_class_ids:
                    res_fire = self.fire_model.predict(
                        frame,
                        classes=self.fire_class_ids,
                        conf=CONF_FIRE,
                        imgsz=640,
                        device=self.device,
                        half=self.use_half,
                        verbose=False,
                    )[0]
                    for box in res_fire.boxes:
                        conf = float(box.conf[0])
                        cls_id = int(box.cls[0])
                        cls_name = self.fire_model.names.get(cls_id, "fire")
                        if conf > fire_max_conf:
                            fire_max_conf = conf
                        x1, y1, x2, y2 = map(int, box.xyxy[0])
                        cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 140, 255), 2)
                        cv2.putText(frame, f"{cls_name.upper()} {conf:.2f}", (x1, max(20, y1 - 8)),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0, 140, 255), 2)

                t1 = time.perf_counter()
                infer_ms = (t1 - t0) * 1000.0
                infer_times.append(infer_ms)

                # Tính FPS tổng (bao gồm cả đọc camera & vẽ khung)
                now_ts = time.perf_counter()
                dt = now_ts - prev_loop_ts
                prev_loop_ts = now_ts
                if dt > 0:
                    loop_times.append(1.0 / dt)
                avg_fps = sum(loop_times) / len(loop_times) if loop_times else 0.0
                avg_infer_ms = sum(infer_times) / len(infer_times) if infer_times else 0.0

                # --- 3. Logic lọc nhiễu (Streak) & Bắn cảnh báo (Cooldown) ---
                self.fire_streak = self.fire_streak + 1 if fire_max_conf >= CONF_FIRE else 0
                if self.away_mode and person_max_conf >= CONF_PERSON:
                    self.person_streak += 1
                else:
                    self.person_streak = 0

                now_epoch = time.time()
                if self.fire_streak >= STREAK_REQUIRED:
                    self.active_event = "fire"
                    self.active_conf = fire_max_conf
                    if now_epoch - self.last_alert_time >= ALERT_COOLDOWN:
                        self._publish_alert(2, "fire", fire_max_conf, "CANH BAO: PHAT HIEN CHAY NO")
                        self.last_alert_time = now_epoch
                elif self.person_streak >= STREAK_REQUIRED:
                    self.active_event = "intrusion"
                    self.active_conf = person_max_conf
                    if now_epoch - self.last_alert_time >= ALERT_COOLDOWN:
                        self._publish_alert(1, "intrusion", person_max_conf, "CANH BAO: PHAT HIEN NGUOI LA")
                        self.last_alert_time = now_epoch

                # --- 4. Vẽ thanh HUD thông số kỹ thuật lên khung hình (cực đẹp lúc quay demo) ---
                gpu_util = self.gpu_mon.get_util()
                mode_str = "AWAY (ARMED)" if self.away_mode else "HOME (SAFE)"
                hud_color = (0, 0, 255) if self.away_mode else (0, 255, 0)
                cv2.rectangle(frame, (0, 0), (frame.shape[1], 36), (15, 23, 42), -1)
                cv2.putText(
                    frame,
                    f"Mode: {mode_str} | {self.device.upper()} | {avg_fps:.1f} FPS ({avg_infer_ms:.1f}ms) | GPU: {gpu_util:.0f}%",
                    (10, 24),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.52,
                    hud_color,
                    2,
                )

                # --- 5. Nén JPEG đẩy sang MJPEG Server cho LAN Web UI ---
                ok_jpg, buf = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 75])
                if ok_jpg:
                    with frame_condition:
                        latest_jpeg = buf.tobytes()
                        frame_condition.notify_all()

                # --- 6. Đẩy Telemetry định kỳ mỗi 2 giây ---
                if now_epoch - last_tele_send >= 2.0:
                    self._publish_telemetry(avg_fps, gpu_util)
                    last_tele_send = now_epoch

                if show_window:
                    cv2.imshow("SmartHome Edge AI (GPU RTX 4050)", frame)
                    if cv2.waitKey(1) & 0xFF == ord("q"):
                        break

        except KeyboardInterrupt:
            print("\n[AI] Đang dừng Edge AI Worker...")
        finally:
            cap.release()
            cv2.destroyAllWindows()
            mjpeg_server.shutdown()
            self.gpu_mon.close()
            self.mqtt_client.loop_stop()
            self.mqtt_client.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SmartHome Edge AI Worker (T09 + N04)")
    parser.add_argument("--device", default=DEFAULT_DEVICE, help="cuda hoặc cpu (dùng để benchmark)")
    parser.add_argument("--source", default=CAMERA_SOURCE_RAW, help="0 (webcam) hoặc đường dẫn video .mp4")
    parser.add_argument("--headless", action="store_true", help="Không mở cửa sổ OpenCV (chỉ stream lên Web UI)")
    args = parser.parse_args()

    worker = EdgeAIWorker(device=args.device, source_raw=args.source)
    worker.run(show_window=not args.headless)