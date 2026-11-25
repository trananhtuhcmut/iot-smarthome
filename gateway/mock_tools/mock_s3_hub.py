import json
import random
import time
import paho.mqtt.client as mqtt

BROKER_HOST = "127.0.0.1"
BROKER_PORT = 1883

# 5 Topic cố định theo Hợp đồng 2
TOPIC_TELEMETRY  = "smarthome/bedroom/telemetry"
TOPIC_HUB_STATUS = "smarthome/hub/status"
TOPIC_CMD_SWITCH = "smarthome/cmd/switch"
TOPIC_CMD_MODE   = "smarthome/cmd/mode"
TOPIC_AI_ALERT   = "smarthome/ai/alert"

state = {
    "espnow_online": True,        # [MỚI] Trạng thái kết nối ESP-NOW tới WROOM
    "ble_online": True,           # Trạng thái kết nối BLE tới ESP32-C3 (DHT11)
    "temperature": 28.5,
    "humidity": 65.2,
    "light": 420,
    "switches": {
        "sw1_bed_light": True,    # [MỚI] Rút gọn còn đúng 1 relay đèn phòng ngủ
    },
    "away_mode": False,
    "fw_version": "1.2.0-chart",  # [MỚI]
    "wifi_channel": 6,
    "rssi": -54,
    "ip": "192.168.1.70",         # [MỚI] IP LAN hiện tại của YoloUNO
}


def publish_hub_status(client):
    """Hàm gom việc gửi smarthome/hub/status (QoS 1, Retain = True)."""
    status_payload = {
        "device": "YoloUNO_Hub",
        "online": True,
        "away_mode": state["away_mode"],
        "fw_version": state["fw_version"],
        "wifi_channel": state["wifi_channel"],
        "rssi": state["rssi"],
        "ip": state["ip"],               
    }
    client.publish(TOPIC_HUB_STATUS, json.dumps(status_payload), qos=1, retain=True)
    print(f"[MOCK-S3] -> {TOPIC_HUB_STATUS}: {status_payload}")


def publish_telemetry(client):
    """Gửi ngay gói telemetry (định kỳ 3s hoặc ngay khi đổi trạng thái)."""
    telemetry_payload = {
        "device": "Bedroom_Node",
        "ts": int(time.time()),
        "espnow_online": state["espnow_online"],
        "ble_online": state["ble_online"],
        "temperature": state["temperature"],
        "humidity": state["humidity"],
        "light": state["light"],
        "switches": state["switches"],
        "away_mode": state["away_mode"],
    }
    client.publish(TOPIC_TELEMETRY, json.dumps(telemetry_payload), qos=0, retain=False)
    print(
        f"[MOCK-S3] -> Telemetry: T={state['temperature']}°C, H={state['humidity']}% "
        f"(BLE={state['ble_online']}, ESPNOW={state['espnow_online']}), "
        f"L={state['light']}, LightSW={state['switches']['sw1_bed_light']}, Away={state['away_mode']}"
    )


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print("[MOCK-S3] Đã kết nối Mosquitto Broker thành công!")
        client.subscribe([
            (TOPIC_CMD_SWITCH, 1),
            (TOPIC_CMD_MODE, 1),
            (TOPIC_AI_ALERT, 1),
        ])
        publish_hub_status(client)
    else:
        print(f"[MOCK-S3] Kết nối thất bại, mã lỗi: {reason_code}")


def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except json.JSONDecodeError:
        print(f"[MOCK-S3] Nhận JSON lỗi trên {topic}: {msg.payload}")
        return

    print(f"\n[MOCK-S3] <- Nhận từ {topic}: {payload}")

    if topic == TOPIC_CMD_SWITCH:
        sw_name = payload.get("switch")
        sw_state = payload.get("state")
        if sw_name in state["switches"] and isinstance(sw_state, bool):
            state["switches"][sw_name] = sw_state
            print(f"[MOCK-S3] Đã đổi {sw_name} = {sw_state} (nguồn: {payload.get('source')})")
            # Phản hồi telemetry ngay lập tức giống mạch thật khi đổi đèn
            publish_telemetry(client)

    elif topic == TOPIC_CMD_MODE:
        away = payload.get("away_mode")
        if isinstance(away, bool):
            state["away_mode"] = away
            # [MỚI] Đổi sang key "turn_off_light" theo Hợp đồng 2 mới
            if away and payload.get("turn_off_light", False):
                state["switches"]["sw1_bed_light"] = False
                print("[MOCK-S3] Bật Away Mode -> Tự động tắt đèn phòng ngủ!")
            else:
                print(f"[MOCK-S3] Chuyển away_mode = {away}")
            publish_hub_status(client)
            publish_telemetry(client)

    elif topic == TOPIC_AI_ALERT:
        alert_code = payload.get("alert_code", 0)
        if alert_code == 0:
            print(f"[MOCK-S3] Đã xóa cảnh báo (LVGL & Web T07 ẩn pop-up): {payload.get('message')}")
        else:
            print(f"[MOCK-S3] !!! BÁO ĐỘNG [code={alert_code}]: {payload.get('message')}")


def main():
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="mock_yolouno_s3_hub",
    )

    # Last Will & Testament (LWT)
    lwt_payload = json.dumps({"device": "YoloUNO_Hub", "online": False})
    client.will_set(TOPIC_HUB_STATUS, payload=lwt_payload, qos=1, retain=True)

    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(BROKER_HOST, BROKER_PORT, keepalive=10)
    client.loop_start()

    try:
        while True:
            state["temperature"] = round(max(22.0, min(36.0, state["temperature"] + random.uniform(-0.3, 0.3))), 1)
            state["humidity"] = round(max(45.0, min(85.0, state["humidity"] + random.uniform(-0.8, 0.8))), 1)
            state["light"] = max(50, min(1000, state["light"] + random.randint(-15, 15)))
            publish_telemetry(client)
            time.sleep(3)

    except KeyboardInterrupt:
        print("\n[MOCK-S3] Dừng giả lập chủ động...")
        client.publish(TOPIC_HUB_STATUS, lwt_payload, qos=1, retain=True)
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()