
#include <WiFi.h>
#include <time.h>
#include <WebSocketsClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "config.h"

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
WebSocketsClient ws;

enum UiState {
  IDLE,
  WAITING,
  STREAMING
};

enum ConnectionState {
  WIFI_CONNECTING,
  WIFI_OFFLINE,
  RELAY_CONNECTING,
  RELAY_ONLINE
};

UiState state = IDLE;
ConnectionState connectionState = WIFI_CONNECTING;

bool wsConnected = false;
bool wsStarted = false;

String inputLine;
uint32_t sentAtMs = 0;

// Display
const int HEADER_H = 22;
const int TEXT_SIZE = 2;
const int CHAR_W = 6 * TEXT_SIZE;
const int LINE_H = 8 * TEXT_SIZE;

int curX = 0;
int curY = 0;

// Diagnostics and retry scheduling
wl_status_t lastWifiStatus = WL_IDLE_STATUS;
bool wifiStatusInitialized = false;

uint32_t lastHealthLog = 0;
uint32_t lastWifiRetry = 0;
uint32_t lastWifiDisconnectLog = 0;

const uint32_t HEALTH_LOG_INTERVAL = 5000;
const uint32_t WIFI_RETRY_INTERVAL = 10000;
const uint32_t PROMPT_TIMEOUT = 45000;

// ---------- Helpers ----------

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN_COMPLETED";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

void drawHeader(const char* msg, uint16_t color) {
  tft.fillRect(0, 0, tft.width(), HEADER_H, ILI9341_NAVY);
  tft.setTextSize(1);
  tft.setTextColor(color);
  tft.setCursor(4, 7);
  tft.print(msg);
}

void clearBody() {
  tft.fillRect(
    0,
    HEADER_H,
    tft.width(),
    tft.height() - HEADER_H,
    ILI9341_BLACK
  );

  curX = 0;
  curY = HEADER_H + 4;
}

void newLine() {
  curX = 0;
  curY += LINE_H;

  if (curY + LINE_H > tft.height()) {
    clearBody();
  }
}

void emit(char c, uint16_t color) {
  if (c == '\r') {
    return;
  }

  if (c == '\n') {
    newLine();
    return;
  }

  if (curX + CHAR_W > tft.width()) {
    newLine();
  }

  tft.setTextSize(TEXT_SIZE);
  tft.setTextColor(color, ILI9341_BLACK);
  tft.setCursor(curX, curY);
  tft.write(c);

  curX += CHAR_W;
}

void emitString(const String& s, uint16_t color) {
  for (size_t i = 0; i < s.length(); i++) {
    emit(s[i], color);
  }
}

// ---------- Wi-Fi ----------

void startWifi() {
  Serial.println("\n[WIFI] Starting station mode");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // Wokwi-GUEST, open network, channel 6.
  WiFi.begin("Wokwi-GUEST", "", 6);

  lastWifiRetry = millis();
  lastWifiStatus = WiFi.status();
  wifiStatusInitialized = true;

  connectionState = WIFI_CONNECTING;
  drawHeader("wifi connecting", ILI9341_YELLOW);
}

void monitorWifi() {
  wl_status_t current = WiFi.status();

  if (!wifiStatusInitialized || current != lastWifiStatus) {
    Serial.printf(
      "[%lu ms] [WIFI] %s -> %s\n",
      (unsigned long)millis(),
      wifiStatusInitialized
        ? wifiStatusName(lastWifiStatus)
        : "INITIAL",
      wifiStatusName(current)
    );

    lastWifiStatus = current;
    wifiStatusInitialized = true;

    if (current == WL_CONNECTED) {
      Serial.printf(
        "[WIFI] Connected | IP=%s | RSSI=%d dBm\n",
        WiFi.localIP().toString().c_str(),
        WiFi.RSSI()
      );

      if (!wsStarted) {
        connectionState = RELAY_CONNECTING;
        drawHeader("wifi ok - starting relay", ILI9341_YELLOW);
      } else if (!wsConnected) {
        connectionState = RELAY_CONNECTING;
        drawHeader("wifi ok - relay reconnect", ILI9341_YELLOW);
      }
    } else {
      wsConnected = false;
      connectionState = WIFI_OFFLINE;

      Serial.println("[WIFI] Network unavailable");

      drawHeader("wifi disconnected", ILI9341_RED);
    }
  }

  // Retry Wi-Fi without restarting the ESP32.
  if (current != WL_CONNECTED &&
      millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL) {

    lastWifiRetry = millis();

    Serial.printf(
      "[WIFI] Retry requested; status=%s\n",
      wifiStatusName(current)
    );

    WiFi.reconnect();
  }
}

