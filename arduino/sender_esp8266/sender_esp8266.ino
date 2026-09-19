/**
 * ════════════════════════════════════════════════════════════════════════
 *  SENDER  —  NodeMCU ESP8266  (board: NodeMCU 1.0 / ESP-12E)
 *
 *  Responsibilities:
 *    • Runs Wi-Fi AP ("MotorControl") for the Android app
 *    • Serves JSON REST API:
 *        GET  /api/status        → motorOn, current, isRunning,
 *                                   waterDetected, espNowConnected
 *        POST /api/motor/on|off  → send ESP-NOW command to Receiver
 *        GET  /api/logs          → ring-buffer of ESP serial log lines
 *        GET  /api/alerts        → (stub) empty alert array
 *        POST /api/alerts/clear  → (stub)
 *    • Sends motor ON/OFF to Receiver via ESP-NOW broadcast (3× for reliability)
 *    • Receives status packets (type 0x02) from Receiver:
 *        current, motorOn (confirmed), waterDetected
 *    • Receives log packets (type 0x03) from Receiver → appended to ring-buffer
 *    • Physical push-buttons: D1=ON, D2=OFF (INPUT_PULLUP, debounced 50 ms)
 *    • LEDs: D3=green (motor ON), GPIO3/RX=red (motor OFF)
 *      → Active LED blinks briefly on each ESP-NOW packet received (link heartbeat)
 *    • Log ring-buffer: 30 entries × 90 chars, served via GET /api/logs
 *    • espNowConnected = true if status packet received within last 10 s
 *
 *  Transport: ESP-NOW broadcast (FF:FF:FF:FF:FF:FF)
 *  LoRa SX1278 wiring present but kept as DEAD CODE.
 *
 *  Libraries (install via Arduino Library Manager):
 *    — ESP8266WiFi      (bundled with ESP8266 Arduino core)
 *    — ESP8266WebServer (bundled with ESP8266 Arduino core)
 *    — espnow           (bundled with ESP8266 Arduino core SDK)
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>   // wifi_set_phy_mode, PHY_MODE_11B
}

// ── LoRa SX1278 — DEAD CODE (wired, not used until LoRa phase) ──────────
// #include <SPI.h>
// #include <LoRa.h>
// #define LORA_RST   D0   // GPIO16
// #define LORA_DIO0  D4   // GPIO2
// #define LORA_SCK   D5   // GPIO14
// #define LORA_MISO  D6   // GPIO12
// #define LORA_MOSI  D7   // GPIO13
// #define LORA_NSS   D8   // GPIO15
// #define LORA_FREQ  433E6
// ─────────────────────────────────────────────────────────────────────────

// ── Pin definitions ───────────────────────────────────────────────────────
#define PIN_BTN_ON   D1   // GPIO5  — Button ON  → GND (INPUT_PULLUP)
#define PIN_BTN_OFF  D2   // GPIO4  — Button OFF → GND (INPUT_PULLUP)
#define PIN_LED_ON   D3   // GPIO0  — Green LED + 220 Ω  (motor ON indicator)
#define PIN_LED_OFF  3    // GPIO3 (RX) — Red LED + 220 Ω (motor OFF indicator)
                          //   Serial opened SERIAL_TX_ONLY → GPIO3 free as output

// ── Wi-Fi AP settings ─────────────────────────────────────────────────────
static const char*   AP_SSID = "MotorControl";
static const char*   AP_PASS = "motor1234";
static const uint8_t WIFI_CH = 1;    // must match receiver

// ── ESP-NOW ───────────────────────────────────────────────────────────────
static uint8_t BCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Command packet  Sender → Receiver  (type 0x01)
typedef struct __attribute__((packed)) {
  uint8_t  type;   // 0x01
  uint8_t  cmd;    // 0x01=ON  0x02=OFF
  uint32_t seq;
} CmdPacket;

// Status packet  Receiver → Sender  (type 0x02) — extended with waterDetected
typedef struct __attribute__((packed)) {
  uint8_t  type;          // 0x02
  float    current;       // ACS712 reading (A)
  uint8_t  motorOn;       // relay confirmed state
  uint8_t  waterDetected; // 1 = water at pipe end, 0 = dry
  uint32_t seq;
} StatusPacket;

// Log packet  Receiver → Sender  (type 0x03)
typedef struct __attribute__((packed)) {
  uint8_t type;    // 0x03
  char    msg[59]; // null-terminated forwarded serial log line
} LogPacket;

// ── Log ring-buffer ────────────────────────────────────────────────────────
#define LOG_ENTRIES  30
#define LOG_LEN      90

static char    g_log[LOG_ENTRIES][LOG_LEN];
static uint8_t g_logHead  = 0;    // next write slot (wraps)
static uint8_t g_logCount = 0;    // valid entries (saturates at LOG_ENTRIES)

static void logAdd(const char* msg) {
  Serial.println(msg);
  strncpy(g_log[g_logHead], msg, LOG_LEN - 1);
  g_log[g_logHead][LOG_LEN - 1] = '\0';
  g_logHead = (g_logHead + 1) % LOG_ENTRIES;
  if (g_logCount < LOG_ENTRIES) g_logCount++;
}

// snprintf wrapper for logAdd
static void logFmt(const char* fmt, ...) {
  char buf[LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  logAdd(buf);
}

// ── State ─────────────────────────────────────────────────────────────────
static volatile bool  g_motorOn       = false;
static volatile float g_currentA      = 0.0f;
static volatile bool  g_waterDetected = false;
static uint32_t       g_cmdSeq        = 0;
static unsigned long  g_lastRecvMs    = 0;   // millis() of last 0x02 status recv

// ── Button debounce ───────────────────────────────────────────────────────
static unsigned long g_btnOnLast  = 0;
static unsigned long g_btnOffLast = 0;
#define DEBOUNCE_MS 50UL

// ── Blink (non-blocking) ──────────────────────────────────────────────────
//   On each ESP-NOW packet received, briefly dark-flash the active LED so
//   the user can see the RF link is alive without disturbing normal indication.
#define BLINK_ON_MS   80UL
#define BLINK_OFF_MS 120UL

static volatile bool g_blinkRequest = false;
static unsigned long g_blinkPhaseMs = 0;
static uint8_t       g_blinkPhase   = 0;  // 0=idle  1=LEDs-off  2=restore

// ── HTTP server ───────────────────────────────────────────────────────────
static ESP8266WebServer server(80);

// ═════════════════════════════════════════════════════════════════════════
//  LED helpers
// ═════════════════════════════════════════════════════════════════════════

static void updateLEDs() {
  digitalWrite(PIN_LED_ON,  g_motorOn ? HIGH : LOW);
  digitalWrite(PIN_LED_OFF, g_motorOn ? LOW  : HIGH);
}

// ═════════════════════════════════════════════════════════════════════════
//  Non-blocking blink — briefly dims active LED on ESP-NOW event
// ═════════════════════════════════════════════════════════════════════════

static void handleBlink() {
  unsigned long now = millis();
  if (g_blinkRequest && g_blinkPhase == 0) {
    g_blinkRequest = false;
    g_blinkPhase   = 1;
    g_blinkPhaseMs = now;
    digitalWrite(PIN_LED_ON,  LOW);   // briefly darken both
    digitalWrite(PIN_LED_OFF, LOW);
  }
  if (g_blinkPhase == 1 && (now - g_blinkPhaseMs) >= BLINK_ON_MS) {
    g_blinkPhase   = 2;
    g_blinkPhaseMs = now;
    updateLEDs();   // restore correct motor-state LED
  }
  if (g_blinkPhase == 2 && (now - g_blinkPhaseMs) >= BLINK_OFF_MS) {
    g_blinkPhase = 0;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ═════════════════════════════════════════════════════════════════════════

void espnowOnSend(uint8_t* /*mac*/, uint8_t status) { (void)status; }

void espnowOnRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
  if (len < 1) return;

  switch (data[0]) {

    case 0x02: {   // StatusPacket from Receiver
      if (len < sizeof(StatusPacket)) return;
      StatusPacket pkt;
      memcpy(&pkt, data, sizeof(pkt));
      g_currentA      = pkt.current;
      g_motorOn       = (pkt.motorOn != 0);       // sync to receiver's confirmed state
      g_waterDetected = (pkt.waterDetected != 0);
      g_lastRecvMs    = millis();
      g_blinkRequest  = true;
      break;
    }

    case 0x03: {   // LogPacket forwarded from Receiver
      if (len < 2) return;
      LogPacket pkt;
      memcpy(&pkt, data, min((size_t)len, sizeof(pkt)));
      pkt.msg[sizeof(pkt.msg) - 1] = '\0';
      logAdd(pkt.msg);
      g_blinkRequest = true;
      break;
    }

    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Motor command helper
// ═════════════════════════════════════════════════════════════════════════

static void sendMotorCommand(bool on) {
  CmdPacket pkt;
  pkt.type = 0x01;
  pkt.cmd  = on ? 0x01 : 0x02;
  pkt.seq  = ++g_cmdSeq;

  logFmt("[SEND] Motor %s  seq=%lu", on ? "ON" : "OFF", (unsigned long)g_cmdSeq);

  // 3 retries, 12 ms apart — ESP-NOW is fire-and-forget; improves reliability
  for (uint8_t i = 0; i < 3; i++) {
    esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
    delay(12);
  }

  g_motorOn = on;
  updateLEDs();
}

// ═════════════════════════════════════════════════════════════════════════
//  HTTP helpers
// ═════════════════════════════════════════════════════════════════════════

static void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static inline bool isEspNowConnected() {
  return (g_lastRecvMs > 0) && (millis() - g_lastRecvMs < 10000UL);
}

// ── /api/status ───────────────────────────────────────────────────────────
static String buildStatusJson() {
  bool running = g_motorOn || (g_currentA > 0.10f);
  String j = "{";
  j += "\"motorOn\":"         + String(g_motorOn         ? "true" : "false") + ",";
  j += "\"current\":"         + String(g_currentA, 2)                        + ",";
  j += "\"isRunning\":"       + String(running            ? "true" : "false") + ",";
  j += "\"waterDetected\":"   + String(g_waterDetected    ? "true" : "false") + ",";
  j += "\"espNowConnected\":" + String(isEspNowConnected() ? "true" : "false");
  j += "}";
  return j;
}

void handleStatus()    { addCorsHeaders(); server.send(200, "application/json", buildStatusJson()); }
void handleMotorOn()   { sendMotorCommand(true);  addCorsHeaders(); server.send(200, "application/json", "{\"success\":true}"); }
void handleMotorOff()  { sendMotorCommand(false); addCorsHeaders(); server.send(200, "application/json", "{\"success\":true}"); }
void handleAlerts()    { addCorsHeaders(); server.send(200, "application/json", "{\"alerts\":[]}"); }
void handleAlertsClear(){ addCorsHeaders(); server.send(200, "application/json", "{\"success\":true}"); }
void handleOptions()   { addCorsHeaders(); server.send(204); }
void handleNotFound()  { server.send(404,  "application/json", "{\"error\":\"not found\"}"); }

// ── /api/logs ─────────────────────────────────────────────────────────────
void handleLogs() {
  addCorsHeaders();
  String j = "{\"logs\":[";
  // Ring-buffer: oldest entry is at (g_logHead) when full, at 0 when not yet full
  uint8_t start = (g_logCount < LOG_ENTRIES) ? 0 : g_logHead;
  for (uint8_t i = 0; i < g_logCount; i++) {
    uint8_t idx = (start + i) % LOG_ENTRIES;
    if (i > 0) j += ',';
    j += '"';
    for (const char* p = g_log[idx]; *p; p++) {
      if      (*p == '"')  j += "\\\"";
      else if (*p == '\\') j += "\\\\";
      else                 j += *p;
    }
    j += '"';
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// ═════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════

void setup() {
  // TX-only serial — frees GPIO3 (RX pin) for the red LED output
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(100);
  logAdd("[SEND] boot");

  // ── GPIO ──────────────────────────────────────────────────────────────
  pinMode(PIN_BTN_ON,  INPUT_PULLUP);
  pinMode(PIN_BTN_OFF, INPUT_PULLUP);
  pinMode(PIN_LED_ON,  OUTPUT);
  pinMode(PIN_LED_OFF, OUTPUT);
  updateLEDs();   // red ON, green OFF at boot (motor OFF)

  // ── Wi-Fi AP ──────────────────────────────────────────────────────────
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, WIFI_CH);
  WiFi.setOutputPower(20.5f);         // max legal TX power
  wifi_set_phy_mode(PHY_MODE_11B);    // 802.11b: ~3× longer range vs g/n

  logFmt("[SEND] AP up  IP=%s  SSID=%s", WiFi.softAPIP().toString().c_str(), AP_SSID);

  // ── ESP-NOW ───────────────────────────────────────────────────────────
  if (esp_now_init() != 0) {
    logAdd("[SEND] ESP-NOW init FAILED — reboot");
    delay(2000);
    ESP.restart();
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(espnowOnSend);
  esp_now_register_recv_cb(espnowOnRecv);
  esp_now_add_peer(BCAST_MAC, ESP_NOW_ROLE_COMBO, WIFI_CH, nullptr, 0);
  logAdd("[SEND] ESP-NOW ready (broadcast)");

  // ── LoRa DEAD CODE ────────────────────────────────────────────────────
  // LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  // if (!LoRa.begin(LORA_FREQ)) { logAdd("LoRa FAIL"); }
  // else { LoRa.setSpreadingFactor(12); LoRa.setTxPower(20); }
  // ─────────────────────────────────────────────────────────────────────

  // ── HTTP routes ────────────────────────────────────────────────────────
  server.on("/api/status",        HTTP_GET,    handleStatus);
  server.on("/api/motor/on",      HTTP_POST,   handleMotorOn);
  server.on("/api/motor/off",     HTTP_POST,   handleMotorOff);
  server.on("/api/logs",          HTTP_GET,    handleLogs);
  server.on("/api/alerts",        HTTP_GET,    handleAlerts);
  server.on("/api/alerts/clear",  HTTP_POST,   handleAlertsClear);
  server.on("/api/status",        HTTP_OPTIONS,handleOptions);
  server.on("/api/motor/on",      HTTP_OPTIONS,handleOptions);
  server.on("/api/motor/off",     HTTP_OPTIONS,handleOptions);
  server.on("/api/logs",          HTTP_OPTIONS,handleOptions);
  server.onNotFound(handleNotFound);
  server.begin();

  logAdd("[SEND] HTTP :80 ready — connect phone to MotorControl / motor1234");
}

// ═════════════════════════════════════════════════════════════════════════
//  Main loop
// ═════════════════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // ── Physical button: ON ────────────────────────────────────────────────
  if (digitalRead(PIN_BTN_ON) == LOW) {
    if ((now - g_btnOnLast) > DEBOUNCE_MS) {
      g_btnOnLast = now;
      if (!g_motorOn) {
        logAdd("[SEND] BTN → Motor ON");
        sendMotorCommand(true);
      }
    }
  } else { g_btnOnLast = 0; }

  // ── Physical button: OFF ───────────────────────────────────────────────
  if (digitalRead(PIN_BTN_OFF) == LOW) {
    if ((now - g_btnOffLast) > DEBOUNCE_MS) {
      g_btnOffLast = now;
      if (g_motorOn) {
        logAdd("[SEND] BTN → Motor OFF");
        sendMotorCommand(false);
      }
    }
  } else { g_btnOffLast = 0; }

  handleBlink();
}
