# Do-An-IoT-_-Sieu-ung-dung-dieu-khien-thiet-bi-da-nang
# Dùng ESP32 - cảm biến DHT11 - màn hình OLED - chatbot XiaoZhi - giao thức MCP
> Tài liệu kỹ thuật chi tiết về kiến trúc phần mềm, luồng xử lý logic và giao thức MCP tích hợp AI Chatbot XiaoZhi cho hệ thống IoT.
# Kiến trúc Phần mềm (Software Architecture)
> Hệ thống phần mềm được thiết kế theo mô hình phân tầng, tách biệt rõ ràng giữa Tầng Giao Tiếp (Communication Layer) và Tầng Ứng Dụng (Application Layer) để đảm bảo tính mở rộng và bảo trì.
#1. Khối Giao Tiếp MCP (lib/WebSocketMCP)
- Đây là lớp trừu tượng đóng gói giao thức WebSocket và chuẩn JSON-RPC 2.0. Khối này chịu trách nhiệm duy trì kết nối an toàn (SSL/TLS) với server XiaoZhi và xử lý các yêu cầu điều khiển từ AI.
  + File WebSocketMCP.h: Định nghĩa cấu trúc ToolResponse, ToolParams và các interface.
  + File WebSocketMCP.cpp: Hiện thực cơ chế handshake, heartbeat (ping/pong), và cơ chế reconnect tự động.
#2. Khối Ứng Dụng & Nghiệp Vụ (src/ESP32_DHT11_OLED_MCP_CHATBOT_XIAOZHI.ino)
- Đây là file logic chính, nơi các cảm biến được đọc, dữ liệu được hiển thị và các "công cụ" (Tools) được đăng ký cho AI sử dụng.
#A.File ESP32_DHT11_OLED_MCP_CHATBOT_XIAOZHI.ino
Vai trò: Điều khiển phần cứng, xử lý vòng lặp cảm biến, và định nghĩa các API cho Chatbot.
>1. Khởi tạo hệ thống (void setup())
- Hàm này thực hiện việc boot các module. Đặc biệt, hàm mcpClient.begin() không chỉ kết nối mạng mà còn đăng ký hàm callback onConnectionStatus để xử lý logic khi kết nối thành công.
>2. Vòng lặp chính (void loop())
- Sử dụng kỹ thuật millis() để xử lý bất đồng bộ, đảm bảo hệ thống không bị block khi chờ cảm biến.
  Cập nhật dữ liệu: Đọc DHT11 mỗi 1000ms.
  Xử lý cảnh báo: So sánh temperature > warningTemp. Nếu thỏa mãn, LED sẽ nhấp nháy với tần số 300ms.
  Duy trì kết nối: Gọi mcpClient.loop() liên tục để xử lý các gói tin WebSocket đến/đi.
>3. Đăng ký công cụ AI (void registerMcpTools())
- Đây là phần quan trọng nhất của khối nghiệp vụ, cho phép AI can thiệp vào hệ thống. Code đăng ký 2 công cụ:
  Tool 1: temperature_and_humidity_values
  Mục đích: Trả về dữ liệu cảm biến cho Chatbot.
  => Logic: Đóng gói giá trị temperature và humidity vào JSON theo chuẩn MCP ToolResponse.
  Tool 2: set_temperature_warning
  Mục đích: Cho phép Chatbot thay đổi ngưỡng cảnh báo nhiệt độ từ xa.
  => Logic: Parse tham số max_temperature từ JSON request và cập nhật trực tiếp biến global warningTemp. Hệ thống sẽ ngay lập tức áp dụng ngưỡng mới này trong vòng lặp loop().
#B. Phân tích Khối Giao Tiếp MCP (WebSocketMCP.cpp/h)
1. Xử lý tin nhắn JSON-RPC (handleJsonRpcMessage)
- Hàm này phân tích chuỗi JSON nhận được từ server XiaoZhi và chuyển hướng đến đúng hàm xử lý:
  + Method initialize: Phản hồi version giao thức (2024-11-05) và khả năng hỗ trợ (capabilities: { tools: {} }) để server xác nhận thiết bị tương thích.
  + Method tools/list: Duyệt qua mảng _tools (đã được đăng ký từ file .ino), trả về danh sách các API mà Chatbot có thể gọi, kèm theo mô tả (description) và định dạng dữ liệu đầu vào (inputSchema)
  + Method tools/call: Khi Chatbot quyết định gọi một công cụ:
- Tìm Tool tương ứng trong mảng _tools dựa vào tên (name).
- Thực thi hàm callback (Lambda function) đã đăng ký ở file .ino.
- Đóng gói kết quả trả về thành chuẩn JSON-RPC 2.0 và gửi ngược lại server.
2. Quản lý kết nối (loop & handleReconnect)
- Sử dụng cơ chế Exponential Backoff: Nếu mất kết nối, hệ thống sẽ thử lại sau 1s, rồi 2s, 4s... tối đa 60s. Điều này tránh gây quá tải mạng khi server bị gián đoạn.
- Heartbeat: Gửi lệnh ping định kỳ để duy trì phiên làm việc. Nếu không nhận được pong sau 120s, hệ thống tự động ngắt và kết nối lại.
- Hiện Trạng & Định Hướng Phát Triển (Current Status & Future Work)
- Lưu ý quan trọng về tính năng điều khiển:

>Hệ thống hiện tại đã hoàn thiện việc Giám sát (đo nhiệt độ) và Cảnh báo (thay đổi ngưỡng kích hoạt LED). Chatbot đã có thể giao tiếp 2 chiều với mạch để cập nhật thông số.
Chức năng Điều khiển máy lạnh thực tế cần bổ sung thêm module hồng ngoại (IR) và mã code mô phỏng Remote để hoàn thiện ở giai đoạn sau. Hiện tại, tính năng này được mô phỏng qua việc thay đổi biến warningTemp (tương đương với việc gửi lệnh điều khiển tới một controller trung tâm).

Kế hoạch nâng cấp (Roadmap):
Hardware: Tích hợp module IR Transmitter (KY-005) vào ESP32.
Software: Thêm thư viện IRremoteESP8266 và đăng ký tool mới control_ac trong registerMcpTools().
Logic: Xây dựng cơ chế "Auto-Mode" trong code main.cpp, tự động bắn lệnh hồng ngoại khi temperature > warningTemp.