// ---------- WebSocket ----------

void startWebSocket() {
  if (wsStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.printf(
    "[WS] Initializing %s:%d%s\n",
    RELAY_HOST,
    RELAY_PORT,
    RELAY_PATH
  );

#if USE_TLS
  Serial.println("[WS] Transport: WSS");
  ws.beginSSL(RELAY_HOST, RELAY_PORT, RELAY_PATH);
#else
  Serial.println("[WS] Transport: WS");
  ws.begin(RELAY_HOST, RELAY_PORT, RELAY_PATH);
#endif

  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 3000, 2);

  wsStarted = true;
  connectionState = RELAY_CONNECTING;

  Serial.println("[WS] Client initialized");
  drawHeader("relay connecting", ILI9341_YELLOW);
}

// ---------- WebSocket events ----------

void onWsEvent(WStype_t type, uint8_t* p, size_t len) {
  Serial.printf(
    "[%lu ms] [WS] Event=%d | WiFi=%s\n",
    (unsigned long)millis(),
    (int)type,
    wifiStatusName(WiFi.status())
  );

  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      connectionState = RELAY_ONLINE;

      Serial.println("[WS] Connected successfully");

      if (p != nullptr && len > 0) {
        Serial.printf("[WS] Info: %.*s\n", (int)len, (char*)p);
      }

      drawHeader("ready", ILI9341_GREEN);
      break;

    case WStype_DISCONNECTED:
      wsConnected = false;
      state = IDLE;

      if (WiFi.status() == WL_CONNECTED) {
        connectionState = RELAY_CONNECTING;

        Serial.println(
          "[WS] Disconnected while Wi-Fi remains connected"
        );

        drawHeader("relay offline - retrying", ILI9341_YELLOW);
      } else {
        connectionState = WIFI_OFFLINE;

        Serial.println(
          "[WS] Disconnected; Wi-Fi is also offline"
        );

        drawHeader("wifi offline", ILI9341_RED);
      }

      break;

    case WStype_ERROR:
      Serial.println("[WS] ERROR event");

      if (p != nullptr && len > 0) {
        Serial.printf("[WS] Error data: %.*s\n", (int)len, (char*)p);
      }

      // ERROR alone does not necessarily mean the connection
      // has been fully disconnected.
      break;

    case WStype_TEXT:
      if (len == 0 || p == nullptr) {
        break;
      }

      if (p[0] == 'C') {
        if (state != STREAMING) {
          state = STREAMING;

          drawHeader("replying", ILI9341_CYAN);

          Serial.printf(
            "[AI] First chunk after %lu ms\n",
            (unsigned long)(millis() - sentAtMs)
          );
        }

        for (size_t i = 1; i < len; i++) {
          emit((char)p[i], ILI9341_WHITE);
        }
      } else if (p[0] == 'D') {
        if (state != IDLE) {
          Serial.printf(
            "[AI] Reply complete after %lu ms\n",
            (unsigned long)(millis() - sentAtMs)
          );
        }

        state = IDLE;

        if (wsConnected) {
          drawHeader("ready", ILI9341_GREEN);
        }
      } else if (p[0] == 'E') {
        state = IDLE;

        newLine();

        for (size_t i = 1; i < len; i++) {
          emit((char)p[i], ILI9341_RED);
        }

        drawHeader("server error", ILI9341_RED);

        Serial.println("[AI] Server returned an error");
      } else {
        Serial.printf(
          "[WS] Unknown text frame prefix: 0x%02X\n",
          p[0]
        );
      }

      break;

    case WStype_BIN:
      Serial.printf("[WS] Binary frame: %u bytes\n", (unsigned)len);
      break;

    case WStype_PING:
      Serial.println("[WS] Ping received");
      break;

    case WStype_PONG:
      Serial.println("[WS] Pong received");
      break;

    default:
      Serial.printf("[WS] Unhandled event: %d\n", (int)type);
      break;
  }
}

// ---------- Prompt handling ----------

