import json, time, threading
import paho.mqtt.client as mqtt

GATEWAY_TOKEN = "fFjYIOYG7JjMvvYDjmLZ"
DEVICES = ["Bedroom_Node", "BLE_Env_Sensor", "YoloUNO_Hub", "Edge_AI_Cam"]

sim_light = False
sim_away = False
sim_fw_version = "1.3.0-ws"

def send_telemetry(client, extra_hub=None):
    ts = int(time.time() * 1000)
    hub_values = {
        "online": True,
        "away_mode": sim_away,
        "fw_version": sim_fw_version,
        "wifi_channel": 6,
        "rssi": -55,
        "ip": "192.168.1.70"
    }
    if extra_hub:
        hub_values.update(extra_hub)

    payload = {
        "Bedroom_Node": [{"ts": ts, "values": {"espnow_online": True, "light": 180, "sw1_bed_light": sim_light}}],
        "BLE_Env_Sensor": [{"ts": ts, "values": {"temperature": 36.5, "humidity": 68.0, "ble_online": True}}],
        "YoloUNO_Hub": [{"ts": ts, "values": hub_values}],
        "Edge_AI_Cam": [{"ts": ts, "values": {"alert_code": 0, "event": "clear", "message": "OK"}}]
    }
    client.publish("v1/gateway/telemetry", json.dumps(payload))

def simulate_ota(client, target_title, target_version):
    global sim_fw_version
    for state in ["DOWNLOADING", "DOWNLOADED", "VERIFIED", "UPDATING"]:
        print(f"[OTA] YoloUNO_Hub -> {state} ({target_version})")
        send_telemetry(client, {"fw_state": state})
        time.sleep(1.5)

    sim_fw_version = target_version
    send_telemetry(client, {
        "fw_state": "UPDATED",
        "current_fw_title": target_title,
        "current_fw_version": target_version
    })
    print(f"[OTA] Cap nhat thanh cong! Phien ban hien tai: {sim_fw_version}")

def on_connect(client, userdata, flags, rc, props=None):
    print("Ket noi CoreIOT:", rc)
    client.subscribe("v1/gateway/rpc")
    client.subscribe("v1/gateway/attributes")
    for d in DEVICES:
        client.publish("v1/gateway/connect", json.dumps({"device": d}))

def on_message(client, userdata, msg):
    global sim_light, sim_away
    payload = json.loads(msg.payload.decode())

    if msg.topic == "v1/gateway/attributes":
        print("Nhan thong bao Shared Attributes (OTA):", payload)
        data = payload.get("data", {})
        if payload.get("device") == "YoloUNO_Hub" and "fw_version" in data:
            new_ver = data["fw_version"]
            new_title = data.get("fw_title", "YoloUNO_Hub_FW")
            if new_ver and new_ver != sim_fw_version:
                threading.Thread(
                    target=simulate_ota,
                    args=(client, new_title, new_ver),
                    daemon=True
                ).start()
        return

    print("Nhan lenh RPC:", payload)
    method = payload["data"]["method"]
    params = payload["data"].get("params", {})

    if method == "setSwitch":
        sim_light = bool(params.get("state", False))
        if sim_light:
            sim_away = False
    elif method == "setAwayMode":
        sim_away = bool(params.get("away_mode", False))
        if sim_away:
            sim_light = False

    ack = {"device": payload["device"], "id": payload["data"]["id"], "data": {"success": True}}
    client.publish("v1/gateway/rpc", json.dumps(ack))
    send_telemetry(client)

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.username_pw_set(GATEWAY_TOKEN)
client.on_connect = on_connect
client.on_message = on_message
client.connect("app.coreiot.io", 1883, 60)
client.loop_start()

while True:
    send_telemetry(client)
    time.sleep(3)