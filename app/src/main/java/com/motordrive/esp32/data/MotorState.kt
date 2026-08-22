package com.motordrive.esp32.data

/**
 * Snapshot of everything the ESP8266 reports back in one /api/status call.
 *
 * ESP8266 Arduino JSON format expected:
 * {
 *   "motorOn":   true,
 *   "voltageR":  231.5,   // MODULE A — phase R (V)
 *   "voltageY":  229.8,   // MODULE A — phase Y (V)
 *   "voltageB":  232.1,   // MODULE A — phase B (V)
 *   "current":   4.2,     // MODULE B — ACS712 current (A); >0.1 A = motor running
 *   "waterFlow": true     // MODULE C — water flow sensor state
 * }
 *
 * Fields not present in the JSON stay null; the UI shows "—".
 * Motor running state is derived from current > 0.1 A inside CurrentModule.
 */
data class MotorState(
    val motorOn: Boolean = false,

    // ── MODULE A: 3-phase voltages ────────────────────────────
    val voltageR: Float? = null,
    val voltageY: Float? = null,
    val voltageB: Float? = null,

    // ── MODULE B: Current sensor ──────────────────────────────
    val current: Float? = null,

    // ── MODULE C: Water flow sensor ───────────────────────────
    val waterFlowing: Boolean? = null,

    // ── Connection metadata ───────────────────────────────────
    val isConnected:   Boolean = false,
    val lastUpdatedMs: Long    = 0L,
    val errorMessage:  String? = null
)
