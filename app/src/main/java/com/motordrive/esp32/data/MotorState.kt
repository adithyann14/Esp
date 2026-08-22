package com.motordrive.esp32.data

/**
 * Snapshot of everything the ESP32 reports back in one /api/status call.
 *
 * ESP32 Arduino JSON format expected:
 * {
 *   "motorOn":   true,
 *   "voltageR":  231.5,   // MODULE A — phase R (V)
 *   "voltageY":  229.8,   // MODULE A — phase Y (V)
 *   "voltageB":  232.1,   // MODULE A — phase B (V)
 *   "current":   4.2,     // MODULE A — output current (A)
 *   "vibration": true,    // MODULE B — vibration sensor state
 *   "waterFlow": true     // MODULE C — water flow sensor state
 * }
 *
 * Fields not present in the JSON stay null; the UI shows "--".
 */
data class MotorState(
    val motorOn: Boolean = false,

    // ── MODULE A: 3-phase voltage + current ──────────────────
    val voltageR: Float? = null,
    val voltageY: Float? = null,
    val voltageB: Float? = null,
    val current:  Float? = null,

    // ── MODULE B: Vibration sensor ───────────────────────────
    val vibrationDetected: Boolean? = null,

    // ── MODULE C: Water flow sensor ──────────────────────────
    val waterFlowing: Boolean? = null,

    // ── Connection metadata ──────────────────────────────────
    val isConnected:  Boolean = false,
    val lastUpdatedMs: Long   = 0L,
    val errorMessage: String? = null
)
