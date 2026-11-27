import json
import os
import threading
import time
import paho.mqtt.client as mqtt
from dotenv import load_dotenv

from offline_db import OfflineBufferDB

# Nạp biến cấu hình từ file gateway/.env
load_dotenv(os.path.join(os.path.dirname(__file__), "..", ".env"))

LOCAL_MQTT_HOST = os.getenv("LOCAL_MQTT_HOST", "127.0.0.1")
LOCAL_MQTT_PORT = int(os.getenv("LOCAL_MQTT_PORT", "1883"))

COREIOT_HOST = os.getenv("COREIOT_HOST", "app.coreiot.io")
COREIOT_PORT = int(os.getenv("COREIOT_PORT", "1883"))
COREIOT_TOKEN = os.getenv("COREIOT_TOKEN", "").strip()

DB_PATH = os.path.join(os.path.dirname(__file__), "..", os.getenv("SQLITE_DB_PATH", "gateway_buffer.db"))

# --- Hợp đồng 2: Local MQTT Topics ---
TOPIC_TELEMETRY    = "smarthome/bedroom/telemetry"
TOPIC_HUB_STATUS   = "smarthome/hub/status"
TOPIC_CMD_SWITCH   = "smarthome/cmd/switch"
TOPIC_CMD_MODE     = "smarthome/cmd/mode"
TOPIC_AI_ALERT     = "smarthome/ai/alert"
# Topic nội bộ bổ sung cho Edge AI (GĐ4) & trạng thái Gateway trên LAN UI (GĐ3)
TOPIC_AI_TELEMETRY = "smarthome/ai/telemetry"
TOPIC_GW_STATUS    = "smarthome/gateway/status"

# --- Hợp đồng 3: CoreIOT MQTT Gateway API Topics ---
CLOUD_TOPIC_CONNECT   = "v1/gateway/connect"
CLOUD_TOPIC_TELEMETRY = "v1/gateway/telemetry"
CLOUD_TOPIC_RPC       = "v1/gateway/rpc"

CLOUD_DEVICES = ["Bedroom_Node", "BLE_Env_Sensor", "YoloUNO_Hub", "Edge_AI_Cam"]


def normalize_ts_ms(raw_ts) -> int:
    """Chuẩn hóa timestamp sang mili-giây (13 chữ số) theo yêu cầu CoreIOT."""
    try:
        val = int(raw_ts)
        if val >= 1_700_000_000_000:
            return val
        if val >= 1_700_000_000:
            return val * 1000
    except (TypeError, ValueError):
        pass
    return int(time.time() * 1000)


