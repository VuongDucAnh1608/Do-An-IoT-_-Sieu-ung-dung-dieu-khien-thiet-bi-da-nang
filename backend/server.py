"""
Do An IoT - Backend Server
FastAPI + MQTT + WebSocket

Chức năng:
  - Nhận dữ liệu cảm biến từ ESP32 qua MQTT
  - Phát dữ liệu thời gian thực tới trình duyệt qua WebSocket
  - Nhận lệnh điều khiển từ giao diện chat và chuyển sang MQTT

Yêu cầu:
  - Mosquitto MQTT Broker đang chạy trên máy (cổng 1883)
  - Xem requirements.txt để cài đặt thư viện

Chạy server:
  uvicorn server:app --host 0.0.0.0 --port 8000 --reload
"""

import json
from contextlib import asynccontextmanager

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi_mqtt import FastMQTT, MQTTConfig
from pydantic import BaseModel

# ============================================================
# Cấu hình MQTT – phải khớp với cài đặt Mosquitto và ESP32
# ============================================================
mqtt_config = MQTTConfig(
    host="localhost",   # Địa chỉ Mosquitto broker
    port=1883,
    username="mqtt_user",
    password="mqtt_password",
)

mqtt = FastMQTT(config=mqtt_config)

# ============================================================
# Lifespan: khởi động và dọn dẹp MQTT đúng cách
# (thay thế @app.on_event deprecated trong FastAPI mới)
# ============================================================
@asynccontextmanager
async def lifespan(app: FastAPI):
    await mqtt.mqtt_startup()
    yield
    await mqtt.mqtt_shutdown()

# ============================================================
# Khởi tạo FastAPI
# ============================================================
app = FastAPI(
    title="IoT Room Control API",
    version="1.0.0",
    lifespan=lifespan,
)

# CORS: cho phép trình duyệt gọi API từ bất kỳ origin nào
# (trong production nên giới hạn lại bằng allow_origins cụ thể)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ============================================================
# Quản lý danh sách kết nối WebSocket
# ============================================================
class ConnectionManager:
    def __init__(self):
        self.active_connections: list[WebSocket] = []

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self.active_connections.append(ws)
        print(f"WebSocket moi ket noi. Tong: {len(self.active_connections)}")

    def disconnect(self, ws: WebSocket):
        if ws in self.active_connections:
            self.active_connections.remove(ws)
        print(f"WebSocket ngat ket noi. Con lai: {len(self.active_connections)}")

    async def broadcast(self, message: dict):
        """Gửi dữ liệu tới tất cả client đang kết nối."""
        data = json.dumps(message)
        disconnected = []
        for ws in self.active_connections:
            try:
                await ws.send_text(data)
            except Exception:
                disconnected.append(ws)
        for ws in disconnected:
            self.disconnect(ws)

manager = ConnectionManager()

# ============================================================
# Schema cho endpoint chat
# ============================================================
class ChatInput(BaseModel):
    message: str

# ============================================================
# MQTT: xử lý khi kết nối thành công với Broker
# ============================================================
@mqtt.on_connect()
async def handle_connect(client, userdata, flags, rc):
    print(f"MQTT ket noi thanh cong. RC={rc}")
    # Đăng ký nhận tất cả topic dữ liệu từ ESP32
    client.subscribe("iot/sensor/data")
    client.subscribe("iot/sensor/status")

# ============================================================
# MQTT: xử lý khi nhận được message
# ============================================================
@mqtt.on_message()
async def handle_mqtt_message(client, topic, payload, qos, properties):
    """
    Nhận dữ liệu từ ESP32 qua MQTT và phát tới tất cả WebSocket client.
    Payload đến phải là JSON hợp lệ.
    """
    try:
        data = json.loads(payload.decode("utf-8"))
        # Đính kèm tên topic để frontend có thể phân biệt loại dữ liệu
        data["_topic"] = topic
        await manager.broadcast(data)
    except json.JSONDecodeError as e:
        print(f"MQTT parse JSON loi ({topic}): {e}")
    except Exception as e:
        print(f"MQTT xu ly loi: {e}")

# ============================================================
# API: Nhận lệnh từ giao diện chat, chuyển đổi sang MQTT
# ============================================================
@app.post("/api/chat")
async def chat_command(input: ChatInput):
    """
    Xử lý lệnh bằng ngôn ngữ tự nhiên từ giao diện chat.

    Lệnh được hỗ trợ (tiếng Việt và tiếng Anh):
      - "auto" / "chế độ tự động"   → chuyển sang chế độ tự động
      - "manual" / "thủ công"        → chuyển sang chế độ thủ công
      - "bật" / "on" / "bat"         → bật relay (chế độ manual)
      - "tắt" / "off" / "tat"        → tắt relay (chế độ manual)
      - "<số>" VD: "26" hoặc "26.5"  → đặt nhiệt độ mục tiêu
    """
    msg = input.message.lower().strip()
    payload: dict = {}

    # Phân tích lệnh
    if "auto" in msg or "tự động" in msg:
        payload["mode"] = "auto"

    elif "manual" in msg or "thủ công" in msg:
        payload["mode"] = "manual"

    elif any(k in msg for k in ["bật", "bat", " on", "on "]):
        payload["mode"] = "manual"
        payload["relay"] = True

    elif any(k in msg for k in ["tắt", "tat", " off", "off "]):
        payload["mode"] = "manual"
        payload["relay"] = False

    else:
        # Tìm số trong câu lệnh → đặt nhiệt độ mục tiêu
        for word in msg.split():
            try:
                val = float(word)
                if 15.0 <= val <= 40.0:  # Giới hạn nhiệt độ hợp lý
                    payload["target"] = val
                    break
            except ValueError:
                continue

    if payload:
        mqtt.publish("iot/device/cmd", json.dumps(payload))
        return {"status": "success", "command": payload}

    return {"status": "unknown_command", "command": {}, "hint": "Thu: 'bat', 'tat', 'auto', 'manual', hoac nhap nhiet do (VD: 26)"}

# ============================================================
# WebSocket endpoint: frontend kết nối vào đây
# ============================================================
@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await manager.connect(websocket)
    try:
        while True:
            # Giữ kết nối mở, nhận ping từ client nếu có
            await websocket.receive_text()
    except WebSocketDisconnect:
        manager.disconnect(websocket)

# ============================================================
# Health check endpoint
# ============================================================
@app.get("/health")
async def health_check():
    return {
        "status": "ok",
        "websocket_clients": len(manager.active_connections),
    }