void submitPrompt(const String& text) {
  if (text.length() == 0) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[PROMPT] Rejected: Wi-Fi offline");
    drawHeader("wifi offline", ILI9341_RED);
    return;
  }

  if (!wsConnected || !ws.isConnected()) {
    Serial.println("[PROMPT] Rejected: relay not connected");
    drawHeader("relay not connected", ILI9341_YELLOW);
    return;
  }

  if (state != IDLE) {
    Serial.println("[PROMPT] Rejected: another request is active");
    return;
  }

  clearBody();

  emitString("> " + text, ILI9341_CYAN);
  newLine();
  newLine();

  String frame = "P" + text;

  if (!ws.sendTXT(frame)) {
    Serial.println("[PROMPT] Failed to queue WebSocket frame");
    drawHeader("send failed", ILI9341_RED);
    return;
  }

  sentAtMs = millis();
  state = WAITING;

  Serial.printf(
    "[PROMPT] Sent %u characters\n",
    (unsigned)text.length()
  );

  drawHeader("waiting for reply", ILI9341_YELLOW);
}

// ---------- Health monitoring ----------

void monitorConnections() {
  if (millis() - lastHealthLog < HEALTH_LOG_INTERVAL) {
    return;
  }

  lastHealthLog = millis();

  wl_status_t wifi = WiFi.status();

  Serial.printf(
    "[HEALTH] wifi=%s ws_started=%d ws_connected=%d "
    "ws_isConnected=%d state=%d heap=%u maxBlock=%u",
    wifiStatusName(wifi),
    wsStarted,
    wsConnected,
    wsStarted ? ws.isConnected() : false,
    (int)state,
    ESP.getFreeHeap(),
    ESP.getMaxAllocHeap()
  );

  if (wifi == WL_CONNECTED) {
    Serial.printf(
      " ip=%s rssi=%d",
      WiFi.localIP().toString().c_str(),
      WiFi.RSSI()
    );
  }

  Serial.println();
}

// ---------- Setup ----------

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32 TFT AI Client");
  Serial.println("Connection diagnostics enabled");
  Serial.println("================================");

  tft.begin(40000000);
  tft.setRotation(TFT_ROTATION);
  tft.setTextWrap(false);
  tft.fillScreen(ILI9341_BLACK);

  drawHeader("booting", ILI9341_YELLOW);
  clearBody();

  Serial.printf("[BOOT] Chip model: %s\n", ESP.getChipModel());
  Serial.printf("[BOOT] CPU frequency: %u MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("[BOOT] Free heap: %u bytes\n", ESP.getFreeHeap());

  startWifi();

  Serial.println("[BOOT] Type a prompt and press Enter.");
}

// ---------- Main loop ----------

void loop() {
  // Start WebSocket only after Wi-Fi is available.
  // The WebSocket library handles subsequent reconnects.
  monitorWifi();

  if (WiFi.status() == WL_CONNECTED && !wsStarted) {
    startWebSocket();
  }

  // Must be called frequently for connection management,
  // heartbeat, and incoming frames.
  if (wsStarted) {
    ws.loop();
  }

  monitorConnections();

  // Serial input
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (c == '\n') {
      inputLine.trim();

      submitPrompt(inputLine);
      inputLine = "";
    } else if (c != '\r' && inputLine.length() < 500) {
      inputLine += c;
    }
  }

  // Thinking animation
  static uint32_t lastDot = 0;
  static uint8_t dots = 0;

  if (state == WAITING && millis() - lastDot >= 350) {
    lastDot = millis();
    dots = (dots + 1) % 4;

    char buf[16] = "thinking";

    for (uint8_t i = 0; i < dots; i++) {
      strcat(buf, ".");
    }

    drawHeader(buf, ILI9341_YELLOW);
  }

  // Request timeout
  if (state != IDLE &&
      millis() - sentAtMs > PROMPT_TIMEOUT) {

    Serial.println("[AI] Request timed out");

    state = IDLE;
    newLine();

    emitString("[timeout]", ILI9341_RED);

    if (WiFi.status() != WL_CONNECTED) {
      drawHeader("wifi offline", ILI9341_RED);
    } else if (!wsConnected) {
      drawHeader("relay disconnected", ILI9341_YELLOW);
    } else {
      drawHeader("reply timeout", ILI9341_RED);
    }
  }
}

