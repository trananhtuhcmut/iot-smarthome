import asyncio
from collections import deque
from contextlib import asynccontextmanager
import json
import os
import time
from typing import Set

from dotenv import load_dotenv
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
import paho.mqtt.client as mqtt
from pydantic import BaseModel
import uvicorn

# Nạp cấu hình từ gateway/.env
load_dotenv(os.path.join(os.path.dirname(__file__), "..", ".env"))
LOCAL_MQTT_HOST = os.getenv("LOCAL_MQTT_HOST", "127.0.0.1")
LOCAL_MQTT_PORT = int(os.getenv("LOCAL_MQTT_PORT", "1883"))

# Topics theo Hợp đồng 2 (bản mới nhất)
TOPIC_TELEMETRY    = "smarthome/bedroom/telemetry"
TOPIC_HUB_STATUS   = "smarthome/hub/status"
TOPIC_CMD_SWITCH   = "smarthome/cmd/switch"
TOPIC_CMD_MODE     = "smarthome/cmd/mode"
TOPIC_AI_ALERT     = "smarthome/ai/alert"
TOPIC_AI_TELEMETRY = "smarthome/ai/telemetry"
TOPIC_GW_STATUS    = "smarthome/gateway/status"

STATIC_DIR = os.path.join(os.path.dirname(__file__), "static")


# --- Model kiểm tra dữ liệu đầu vào cho API (Pydantic) ---
class SwitchCommand(BaseModel):
    switch: str = "sw1_bed_light"
    state: bool


class ModeCommand(BaseModel):
    away_mode: bool


class TestAlertCommand(BaseModel):
    alert_code: int = 1
    event: str = "intrusion"
    confidence: float = 0.92
    message: str = "CANH BAO: PHAT HIEN NGUOI LA (TEST)"


# --- Trạng thái tổng hợp lưu trong RAM của Web Server ---
state = {
    "hub": {
        "online": False,
        "fw_version": "--",
        "wifi_channel": 0,
        "rssi": 0,
        "ip": "",
    },
    "gateway": {
        "mqtt_connected": False,
        "cloud_online": False,
        "queue_size": 0,
    },
    "bedroom": {
        "espnow_online": False,
        "ble_online": False,
        "temperature": None,
        "humidity": None,
        "light": None,
        "sw1_bed_light": False,
        "away_mode": False,
        "last_update": 0,
    },
    "ai": {
        "ai_event": "none",
        "ai_confidence": 0.0,
        "fps": 0.0,
        "gpu_util": 0.0,
    },
    "alert": {
        "active": False,
        "alert_code": 0,
        "event": "none",
        "confidence": 0.0,
        "message": "",
        "ts": 0,
    },
}

# Lưu 30 mẫu cảm biến gần nhất để vẽ đồ thị & 25 dòng log sự kiện
sensor_history = deque(maxlen=30)
event_logs = deque(maxlen=25)

# Tập các trình duyệt đang mở WebSocket và tham chiếu tới asyncio event loop
ws_clients: Set[WebSocket] = set()
main_loop: asyncio.AbstractEventLoop | None = None


def add_log(source: str, text: str):
    event_logs.appendleft({
        "time": time.strftime("%H:%M:%S"),
        "source": source,
        "text": text,
    })


def build_snapshot() -> dict:
    """Đóng gói toàn bộ trạng thái hiện tại gửi xuống trình duyệt."""
    return {
        "type": "state",
        "state": state,
        "history": list(sensor_history),
        "logs": list(event_logs),
    }


async def broadcast_state():
    """Đẩy dữ liệu mới tới tất cả các trình duyệt (PC, điện thoại) đang kết nối."""
    if not ws_clients:
        return
    payload = build_snapshot()
    dead = []
    for ws in list(ws_clients):
        try:
            await ws.send_json(payload)
        except Exception:
            dead.append(ws)
    for ws in dead:
        ws_clients.discard(ws)


def notify_clients_from_thread():
    """Cầu nối an toàn từ luồng Paho-MQTT sang luồng Asyncio của FastAPI."""
    if main_loop and main_loop.is_running():
        asyncio.run_coroutine_threadsafe(broadcast_state(), main_loop)


