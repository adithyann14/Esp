/**
 * ════════════════════════════════════════════════════════════════════════
 *  RECEIVER  —  NodeMCU ESP8266  (board: NodeMCU 1.0 / ESP-12E)
 *
 *  Responsibilities:
 *    • Receives motor ON/OFF commands from Sender via ESP-NOW broadcast
 *    • Servo (D3/GPIO0): 90°=idle | 0°=push START | 180°=push STOP
 *      Returns to 90° after 500 ms (button-press simulation)
 *    • Relay (D1/GPIO5): HIGH=ON, LOW=OFF  (direct 5 V pump drive)
 *    • ACS712 current sensor (A0): reported back to Sender every 2 s
 *    • Motor state tracked via last servo action
 *
 *  Transport: ESP-NOW broadcast (FF:FF:FF:FF:FF:FF)
 *  Connects to Sender's Wi-Fi AP at boot for channel synchronisation.
 *
 *  DEAD CODE (wired, not active):
 *    • LoRa SX1278  (D0=RST, D4=DIO0, D5=SCK, D6=MISO, D7=MOSI, D8=NSS)
 *    • Water sensor via BC547  (D2)
 *
 *  Libraries required:
 *    — ESP8266WiFi   (bundled with ESP8266 Arduino core)
 *    — Servo         (bundled with ESP8266 Arduino core — ESP8266Servo)
 *    — espnow        (bundled with ESP8266 Arduino core SDK)
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <Servo.h>

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>
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
#define PIN_RELAY   D1   // GPIO5  — Relay IN  (HIGH = relay energised = pump ON)
// #define PIN_WATER D2   // GPIO4  — DEAD CODE: BC547 water sensor (10k pull-up to 3.3 V)
#define PIN_SERVO   D3   // GPIO0  — Servo signal  ⚠ boot pin — servo does not pull LOW
// D4–D8  → LoRa dead code (see above)
// A0     → ACS712 current sensor (active)

// ── Sender AP credentials (receiver joins for channel sync) ───────────────
static const char*    AP_SSID        = "MotorControl";
static const char*    AP_PASS        = "motor1234";
static const uint8_t  WIFI_CH        = 1;
static const uint16_t AP_TIMEOUT_MS  = 10000;

// ── ESP-NOW ───────────────────────────────────────────────────────────────
static uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct __attribute__((packed)) {
  uint8_t  type;   // 0x01
  uint8_t  cmd;    // 0x01 = ON   0x02 = OFF
  uint32_t seq;
} CmdPacket;

typedef struct __attribute__((packed)) {
  uint8_t  type;     // 0x02
  float    current;  // ACS712 reading (A)
  uint8_t  motorOn;  // confirmed state
  uint32_t seq;
} StatusPacket;

// ── ACS712 calibration  (5A module on 3.3 V supply) ──────────────────────
//
//  At 3.3 V supply:
//    Zero-current output  = VCC/2 = 1.65 V
//    Sensitivity scales with VCC: 185 mV/A × (3.3 / 5) ≈ 122 mV/A
//
//  Fine-tune ACS_ZERO_V if your module reads non-zero at no load.
// ─────────────────────────────────────────────────────────────────────────
static const float ACS_ZERO_V          = 1.65f;   // V at zero current
static const float ACS_SENS_V_PER_A    = 0.122f;  // V/A  (≈ 122 mV/A)
static const float CURRENT_RUN_THRESH  = 0.10f;   // A — above this = motor running

// ── Servo angles ──────────────────────────────────────────────────────────
#define SERVO_NEUTRAL  90    // resting — no action
#define SERVO_START     0    // push START button → motor ON
#define SERVO_STOP    180    // push STOP  button → motor OFF
#define SERVO_HOLD_MS 500UL  // time to hold the button
#define SERVO_BACK_MS 300UL  // time for arm to return to neutral

// ── State ─────────────────────────────────────────────────────────────────
static Servo motorServo;

enum class ServoState : uint8_t { IDLE, PRESSING, RETURNING };
static ServoState    servoState   = ServoState::IDLE;
static unsigned long servoTimer   = 0;

static volatile bool     g_pendingOn  = false;
static volatile bool     g_pendingOff = false;
static volatile uint32_t g_lastSeq    = 0;
static bool              g_motorOn    = false;

static uint32_t      g_statusSeq    = 0;
static unsigned long g_lastStatusMs = 0;
#define STATUS_INTERVAL_MS 2000UL

// ═════════════════════════════════════════════════════════════════════════
//  ACS712 current reading  (averaged over 100 samples)
// ═════════════════════════════════════════════════════════════════════════

static float readCurrentA() {
  long sum = 0;
  const int N = 100;
  for (int i = 0; i < N; i++) {
    sum += analogRead(A0);
    delayMicroseconds(100);
  }
  float voltage = (sum / (float)N / 1023.0f) * 3.3f;      // V
  float current = (voltage - ACS_ZERO_V) / ACS_SENS_V_PER_A; // A
  return current;
}

// ═════════════════════════════════════════════════════════════════════════
//  Servo / relay helpers
// ═════════════════════════════════════════════════════════════════════════

static void activateMotor() {
  if (servoState != ServoState::IDLE) return;  // servo busy — ignore
  Serial.println("[RECV] → Motor ON  (servo 0°, relay HIGH)");
  motorServo.write(SERVO_START);
  digitalWrite(PIN_RELAY, HIGH);
  g_motorOn   = true;
  servoState  = ServoState::PRESSING;
  servoTimer  = millis();
}

static void deactivateMotor() {
  if (servoState != ServoState::IDLE) return;  // servo busy — ignore
  Serial.println("[RECV] → Motor OFF  (servo 180°, relay LOW)");
  motorServo.write(SERVO_STOP);
  digitalWrite(PIN_RELAY, LOW);
  g_motorOn   = false;
  servoState  = ServoState::PRESSING;
  servoTimer  = millis();
}

/* Non-blocking servo state machine — called every loop iteration. */
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
        Serial.println("[RECV] servo → neutral");
      }
      break;
    case ServoState::IDLE:
    default:
      break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Periodic status broadcast back to Sender
