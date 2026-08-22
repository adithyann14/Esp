/**
 * ════════════════════════════════════════════════════════════════════════
 *  SENDER  —  NodeMCU ESP8266  (board: NodeMCU 1.0 / ESP-12E)
 *
 *  Responsibilities:
 *    • Runs a Wi-Fi AP ("MotorControl") for the Android app
 *    • Serves a simple JSON REST API (GET /api/status, POST /api/motor/on|off …)
 *    • Sends motor ON/OFF commands to the Receiver via ESP-NOW broadcast
 *    • Receives current readings back from the Receiver via ESP-NOW
 *    • Physical push-buttons (D1=ON, D2=OFF) with debounce
 *    • LEDs  D3=green (motor ON) | GPIO3/RX=red (motor OFF)
 *
 *  Transport: ESP-NOW broadcast (FF:FF:FF:FF:FF:FF)
 *  LoRa SX1278 wiring is present on the board but kept as DEAD CODE.
 *
 *  Libraries required (install via Arduino Library Manager):
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
#define PIN_BTN_ON   D1     // GPIO5  — Button ON  → GND (INPUT_PULLUP)
#define PIN_BTN_OFF  D2     // GPIO4  — Button OFF → GND (INPUT_PULLUP)
#define PIN_LED_ON   D3     // GPIO0  — Green LED + 220 Ω  (motor ON indicator)
#define PIN_LED_OFF  3      // GPIO3 (RX) — Red LED + 220 Ω (motor OFF indicator)
                            //   Serial is TX-only so GPIO3 is free for LED use

// ── Wi-Fi AP settings ────────────────────────────────────────────────────
static const char* AP_SSID    = "MotorControl";
static const char* AP_PASS    = "motor1234";
static const uint8_t WIFI_CH  = 1;         // must match receiver

// ── ESP-NOW ───────────────────────────────────────────────────────────────
static uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Command packet  Sender → Receiver
typedef struct __attribute__((packed)) {
  uint8_t  type;   // 0x01
  uint8_t  cmd;    // 0x01=ON  0x02=OFF
  uint32_t seq;
} CmdPacket;

// Status packet  Receiver → Sender
typedef struct __attribute__((packed)) {
  uint8_t  type;     // 0x02
  float    current;  // ACS712 reading (A)
  uint8_t  motorOn;  // servo/relay confirmed state
  uint32_t seq;
} StatusPacket;

// ── State ─────────────────────────────────────────────────────────────────
static volatile bool  g_motorOn      = false;
static volatile float g_currentA     = 0.0f;
static uint32_t       g_cmdSeq       = 0;

// ── Button debounce ───────────────────────────────────────────────────────
static unsigned long g_btnOnLast  = 0;
static unsigned long g_btnOffLast = 0;
#define DEBOUNCE_MS  50UL

// ── HTTP server ───────────────────────────────────────────────────────────
static ESP8266WebServer server(80);

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ═════════════════════════════════════════════════════════════════════════

void espnowOnSend(uint8_t* /*mac*/, uint8_t status) {
  // Optional delivery feedback — ignored for now
  (void)status;
}

void espnowOnRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
  if (len < 1) return;
  if (data[0] == 0x02 && len >= sizeof(StatusPacket)) {
    StatusPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));
    g_currentA = pkt.current;
    // If receiver says motor is confirmed on, trust it
    if (pkt.motorOn) g_motorOn = true;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Motor command helpers
// ═════════════════════════════════════════════════════════════════════════

static void updateLEDs() {
  digitalWrite(PIN_LED_ON,  g_motorOn ? HIGH : LOW);
  digitalWrite(PIN_LED_OFF, g_motorOn ? LOW  : HIGH);
}

static void sendMotorCommand(bool on) {
  CmdPacket pkt;
  pkt.type = 0x01;
  pkt.cmd  = on ? 0x01 : 0x02;
  pkt.seq  = ++g_cmdSeq;

  // Send 3×  — ESP-NOW is fire-and-forget; retries improve reliability
  for (uint8_t i = 0; i < 3; i++) {
    esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
    delay(12);
  }

  g_motorOn = on;
  updateLEDs();
}

// ═════════════════════════════════════════════════════════════════════════
//  HTTP handlers
// ═════════════════════════════════════════════════════════════════════════

/* Builds a compact JSON status string without pulling in ArduinoJson. */
static String buildStatusJson() {
  bool running = (g_currentA > 0.1f);
  String j = "{";
  j += "\"motorOn\":"  + String(g_motorOn  ? "true" : "false") + ",";
  j += "\"current\":"  + String(g_currentA, 2) + ",";
  j += "\"isRunning\":" + String(running   ? "true" : "false");
  // voltageR / voltageY / voltageB intentionally omitted (sensors not fitted).
  // App shows "—" when the key is absent.
  j += "}";
  return j;
}

