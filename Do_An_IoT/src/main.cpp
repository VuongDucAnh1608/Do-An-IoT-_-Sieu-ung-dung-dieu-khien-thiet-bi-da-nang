/**
 * Do An IoT - ESP32 Firmware
 * Chức năng:
 *   - Đọc nhiệt độ/độ ẩm từ cảm biến DHT22
 *   - Điều khiển relay theo chế độ manual/auto
 *   - Giao tiếp với backend qua giao thức MQTT
 *
 * Hardware:
 *   - ESP32 DevKit
 *   - DHT22 nối vào GPIO 4
 *   - Relay  nối vào GPIO 5
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ============================================================
// Cấu hình mạng – thay bằng thông tin thực tế của bạn
// ============================================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ============================================================
// Cấu hình MQTT Broker
// ============================================================
const char*    MQTT_BROKER = "YOUR_SERVER_IP";   // IP hoặc hostname của VPS/PC chạy Mosquitto
const uint16_t MQTT_PORT   = 1883;
const char*    MQTT_USER   = "mqtt_user";        // Phải khớp với cấu hình Mosquitto
const char*    MQTT_PASS   = "mqtt_password";
const char*    CLIENT_ID   = "esp32_sensor_01";  // Mỗi thiết bị phải có ID duy nhất

// ============================================================
// MQTT Topics
// ============================================================
const char* TOPIC_DATA   = "iot/sensor/data";   // ESP32 -> Backend: gửi đo lường
const char* TOPIC_CMD    = "iot/device/cmd";    // Backend -> ESP32: nhận lệnh
const char* TOPIC_STATUS = "iot/sensor/status"; // ESP32 -> Backend: trạng thái thiết bị

// ============================================================
// Cấu hình phần cứng
// ============================================================
#define RELAY_PIN  5
#define DHT_PIN    4
#define DHT_TYPE   DHT22

// ============================================================
// Khoảng thời gian gửi dữ liệu cảm biến (ms)
// ============================================================
const unsigned long SEND_INTERVAL = 15000; // 15 giây

// ============================================================
// Biến toàn cục
// ============================================================
WiFiClient    espClient;
PubSubClient  mqttClient(espClient);
DHT           dht(DHT_PIN, DHT_TYPE);

unsigned long lastSendTime = 0;
String        mode         = "manual";  // "manual" hoặc "auto"
float         targetTemp   = 25.0;      // Ngưỡng nhiệt độ mục tiêu ở chế độ auto
bool          relayState   = false;     // true = relay đang BẬT

// ============================================================
// Kết nối WiFi (có timeout 30 giây)
// ============================================================
void connectWiFi() {
  Serial.print("Dang ket noi WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > 30000) {
      Serial.println("\nKet noi WiFi that bai! Khoi dong lai...");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nWiFi OK. IP: ");
  Serial.println(WiFi.localIP());
}

// ============================================================
// Publish trạng thái hiện tại của thiết bị lên topic STATUS
// ============================================================
void publishStatus() {
  float currentTemp = dht.readTemperature();

  JsonDocument doc;
  doc["mode"]   = mode;
  doc["target"] = targetTemp;
  doc["relay"]  = relayState;
  doc["temp"]   = isnan(currentTemp) ? 0.0 : currentTemp;

  char buf[256];
  serializeJson(doc, buf);
  mqttClient.publish(TOPIC_STATUS, buf, true); // retain = true để client mới nhận ngay trạng thái
  Serial.print("Published STATUS: ");
  Serial.println(buf);
}

// ============================================================
// Xử lý logic tự động: bật/tắt relay theo ngưỡng nhiệt độ
// Dải hysteresis ±0.5°C để tránh relay đóng/ngắt liên tục
// ============================================================
void handleAutoLogic() {
  if (mode != "auto") return;

  float t = dht.readTemperature();
  if (isnan(t)) return;

  if (t > targetTemp + 0.5f && !relayState) {
    // Nhiệt độ quá cao → bật relay (VD: quạt/điều hòa)
    digitalWrite(RELAY_PIN, HIGH);
    relayState = true;
    publishStatus();
    Serial.println("AUTO: Bat relay (nhiet do cao)");
  } else if (t < targetTemp - 0.5f && relayState) {
    // Nhiệt độ đã đủ mát → tắt relay
    digitalWrite(RELAY_PIN, LOW);
    relayState = false;
    publishStatus();
    Serial.println("AUTO: Tat relay (nhiet do on dinh)");
  }
}

// ============================================================
// Callback nhận lệnh từ MQTT topic CMD
// Payload JSON hợp lệ:
//   {"mode": "auto"}
//   {"mode": "manual", "relay": true}
//   {"target": 26.0}
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Chuyển payload bytes sang String
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("MQTT CMD nhan duoc: ");
  Serial.println(msg);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.print("JSON parse loi: ");
    Serial.println(err.c_str());
    return;
  }

  // Cập nhật chế độ hoạt động
  if (doc["mode"].is<const char*>()) {
    mode = doc["mode"].as<String>();
  }

  // Cập nhật nhiệt độ mục tiêu
  if (doc["target"].is<float>()) {
    targetTemp = doc["target"].as<float>();
  }

  // Điều khiển relay thủ công (chỉ hiệu lực ở chế độ manual)
  if (doc["relay"].is<bool>() && mode == "manual") {
    relayState = doc["relay"].as<bool>();
    digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
    Serial.print("MANUAL: Relay -> ");
    Serial.println(relayState ? "BAT" : "TAT");
  }

  publishStatus(); // Phản hồi trạng thái mới ngay sau khi xử lý lệnh
}

// ============================================================
// Kết nối MQTT Broker (có retry không vô hạn)
// ============================================================
void connectMQTT() {
  int retries = 0;
  while (!mqttClient.connected() && retries < 5) {
    Serial.print("Dang ket noi MQTT...");
    if (mqttClient.connect(CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println(" OK!");
      mqttClient.subscribe(TOPIC_CMD);  // Đăng ký nhận lệnh
      publishStatus();                  // Gửi trạng thái ban đầu
    } else {
      Serial.print(" Loi rc=");
      Serial.println(mqttClient.state());
      retries++;
      delay(3000);
    }
  }
  // Nếu vẫn không kết nối được, khởi động lại ESP32
  if (!mqttClient.connected()) {
    Serial.println("MQTT ket noi that bai! Khoi dong lai...");
    ESP.restart();
  }
}

// ============================================================
// Publish dữ liệu cảm biến lên topic DATA
// ============================================================
void publishSensorData() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    Serial.println("Doc cam bien DHT22 loi!");
    return;
  }

  JsonDocument doc;
  doc["temp"] = t;
  doc["hum"]  = h;

  char buf[128];
  serializeJson(doc, buf);
  mqttClient.publish(TOPIC_DATA, buf);

  Serial.print("Sensor DATA: ");
  Serial.println(buf);
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Khởi tạo chân relay ở trạng thái TẮT
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // Khởi tạo cảm biến DHT
  dht.begin();

  // Kết nối mạng
  connectWiFi();

  // Cấu hình MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512); // Tăng buffer cho JSON lớn hơn

  // Kết nối MQTT
  connectMQTT();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  // Tự kết nối lại nếu bị ngắt
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  // Kiểm tra logic tự động
  handleAutoLogic();

  // Gửi dữ liệu cảm biến định kỳ
  if (millis() - lastSendTime >= SEND_INTERVAL) {
    publishSensorData();
    lastSendTime = millis();
  }
}
