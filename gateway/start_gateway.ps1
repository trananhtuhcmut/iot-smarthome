param(
    [switch]$WithMock  # Thêm cờ -WithMock nếu muốn chạy kèm mạch ESP32-S3 giả lập
)

$GatewayDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$MosquittoExe = "C:\Program Files\mosquitto\mosquitto.exe"

Write-Host "=== KHOI DONG LOCAL IOT GATEWAY (LAYER 2) ===" -ForegroundColor Cyan

# 1. Mosquitto MQTT Broker
Start-Process powershell -ArgumentList "-NoExit", "-Command", "cd '$GatewayDir'; & '$MosquittoExe' -c .\mosquitto\mosquitto.conf -v"
Start-Sleep -Seconds 2

# 2. Gateway Core (SQLite Offline Buffer + CoreIOT Sync)
Start-Process powershell -ArgumentList "-NoExit", "-Command", "cd '$GatewayDir'; python .\core\gateway_core.py"
Start-Sleep -Seconds 1

# 3. Local LAN Web UI (FastAPI + WebSocket, port 8000)
Start-Process powershell -ArgumentList "-NoExit", "-Command", "cd '$GatewayDir'; python .\web_ui\server.py"
Start-Sleep -Seconds 1

# 4. Edge AI GPU Worker (YOLO11n Person + Fire/Smoke, port 8001)
Start-Process powershell -ArgumentList "-NoExit", "-Command", "cd '$GatewayDir'; python .\edge_ai\ai_worker.py"

# 5. (Tùy chọn) Mạch giả lập ESP32-S3 khi chưa cắm phần cứng thật
if ($WithMock) {
    Start-Sleep -Seconds 1
    Start-Process powershell -ArgumentList "-NoExit", "-Command", "cd '$GatewayDir'; python .\mock_tools\mock_s3_hub.py"
}

Write-Host "Da khoi chay xong! Mo trinh duyet tai: http://127.0.0.1:8000" -ForegroundColor Green