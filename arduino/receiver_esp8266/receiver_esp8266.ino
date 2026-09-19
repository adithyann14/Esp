/**
 * ════════════════════════════════════════════════════════════════════════
 *  RECEIVER  —  NodeMCU ESP8266  (board: NodeMCU 1.0 / ESP-12E)
 *
 *  Responsibilities:
 *    • Receives motor ON/OFF commands from Sender via ESP-NOW broadcast
 *    • Servo (D3/GPIO0): 90°=idle | 0°=push START | 180°=push STOP
 *      Returns to 90° after 500 ms (button-press simulation)
 *    • Relay (D1/GPIO5): HIGH=ON, LOW=OFF  ← active-HIGH (via level shifter)
 *    • ACS712 current sensor (A0): averaged over 100 samples, sent every 2 s
 *    • Water sensor (D2/GPIO4): active-LOW — LOW = water detected at pipe end
 *    • Motor auto cut-off: if no water in 30 s after motor ON → relay OFF
 *    • D4 (GPIO2, built-in LED, active-LOW): brief blink on every ESP-NOW recv
 *
 *  Transport: ESP-NOW broadcast (FF:FF:FF:FF:FF:FF)
 *  Connects to Sender's Wi-Fi AP at boot for channel sync (then stays STA).
 *
 *  DEAD CODE (wired but inactive — LoRa phase later):
 *    • LoRa SX1278  D0=RST, D4=DIO0*, D5=SCK, D6=MISO, D7=MOSI, D8=NSS
 *      (*D4 repurposed as blink LED while LoRa is dead code)
 *
 *  Libraries (install via Arduino Library Manager):
 *    — ESP8266WiFi  (bundled with ESP8266 Arduino core)
 *    — Servo        (bundled — uses ESP8266Servo)
 *    — espnow       (bundled with ESP8266 Arduino core SDK)
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <Servo.h>

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>   // wifi_set_phy_mode, PHY_MODE_11B, wifi_set_channel
}

// ── LoRa SX1278 — DEAD CODE (wired, not used until LoRa phase) ──────────
// #include <SPI.h>
// #include <LoRa.h>
// #define LORA_RST   D0   // GPIO16
// #define LORA_DIO0  D4   // GPIO2  ← repurposed as blink LED (active-LOW)
// #define LORA_SCK   D5   // GPIO14
// #define LORA_MISO  D6   // GPIO12
// #define LORA_MOSI  D7   // GPIO13
// #define LORA_NSS   D8   // GPIO15
// #define LORA_FREQ  433E6
// ─────────────────────────────────────────────────────────────────────────

// ── Pin definitions ───────────────────────────────────────────────────────
#define PIN_RELAY      D1   // GPIO5  — Relay IN: HIGH=ON (active-HIGH via level shifter)
#define PIN_WATER      D2   // GPIO4  — Water sensor: INPUT_PULLUP, LOW = water detected
#define PIN_SERVO      D3   // GPIO0  — Servo signal (boot-safe; servo does not pull LOW)
#define PIN_BLINK_LED  D4   // GPIO2  — Built-in LED (active-LOW); blink on ESP-NOW event
// D5–D8  → LoRa dead code (see above)
// A0     → ACS712 current sensor (active)

// ── Sender AP credentials (receiver joins at boot for channel sync) ────────
static const char*    AP_SSID       = "MotorControl";
static const char*    AP_PASS       = "motor1234";
static const uint8_t  WIFI_CH       = 1;        // must match sender
static const uint16_t AP_TIMEOUT_MS = 10000;

// ── ESP-NOW ───────────────────────────────────────────────────────────────
static uint8_t BCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Command packet  Sender → Receiver  (type 0x01)
typedef struct __attribute__((packed)) {
  uint8_t  type;   // 0x01
  uint8_t  cmd;    // 0x01=ON  0x02=OFF
  uint32_t seq;
} CmdPacket;

// Status packet  Receiver → Sender  (type 0x02)
typedef struct __attribute__((packed)) {
  uint8_t  type;          // 0x02
  float    current;       // ACS712 reading (A)
  uint8_t  motorOn;       // confirmed relay state
  uint8_t  waterDetected; // 1 = water at pipe end (sensor LOW), 0 = dry
  uint32_t seq;
} StatusPacket;

// Log packet  Receiver → Sender  (type 0x03)
// Lets the receiver forward serial log lines to the sender's ring buffer
// so the Android app can read them via GET /api/logs.
typedef struct __attribute__((packed)) {
  uint8_t type;    // 0x03
  char    msg[59]; // null-terminated, 59 chars + type byte = 60 ≤ 250 B limit
} LogPacket;

// ── ACS712 calibration (5 A module at 3.3 V supply) ──────────────────────
//   Zero-current voltage = VCC/2 = 1.65 V
//   Sensitivity scales with VCC: 185 mV/A × (3.3/5) ≈ 122 mV/A
//   Fine-tune ACS_ZERO_V if your board reads non-zero at no load.
static const float ACS_ZERO_V         = 1.65f;
static const float ACS_SENS_V_PER_A   = 0.122f;
static const float CURRENT_RUN_THRESH = 0.10f;   // A — above this = motor running

// ── Servo angles ──────────────────────────────────────────────────────────
#define SERVO_NEUTRAL   90
#define SERVO_START      0
#define SERVO_STOP     180
#define SERVO_HOLD_MS  500UL
#define SERVO_BACK_MS  300UL

// ── Water-timeout auto cut-off ────────────────────────────────────────────
#define WATER_TIMEOUT_MS  30000UL   // 30 s of no water after motor ON → cut off

// ── Blink timing ──────────────────────────────────────────────────────────
#define BLINK_ON_MS   80UL
#define BLINK_OFF_MS 120UL

// ── State ─────────────────────────────────────────────────────────────────
static Servo motorServo;

enum class ServoState : uint8_t { IDLE, PRESSING, RETURNING };
static ServoState    servoState  = ServoState::IDLE;
static unsigned long servoTimer  = 0;

static volatile bool     g_pendingOn  = false;
static volatile bool     g_pendingOff = false;
static volatile uint32_t g_lastSeq    = 0;
static bool              g_motorOn    = false;

// Water tracking
static bool          g_waterDetected = false;
static unsigned long g_motorOnTime   = 0;      // millis() when motor was last turned ON
static bool          g_waterTimeout  = false;  // latched after auto cut-off

// Status broadcast
static uint32_t      g_statusSeq    = 0;
static unsigned long g_lastStatusMs = 0;
#define STATUS_INTERVAL_MS 2000UL

// Blink state machine (non-blocking; set from ISR-like ESP-NOW callback)
static volatile bool g_blinkRequest = false;
static unsigned long g_blinkPhaseMs = 0;
static uint8_t       g_blinkPhase   = 0;  // 0=idle  1=LED-on  2=cooloff

// ═════════════════════════════════════════════════════════════════════════
//  Log helper — prints to serial AND forwards to Sender via ESP-NOW 0x03
// ═════════════════════════════════════════════════════════════════════════

static void sendLog(const char* msg) {
  Serial.println(msg);
  LogPacket pkt;
  pkt.type = 0x03;
  strncpy(pkt.msg, msg, sizeof(pkt.msg) - 1);
  pkt.msg[sizeof(pkt.msg) - 1] = '\0';
  esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
}

// ═════════════════════════════════════════════════════════════════════════
//  ACS712 current reading  (100 samples averaged)
// ═════════════════════════════════════════════════════════════════════════

static float readCurrentA() {
  long sum = 0;
  const int N = 100;
  for (int i = 0; i < N; i++) {
    sum += analogRead(A0);
    delayMicroseconds(100);
  }
  float v = (sum / (float)N / 1023.0f) * 3.3f;
  return (v - ACS_ZERO_V) / ACS_SENS_V_PER_A;
}

// ═════════════════════════════════════════════════════════════════════════
//  Water sensor
// ═════════════════════════════════════════════════════════════════════════

static inline bool readWater() {
  // D2/GPIO4 with INPUT_PULLUP. Sensor conducting → pulls LOW → true = water.
  return (digitalRead(PIN_WATER) == LOW);
}

// ═════════════════════════════════════════════════════════════════════════
//  Non-blocking blink (D4, active-LOW built-in LED)
// ═════════════════════════════════════════════════════════════════════════

static inline void triggerBlink() { g_blinkRequest = true; }

static void handleBlink() {
  unsigned long now = millis();
  if (g_blinkRequest && g_blinkPhase == 0) {
    g_blinkRequest = false;
    g_blinkPhase   = 1;
    g_blinkPhaseMs = now;
    digitalWrite(PIN_BLINK_LED, LOW);   // active-LOW: LOW = LED on
  }
  if (g_blinkPhase == 1 && (now - g_blinkPhaseMs) >= BLINK_ON_MS) {
    g_blinkPhase   = 2;
    g_blinkPhaseMs = now;
    digitalWrite(PIN_BLINK_LED, HIGH);  // LED off
  }
  if (g_blinkPhase == 2 && (now - g_blinkPhaseMs) >= BLINK_OFF_MS) {
    g_blinkPhase = 0;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Servo / relay helpers
// ═════════════════════════════════════════════════════════════════════════

static void activateMotor() {
  if (servoState != ServoState::IDLE) return;  // servo busy
  sendLog("[RECV] Motor ON  (servo 0deg, relay HIGH)");
  motorServo.write(SERVO_START);
  digitalWrite(PIN_RELAY, HIGH);   // active-HIGH relay ON
  g_motorOn      = true;
  g_motorOnTime  = millis();
  g_waterTimeout = false;
  servoState     = ServoState::PRESSING;
  servoTimer     = millis();
}

static void deactivateMotor() {
  if (servoState != ServoState::IDLE) return;
  sendLog("[RECV] Motor OFF  (servo 180deg, relay LOW)");
  motorServo.write(SERVO_STOP);
  digitalWrite(PIN_RELAY, LOW);   // relay OFF
  g_motorOn  = false;
  servoState = ServoState::PRESSING;
  servoTimer = millis();
}

static void handleServo() {
  unsigned long now = millis();
  switch (servoState) {
    case ServoState::PRESSING:
      if (now - servoTimer >= SERVO_HOLD_MS) {
        motorServo.write(SERVO_NEUTRAL);
        servoState = ServoState::RETURNING;
        servoTimer = now;
      }
      break;
    case ServoState::RETURNING:
      if (now - servoTimer >= SERVO_BACK_MS) {
        servoState = ServoState::IDLE;
        Serial.println("[RECV] servo → neutral/idle");
      }
      break;
    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Water-timeout watchdog — call every loop()
//  Resets the 30-second window whenever water IS detected, so the cut-off
//  only fires after a *continuous* dry spell of 30 s.
// ═════════════════════════════════════════════════════════════════════════

static void handleWaterWatchdog() {
  if (!g_motorOn || g_waterTimeout) return;

  g_waterDetected = readWater();

  if (g_waterDetected) {
    // Water flowing — keep resetting the start timestamp so the 30s window
    // restarts from the last moment water was detected.
    g_motorOnTime = millis();
  } else {
    unsigned long elapsed = millis() - g_motorOnTime;
    if (elapsed >= WATER_TIMEOUT_MS) {
      g_waterTimeout = true;
      sendLog("[RECV] WATER TIMEOUT — no water for 30s — auto OFF");
      deactivateMotor();
    } else {
      // Optional: log every 5 s while dry
      static unsigned long lastDryLog = 0;
      if (millis() - lastDryLog > 5000UL) {
        lastDryLog = millis();
        char buf[60];
        snprintf(buf, sizeof(buf), "[RECV] dry for %lu s — will cut at 30s", elapsed / 1000UL);
        Serial.println(buf);
      }
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Periodic status broadcast to Sender (every 2 s)
// ═════════════════════════════════════════════════════════════════════════

static void sendStatusPeriodic() {
  unsigned long now = millis();
  if (now - g_lastStatusMs < STATUS_INTERVAL_MS) return;
  g_lastStatusMs = now;

  float c = readCurrentA();
  g_waterDetected = readWater();

  StatusPacket pkt;
  pkt.type          = 0x02;
  pkt.current       = c;
  pkt.motorOn       = (g_motorOn || c > CURRENT_RUN_THRESH) ? 1 : 0;
  pkt.waterDetected = g_waterDetected ? 1 : 0;
  pkt.seq           = ++g_statusSeq;

  esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));

  Serial.printf("[RECV] status tx → current=%.2fA  motorOn=%d  water=%d\n",
                c, (int)pkt.motorOn, (int)pkt.waterDetected);
}

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ═════════════════════════════════════════════════════════════════════════

void espnowOnSend(uint8_t* /*mac*/, uint8_t /*status*/) {}

void espnowOnRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
  if (len < sizeof(CmdPacket)) return;

  CmdPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.type != 0x01) return;
  if (pkt.seq == g_lastSeq) return;   // deduplicate 3× retries from sender
  g_lastSeq = pkt.seq;

  if      (pkt.cmd == 0x01) g_pendingOn  = true;
  else if (pkt.cmd == 0x02) g_pendingOff = true;

  // Blink to confirm packet received (flag only — no GPIO in callback)
  g_blinkRequest = true;
}

// ═════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[RECV] boot");

  // ── GPIO ──────────────────────────────────────────────────────────────
  pinMode(PIN_RELAY,     OUTPUT);
  digitalWrite(PIN_RELAY, LOW);          // relay OFF at boot

  pinMode(PIN_WATER,     INPUT_PULLUP);  // water sensor active-LOW

  pinMode(PIN_BLINK_LED, OUTPUT);
  digitalWrite(PIN_BLINK_LED, HIGH);     // active-LOW → HIGH = off at boot

  // ── Servo ─────────────────────────────────────────────────────────────
  motorServo.attach(PIN_SERVO);
  motorServo.write(SERVO_NEUTRAL);       // 90° safe resting
  delay(300);

  // ── Wi-Fi STA — join Sender AP for channel synchronisation ────────────
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID, AP_PASS);
  WiFi.setOutputPower(20.5f);          // max TX power (20.5 dBm)
  wifi_set_phy_mode(PHY_MODE_11B);     // 802.11b: lower data-rate, longer range

  Serial.print("[RECV] connecting to Sender AP");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < AP_TIMEOUT_MS) {
    delay(200);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[RECV] joined AP  IP=%s  ch=%d\n",
                  WiFi.localIP().toString().c_str(), (int)WiFi.channel());
  } else {
    // Sender may not be up yet — force the shared channel and continue
    Serial.println("\n[RECV] AP not found — forcing channel " + String(WIFI_CH));
    WiFi.disconnect();
    wifi_set_channel(WIFI_CH);
  }

  // ── ESP-NOW ───────────────────────────────────────────────────────────
  if (esp_now_init() != 0) {
    Serial.println("[RECV] ESP-NOW init FAILED — rebooting in 2 s");
    delay(2000);
    ESP.restart();
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(espnowOnSend);
  esp_now_register_recv_cb(espnowOnRecv);
  esp_now_add_peer(BCAST_MAC, ESP_NOW_ROLE_COMBO, WIFI_CH, nullptr, 0);
  Serial.println("[RECV] ESP-NOW ready (broadcast on ch " + String(WIFI_CH) + ")");

  // ── LoRa DEAD CODE ────────────────────────────────────────────────────
  // LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  // if (!LoRa.begin(LORA_FREQ)) { Serial.println("LoRa FAIL"); }
  // else { LoRa.setSpreadingFactor(12); LoRa.setTxPower(20); }
  // ─────────────────────────────────────────────────────────────────────

  // Boot-complete indicator: 3 quick blinks
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_BLINK_LED, LOW);  delay(80);
    digitalWrite(PIN_BLINK_LED, HIGH); delay(120);
  }

  Serial.println("[RECV] ready");
  Serial.println("[RECV]   relay=active-HIGH  water=D2-active-LOW  blink=D4");
  Serial.println("[RECV]   water timeout=30s  current thresh=0.10A on ESP");
}

// ═════════════════════════════════════════════════════════════════════════
//  Main loop
// ═════════════════════════════════════════════════════════════════════════

void loop() {
  // Drain pending commands (set in ESP-NOW recv callback)
  if (g_pendingOn)  { g_pendingOn  = false; activateMotor();   }
  if (g_pendingOff) { g_pendingOff = false; deactivateMotor(); }

  handleServo();
  handleWaterWatchdog();
  sendStatusPeriodic();
  handleBlink();
}