class LocalIoTGateway:
    def __init__(self):
        self.db = OfflineBufferDB(DB_PATH)
        self.cloud_connected = False
        self.running = True
        self.sync_event = threading.Event()

        # 1. Local MQTT Client (Mosquitto cục bộ)
        self.local_client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="local_iot_gateway_core",
        )
        self.local_client.on_connect = self._on_local_connect
        self.local_client.on_message = self._on_local_message

        # 2. Cloud MQTT Client (CoreIOT Gateway)
        self.cloud_client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="coreiot_gateway_sync_client",
        )
        if COREIOT_TOKEN:
            self.cloud_client.username_pw_set(username=COREIOT_TOKEN)
        self.cloud_client.on_connect = self._on_cloud_connect
        self.cloud_client.on_disconnect = self._on_cloud_disconnect
        self.cloud_client.on_message = self._on_cloud_message
        self.cloud_client.reconnect_delay_set(min_delay=1, max_delay=15)

    # =========================================================
    # PHẦN A: LOCAL MQTT -> CHUẨN HÓA -> LƯU ĐỆM SQLITE
    # =========================================================
    def _on_local_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print(f"[LOCAL] Đã kết nối Mosquitto ({LOCAL_MQTT_HOST}:{LOCAL_MQTT_PORT})")
            client.subscribe([
                (TOPIC_TELEMETRY, 0),
                (TOPIC_HUB_STATUS, 1),
                (TOPIC_AI_ALERT, 1),
                (TOPIC_AI_TELEMETRY, 0),
            ])
            self._publish_gateway_status()
        else:
            print(f"[LOCAL] Kết nối Mosquitto thất bại, mã lỗi: {reason_code}")

    def _on_local_message(self, client, userdata, msg):
        topic = msg.topic
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except json.JSONDecodeError:
            print(f"[LOCAL] Bỏ qua gói JSON lỗi trên {topic}")
            return

        if topic == TOPIC_TELEMETRY:
            self._handle_bedroom_telemetry(payload)
        elif topic == TOPIC_HUB_STATUS:
            self._handle_hub_status(payload)
        elif topic == TOPIC_AI_TELEMETRY:
            self._handle_ai_telemetry(payload)
        elif topic == TOPIC_AI_ALERT:
            self._handle_ai_alert(payload)

    def _handle_bedroom_telemetry(self, data: dict):
        """
        Tách gói smarthome/bedroom/telemetry thành 3 thiết bị trên CoreIOT.
        Chỉ đẩy giá trị cảm biến khi tầng truyền tương ứng (ESP-NOW / BLE) đang online
        để tránh vẽ đường ngang giá trị cũ hoặc tụt về 0.0 khi rút nguồn node con.
        """
        ts_ms = normalize_ts_ms(data.get("ts"))
        switches = data.get("switches", {})

        espnow_ok = bool(data.get("espnow_online", True))
        ble_ok = espnow_ok and bool(data.get("ble_online", False))

        bedroom_values = {}
        if "espnow_online" in data:
            bedroom_values["espnow_online"] = espnow_ok
        if espnow_ok:
            if "light" in data:
                bedroom_values["light"] = data["light"]
            if "sw1_bed_light" in switches:
                bedroom_values["sw1_bed_light"] = bool(switches["sw1_bed_light"])

        ble_values = {}
        if "ble_online" in data:
            ble_values["ble_online"] = ble_ok
        if ble_ok:
            for ble_key in ("temperature", "humidity"):
                if ble_key in data:
                    ble_values[ble_key] = data[ble_key]

        cloud_payload = {}
        if bedroom_values:
            cloud_payload["Bedroom_Node"] = [{"ts": ts_ms, "values": bedroom_values}]
        if ble_values:
            cloud_payload["BLE_Env_Sensor"] = [{"ts": ts_ms, "values": ble_values}]
        if "away_mode" in data:
            cloud_payload["YoloUNO_Hub"] = [{"ts": ts_ms, "values": {"away_mode": bool(data["away_mode"])}}]

        if cloud_payload:
            self._enqueue_for_cloud(cloud_payload, tag="Bedroom+BLE")

    def _handle_hub_status(self, data: dict):
        """Phiên dịch smarthome/hub/status -> YoloUNO_Hub (đủ 6 trường mới)."""
        ts_ms = int(time.time() * 1000)
        values = {}
        # [MỚI] Bổ sung away_mode và ip theo Hợp đồng 3 mới
        for key in ("online", "away_mode", "fw_version", "wifi_channel", "rssi", "ip"):
            if key in data:
                values[key] = data[key]

        if values:
            cloud_payload = {"YoloUNO_Hub": [{"ts": ts_ms, "values": values}]}
            self._enqueue_for_cloud(cloud_payload, tag="HubStatus")
            self.db.log_event("hub", "status", json.dumps(values))

    def _handle_ai_telemetry(self, data: dict):
        """Đẩy chỉ số fps, gpu_util, ai_event định kỳ lên Edge_AI_Cam."""
        ts_ms = normalize_ts_ms(data.get("ts"))
        values = {}
        for key in ("ai_event", "ai_confidence", "fps", "gpu_util"):
            if key in data:
                values[key] = data[key]

        if values:
            cloud_payload = {"Edge_AI_Cam": [{"ts": ts_ms, "values": values}]}
            self._enqueue_for_cloud(cloud_payload, tag="EdgeAI")

    def _handle_ai_alert(self, data: dict):
        """Ghi log sự kiện cảnh báo AI & đồng bộ lên Edge_AI_Cam."""
        ts_ms = normalize_ts_ms(data.get("ts"))
        event_name = data.get("event", "none")
        if event_name == "clear":
            event_name = "none"

        values = {
            "ai_event": event_name,
            "ai_confidence": float(data.get("confidence", 0.0)),
        }
        cloud_payload = {"Edge_AI_Cam": [{"ts": ts_ms, "values": values}]}
        self._enqueue_for_cloud(cloud_payload, tag=f"AIAlert:{event_name}")
        self.db.log_event("edge_ai", event_name, data.get("message", ""))

    def _enqueue_for_cloud(self, cloud_payload: dict, tag: str = ""):
        payload_str = json.dumps(cloud_payload)
        q_size = self.db.enqueue_telemetry(payload_str)
        if not self.cloud_connected:
            print(f"[OFFLINE-BUF] Mất mạng Cloud -> Đã lưu đệm [{tag}] vào SQLite (Hàng đợi: {q_size} gói)")
        self._publish_gateway_status(q_size)
        self.sync_event.set()

    def _publish_gateway_status(self, queue_size: int | None = None):
        if queue_size is None:
            queue_size = self.db.count_queue()
        status = {
            "cloud_online": self.cloud_connected,
            "queue_size": queue_size,
            "ts": int(time.time()),
        }
        if self.local_client.is_connected():
            self.local_client.publish(TOPIC_GW_STATUS, json.dumps(status), qos=0, retain=True)

    # =========================================================
    # PHẦN B: COREIOT RPC -> PHIÊN DỊCH XUỐNG LOCAL MQTT
    # =========================================================
    def _on_cloud_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            self.cloud_connected = True
            print(f"\n[CLOUD] === ĐÃ KẾT NỐI COREIOT ({COREIOT_HOST}) THÀNH CÔNG! ===")
            self.db.log_event("gateway", "cloud_online", "Connected to CoreIOT")

            # Khai báo 4 thiết bị con với CoreIOT (v1/gateway/connect)
            for dev in CLOUD_DEVICES:
                client.publish(CLOUD_TOPIC_CONNECT, json.dumps({"device": dev}), qos=1)

            # Lắng nghe lệnh RPC từ Dashboard / RuleChain
            client.subscribe(CLOUD_TOPIC_RPC, qos=1)

            self._publish_gateway_status()
            self.sync_event.set()
        else:
            self.cloud_connected = False
            print(f"[CLOUD] Từ chối kết nối CoreIOT (Kiểm tra Access Token!), mã: {reason_code}")

    def _on_cloud_disconnect(self, client, userdata, disconnect_flags, reason_code, properties):
        if self.cloud_connected:
            print(f"\n[CLOUD] !!! MẤT KẾT NỐI COREIOT (mã: {reason_code}) -> Chuyển sang lưu đệm SQLite")
            self.db.log_event("gateway", "cloud_offline", f"Disconnected rc={reason_code}")
        self.cloud_connected = False
        self._publish_gateway_status()

    def _on_cloud_message(self, client, userdata, msg):
        if msg.topic != CLOUD_TOPIC_RPC:
            return
        try:
            rpc = json.loads(msg.payload.decode("utf-8"))
        except json.JSONDecodeError:
            print(f"[CLOUD-RPC] JSON lỗi: {msg.payload}")
            return

        print(f"\n[CLOUD-RPC] <- Nhận RPC từ CoreIOT: {rpc}")
        device = rpc.get("device")
        data = rpc.get("data", {})
        req_id = data.get("id")
        method = data.get("method")

        raw_params = data.get("params")
        params = raw_params if isinstance(raw_params, dict) else {}

        ok = False
        # 1. Bật/tắt đèn phòng ngủ (setSwitch) - hỗ trợ cả dict lẫn bool
        if device == "Bedroom_Node" and method == "setSwitch":
            state_val = raw_params if isinstance(raw_params, bool) else bool(params.get("state"))
            local_cmd = {
                "target": "Bedroom_Node",
                "switch": params.get("switch", "sw1_bed_light"),
                "state": state_val,
                "source": "coreiot",
            }
            self.local_client.publish(TOPIC_CMD_SWITCH, json.dumps(local_cmd), qos=1)
            print(f"[GATEWAY] -> Đẩy xuống {TOPIC_CMD_SWITCH}: {local_cmd}")
            ok = True

        # 2. Bật/tắt chế độ Ra khỏi nhà (setAwayMode) - hỗ trợ cả dict lẫn bool
        elif device == "YoloUNO_Hub" and method == "setAwayMode":
            away = raw_params if isinstance(raw_params, bool) else bool(params.get("away_mode"))
            local_cmd = {
                "away_mode": away,
                "turn_off_light": away,
                "source": "coreiot",
            }
            self.local_client.publish(TOPIC_CMD_MODE, json.dumps(local_cmd), qos=1)
            print(f"[GATEWAY] -> Đẩy xuống {TOPIC_CMD_MODE}: {local_cmd}")
            ok = True

        # 3. Xác nhận tắt cảnh báo AI (ackAlert) - hỗ trợ cả dict, bool lẫn None
        elif device == "Edge_AI_Cam" and method == "ackAlert":
            should_clear = raw_params if isinstance(raw_params, bool) else bool(params.get("clear", True))
            if should_clear:
                local_cmd = {
                    "alert_code": 0,
                    "event": "clear",
                    "message": "OK",
                    "ts": int(time.time()),
                }
                self.local_client.publish(TOPIC_AI_ALERT, json.dumps(local_cmd), qos=1)
                print(f"[GATEWAY] -> Đẩy xuống {TOPIC_AI_ALERT}: {local_cmd}")
                ok = True
        else:
            print(f"[CLOUD-RPC] Không hỗ trợ method '{method}' cho thiết bị '{device}'")

        # 4. Phản hồi xác nhận RPC đúng định dạng {"success": true/false}
        if req_id is not None:
            resp = {
                "device": device,
                "id": req_id,
                "data": {"success": ok},
            }
            client.publish(CLOUD_TOPIC_RPC, json.dumps(resp), qos=1)

    # =========================================================
    # PHẦN C: LUỒNG NỀN XẢ HÀNG ĐỢI SQLITE LÊN COREIOT
    # =========================================================
    def _cloud_sync_worker(self):
        while self.running:
            self.sync_event.wait(timeout=2.0)
            self.sync_event.clear()

            if not self.cloud_connected:
                continue

            while self.running and self.cloud_connected:
                batch = self.db.peek_batch(limit=10)
                if not batch:
                    break

                for row_id, payload_json in batch:
                    if not self.cloud_connected or not self.running:
                        break

                    info = self.cloud_client.publish(CLOUD_TOPIC_TELEMETRY, payload_json, qos=1)
                    if info.rc != mqtt.MQTT_ERR_SUCCESS:
                        print(f"[SYNC] Gửi thất bại (rc={info.rc}), giữ lại gói #{row_id} trong SQLite.")
                        break

                    try:
                        info.wait_for_publish(timeout=5.0)
                        if info.is_published():
                            self.db.delete_by_id(row_id)
                            remain = self.db.count_queue()
                            print(f"[SYNC] -> Đã đẩy gói #{row_id} lên CoreIOT (Còn trong SQLite: {remain})")
                            self._publish_gateway_status(remain)
                        else:
                            print(f"[SYNC] Quá giờ chờ PUBACK cho gói #{row_id}, giữ lại thử sau.")
                            break
                    except RuntimeError as e:
                        print(f"[SYNC] Lỗi chờ xác nhận gói #{row_id}: {e}")
                        break

                if self.db.count_queue() > 0:
                    time.sleep(0.1)

    def start(self):
        print("[GATEWAY] Đang khởi động Local IoT Gateway Core...")
        print(f"[GATEWAY] SQLite Buffer: {os.path.abspath(DB_PATH)} (Đang chờ: {self.db.count_queue()} gói)")

        worker_thread = threading.Thread(target=self._cloud_sync_worker, daemon=True)
        worker_thread.start()

        self.local_client.reconnect_delay_set(min_delay=1, max_delay=10)
        self.local_client.connect_async(LOCAL_MQTT_HOST, LOCAL_MQTT_PORT, keepalive=15)
        self.local_client.loop_start()

        if COREIOT_TOKEN and COREIOT_TOKEN != "DIEN_ACCESS_TOKEN_GATEWAY_VAO_DAY":
            print(f"[CLOUD] Đang kết nối CoreIOT ({COREIOT_HOST}:{COREIOT_PORT})...")
            self.cloud_client.connect_async(COREIOT_HOST, COREIOT_PORT, keepalive=30)
            self.cloud_client.loop_start()
        else:
            print("[CLOUD] Chưa cấu hình COREIOT_TOKEN trong file .env -> Đang chạy chế độ OFFLINE DEMO!")

        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n[GATEWAY] Đang dừng Gateway Core an toàn...")
            self.running = False
            self.sync_event.set()
            self.local_client.loop_stop()
            self.local_client.disconnect()
            self.cloud_client.loop_stop()
            self.cloud_client.disconnect()
            self.db.close()


if __name__ == "__main__":
    gw = LocalIoTGateway()
    gw.start()