# =========================================================
# PHẦN MQTT CLIENT (CHẠY TRÊN LUỒNG NỀN)
# =========================================================
mqtt_client = mqtt.Client(
    callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
    client_id="local_lan_web_ui",
)


def on_mqtt_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        state["gateway"]["mqtt_connected"] = True
        add_log("SYSTEM", "Web UI đã kết nối Mosquitto Broker")
        client.subscribe([
            (TOPIC_TELEMETRY, 0),
            (TOPIC_HUB_STATUS, 1),
            (TOPIC_GW_STATUS, 0),
            (TOPIC_AI_ALERT, 1),
            (TOPIC_AI_TELEMETRY, 0),
        ])
        notify_clients_from_thread()


def on_mqtt_disconnect(client, userdata, disconnect_flags, reason_code, properties):
    state["gateway"]["mqtt_connected"] = False
    add_log("SYSTEM", f"Mất kết nối Mosquitto (rc={reason_code})")
    notify_clients_from_thread()


def on_mqtt_message(client, userdata, msg):
    topic = msg.topic
    try:
        data = json.loads(msg.payload.decode("utf-8"))
    except json.JSONDecodeError:
        return

    if topic == TOPIC_TELEMETRY:
        switches = data.get("switches", {})
        espnow_ok = bool(data.get("espnow_online", True))
        ble_ok = espnow_ok and bool(data.get("ble_online", False))

        state["bedroom"]["espnow_online"] = espnow_ok
        state["bedroom"]["ble_online"] = ble_ok

        if ble_ok:
            state["bedroom"]["temperature"] = data.get("temperature")
            state["bedroom"]["humidity"] = data.get("humidity")
            if data.get("temperature") is not None and data.get("humidity") is not None:
                sensor_history.append({
                    "t": time.strftime("%H:%M:%S"),
                    "temp": float(data["temperature"]),
                    "hum": float(data["humidity"]),
                })
        else:
            state["bedroom"]["temperature"] = None
            state["bedroom"]["humidity"] = None

        if espnow_ok:
            state["bedroom"]["light"] = data.get("light")
            if "sw1_bed_light" in switches:
                state["bedroom"]["sw1_bed_light"] = bool(switches["sw1_bed_light"])
        else:
            state["bedroom"]["light"] = None

        if "away_mode" in data:
            state["bedroom"]["away_mode"] = bool(data["away_mode"])
        state["bedroom"]["last_update"] = int(time.time())

    elif topic == TOPIC_HUB_STATUS:
        was_online = state["hub"]["online"]
        now_online = bool(data.get("online", False))
        state["hub"]["online"] = now_online
        if "fw_version" in data:
            state["hub"]["fw_version"] = data["fw_version"]
        if "wifi_channel" in data:
            state["hub"]["wifi_channel"] = data["wifi_channel"]
        if "rssi" in data:
            state["hub"]["rssi"] = data["rssi"]
        if "ip" in data:
            state["hub"]["ip"] = data["ip"]
        if "away_mode" in data:
            state["bedroom"]["away_mode"] = bool(data["away_mode"])

        if was_online != now_online:
            add_log("HUB", "YoloUNO Hub ONLINE" if now_online else "YoloUNO Hub OFFLINE (LWT)")

    elif topic == TOPIC_GW_STATUS:
        state["gateway"]["cloud_online"] = bool(data.get("cloud_online", False))
        state["gateway"]["queue_size"] = int(data.get("queue_size", 0))

    elif topic == TOPIC_AI_TELEMETRY:
        for k in ("ai_event", "ai_confidence", "fps", "gpu_util"):
            if k in data:
                state["ai"][k] = data[k]

    elif topic == TOPIC_AI_ALERT:
        code = int(data.get("alert_code", 0))
        event_name = data.get("event", "none")
        if code == 0 or event_name == "clear":
            state["alert"] = {
                "active": False,
                "alert_code": 0,
                "event": "none",
                "confidence": 0.0,
                "message": "",
                "ts": int(time.time()),
            }
            add_log("AI", "Đã xác nhận xóa cảnh báo")
        else:
            state["alert"] = {
                "active": True,
                "alert_code": code,
                "event": event_name,
                "confidence": float(data.get("confidence", 0.0)),
                "message": data.get("message", "CANH BAO AN NINH"),
                "ts": int(data.get("ts", time.time())),
            }
            add_log("ALERT", f"[{event_name.upper()}] {state['alert']['message']}")

    notify_clients_from_thread()