static void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleStatus() {
  addCorsHeaders();
  server.send(200, "application/json", buildStatusJson());
}

void handleMotorOn() {
  sendMotorCommand(true);
  addCorsHeaders();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleMotorOff() {
  sendMotorCommand(false);
  addCorsHeaders();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleAlerts() {
  addCorsHeaders();
  server.send(200, "application/json", "{\"alerts\":[]}");
}

void handleAlertsClear() {
  addCorsHeaders();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleOptions() {        // pre-flight CORS
  addCorsHeaders();
  server.send(204);
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ═════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════

void setup() {
  // TX-only serial — frees GPIO3 (RX) for the red LED
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(100);
  Serial.println("\n[SENDER] boot");

  // ── GPIO setup ──────────────────────────────────────────────────────
  pinMode(PIN_BTN_ON,  INPUT_PULLUP);
  pinMode(PIN_BTN_OFF, INPUT_PULLUP);
  pinMode(PIN_LED_ON,  OUTPUT);
  pinMode(PIN_LED_OFF, OUTPUT);
  updateLEDs();                 // red ON, green OFF at boot

  // ── Wi-Fi AP ─────────────────────────────────────────────────────────
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, WIFI_CH);

  // Maximum TX power for longest range
  WiFi.setOutputPower(20.5f);

  // 802.11b PHY mode: ~300 m open-air vs ~100 m for n/g
  wifi_set_phy_mode(PHY_MODE_11B);

  Serial.print("[SENDER] AP IP: ");
  Serial.println(WiFi.softAPIP());

  // ── ESP-NOW (init after WiFi) ─────────────────────────────────────────
  if (esp_now_init() != 0) {
    Serial.println("[SENDER] ESP-NOW init FAILED — rebooting");
    delay(2000);
    ESP.restart();
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(espnowOnSend);
  esp_now_register_recv_cb(espnowOnRecv);

  // Add broadcast peer on the AP channel
  esp_now_add_peer(BCAST_MAC, ESP_NOW_ROLE_COMBO, WIFI_CH, nullptr, 0);

  Serial.println("[SENDER] ESP-NOW ready (broadcast)");

  // ── LoRa DEAD CODE ────────────────────────────────────────────────────
  // LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  // if (!LoRa.begin(LORA_FREQ)) { Serial.println("LoRa init failed"); }
  // else { LoRa.setSpreadingFactor(12); LoRa.setTxPower(20); }
  // ─────────────────────────────────────────────────────────────────────

  // ── HTTP routes ───────────────────────────────────────────────────────
  server.on("/api/status",        HTTP_GET,    handleStatus);
  server.on("/api/motor/on",      HTTP_POST,   handleMotorOn);
  server.on("/api/motor/off",     HTTP_POST,   handleMotorOff);
  server.on("/api/alerts",        HTTP_GET,    handleAlerts);
  server.on("/api/alerts/clear",  HTTP_POST,   handleAlertsClear);
  server.on("/api/status",        HTTP_OPTIONS,handleOptions);
  server.on("/api/motor/on",      HTTP_OPTIONS,handleOptions);
  server.on("/api/motor/off",     HTTP_OPTIONS,handleOptions);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("[SENDER] HTTP server started on :80");
  Serial.println("[SENDER] ready — connect phone to \"MotorControl\" / motor1234");
}

// ═════════════════════════════════════════════════════════════════════════
//  Main loop
// ═════════════════════════════════════════════════════════════════════════

void loop() {
  // ── HTTP clients ──────────────────────────────────────────────────
  server.handleClient();

  // ── Physical buttons ──────────────────────────────────────────────
  unsigned long now = millis();

  if (digitalRead(PIN_BTN_ON) == LOW) {
    if ((now - g_btnOnLast) > DEBOUNCE_MS) {
      g_btnOnLast = now;
      if (!g_motorOn) {
        Serial.println("[SENDER] BTN → Motor ON");
        sendMotorCommand(true);
      }
    }
  } else {
    g_btnOnLast = 0;   // reset so next press triggers immediately
  }

  if (digitalRead(PIN_BTN_OFF) == LOW) {
    if ((now - g_btnOffLast) > DEBOUNCE_MS) {
      g_btnOffLast = now;
      if (g_motorOn) {
        Serial.println("[SENDER] BTN → Motor OFF");
        sendMotorCommand(false);
      }
    }
  } else {
    g_btnOffLast = 0;
  }
}