// ═════════════════════════════════════════════════════════════════════════

static void sendStatusPeriodic() {
  unsigned long now = millis();
  if (now - g_lastStatusMs < STATUS_INTERVAL_MS) return;
  g_lastStatusMs = now;

  float c = readCurrentA();

  StatusPacket pkt;
  pkt.type    = 0x02;
  pkt.current = c;
  pkt.motorOn = (g_motorOn || c > CURRENT_RUN_THRESH) ? 1 : 0;
  pkt.seq     = ++g_statusSeq;

  esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));

  Serial.printf("[RECV] status → current=%.2f A  motorOn=%d\n",
                c, (int)pkt.motorOn);
}

// ═════════════════════════════════════════════════════════════════════════
//  DEAD CODE — water sensor
// ═════════════════════════════════════════════════════════════════════════

// static bool readWaterSensor() {
//   // BC547 on D2, 10k pull-up to 3.3 V.
//   // LOW = water detected (transistor conducting).
//   // TODO: tune threshold / debounce before enabling.
//   return digitalRead(D2) == LOW;
// }

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ═════════════════════════════════════════════════════════════════════════

void espnowOnSend(uint8_t* /*mac*/, uint8_t /*status*/) { }

void espnowOnRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
  if (len < sizeof(CmdPacket)) return;

  CmdPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.type != 0x01) return;
  if (pkt.seq == g_lastSeq) return;   // deduplicate retries
  g_lastSeq = pkt.seq;

  if      (pkt.cmd == 0x01) g_pendingOn  = true;
  else if (pkt.cmd == 0x02) g_pendingOff = true;
}

// ═════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[RECV] boot");

  // ── GPIO ──────────────────────────────────────────────────────────────
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);   // relay off at boot

  // ── Servo ─────────────────────────────────────────────────────────────
  motorServo.attach(PIN_SERVO);
  motorServo.write(SERVO_NEUTRAL);   // 90° safe resting position
  delay(300);

  // ── Wi-Fi — join Sender's AP for channel sync ─────────────────────────
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID, AP_PASS);

  WiFi.setOutputPower(20.5f);          // max TX power
  wifi_set_phy_mode(PHY_MODE_11B);     // 802.11b for maximum range

  Serial.print("[RECV] connecting to AP");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < AP_TIMEOUT_MS) {
    delay(200);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[RECV] joined AP  IP=%s  channel=%d\n",
                  WiFi.localIP().toString().c_str(),
                  (int)WiFi.channel());
  } else {
    // Sender not up yet — force channel and continue
    Serial.println("\n[RECV] AP not found — forcing channel " + String(WIFI_CH));
    WiFi.disconnect();
    wifi_set_channel(WIFI_CH);
  }

  // ── ESP-NOW ───────────────────────────────────────────────────────────
  if (esp_now_init() != 0) {
    Serial.println("[RECV] ESP-NOW init FAILED — rebooting");
    delay(2000);
    ESP.restart();
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(espnowOnSend);
  esp_now_register_recv_cb(espnowOnRecv);
  esp_now_add_peer(BCAST_MAC, ESP_NOW_ROLE_COMBO, WIFI_CH, nullptr, 0);

  Serial.println("[RECV] ESP-NOW ready (broadcast)");

  // ── LoRa DEAD CODE ────────────────────────────────────────────────────
  // LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  // if (!LoRa.begin(LORA_FREQ)) { Serial.println("LoRa init failed"); }
  // else { LoRa.setSpreadingFactor(12); LoRa.setTxPower(20); }
  // ─────────────────────────────────────────────────────────────────────

  Serial.println("[RECV] ready");
}

// ═════════════════════════════════════════════════════════════════════════
//  Main loop
// ═════════════════════════════════════════════════════════════════════════

void loop() {
  // Execute commands queued by ESP-NOW callback
  if (g_pendingOn) {
    g_pendingOn = false;
    activateMotor();
  }
  if (g_pendingOff) {
    g_pendingOff = false;
    deactivateMotor();
  }

  handleServo();
  sendStatusPeriodic();
}