mqtt_client.on_connect = on_mqtt_connect
mqtt_client.on_disconnect = on_mqtt_disconnect
mqtt_client.on_message = on_mqtt_message


# =========================================================
# VÒNG ĐỜI KHỞI ĐỘNG / TẮT FASTAPI (LIFESPAN)
# =========================================================
@asynccontextmanager
async def lifespan(app: FastAPI):
    global main_loop
    main_loop = asyncio.get_running_loop()
    mqtt_client.connect_async(LOCAL_MQTT_HOST, LOCAL_MQTT_PORT, keepalive=15)
    mqtt_client.loop_start()
    yield
    mqtt_client.loop_stop()
    mqtt_client.disconnect()


app = FastAPI(title="SmartHome Local LAN Web UI (T10)", lifespan=lifespan)


# =========================================================
# CÁC ĐƯỜNG DẪN WEB, REST API & WEBSOCKET
# =========================================================
@app.get("/")
async def index():
    return FileResponse(os.path.join(STATIC_DIR, "index.html"))


@app.get("/api/state")
async def get_state():
    return build_snapshot()


@app.post("/api/cmd/switch")
async def cmd_switch(cmd: SwitchCommand):
    payload = {
        "target": "Bedroom_Node",
        "switch": cmd.switch,
        "state": cmd.state,
        "source": "lan_ui",
    }
    mqtt_client.publish(TOPIC_CMD_SWITCH, json.dumps(payload), qos=1)
    add_log("LAN_UI", f"Đổi {cmd.switch} -> {'BẬT' if cmd.state else 'TẮT'}")
    await broadcast_state()
    return {"ok": True, "sent": payload}


@app.post("/api/cmd/mode")
async def cmd_mode(cmd: ModeCommand):
    # Đúng key "turn_off_light" theo Hợp đồng 2 mới nhất
    payload = {
        "away_mode": cmd.away_mode,
        "turn_off_light": cmd.away_mode,
        "source": "lan_ui",
    }
    mqtt_client.publish(TOPIC_CMD_MODE, json.dumps(payload), qos=1)
    add_log("LAN_UI", f"Chế độ Ra khỏi nhà (Away Mode) -> {'BẬT' if cmd.away_mode else 'TẮT'}")
    await broadcast_state()
    return {"ok": True, "sent": payload}


@app.post("/api/cmd/clear_alert")
async def cmd_clear_alert():
    payload = {
        "alert_code": 0,
        "event": "clear",
        "message": "OK",
        "ts": int(time.time()),
    }
    mqtt_client.publish(TOPIC_AI_ALERT, json.dumps(payload), qos=1)
    return {"ok": True, "sent": payload}


@app.post("/api/test/alert")
async def trigger_test_alert(cmd: TestAlertCommand):
    """Nút giả lập cảnh báo AI để test luồng cảnh báo trước khi làm Giai đoạn 4."""
    payload = {
        "alert_code": cmd.alert_code,
        "event": cmd.event,
        "confidence": cmd.confidence,
        "message": cmd.message,
        "ts": int(time.time()),
    }
    mqtt_client.publish(TOPIC_AI_ALERT, json.dumps(payload), qos=1)
    return {"ok": True, "sent": payload}


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    ws_clients.add(ws)
    try:
        # Vừa mở trang là gửi ngay dữ liệu mới nhất + lịch sử đồ thị
        await ws.send_json(build_snapshot())
        while True:
            # Giữ kết nối mở, nhận ping từ trình duyệt nếu có
            await ws.receive_text()
    except WebSocketDisconnect:
        ws_clients.discard(ws)
    except Exception:
        ws_clients.discard(ws)


if __name__ == "__main__":
    # 0.0.0.0 để cả máy PC lẫn điện thoại trong mạng LAN đều truy cập được
    uvicorn.run(app, host="0.0.0.0", port=8000)