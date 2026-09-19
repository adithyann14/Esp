package com.motordrive.esp32.data

/**
 * Snapshot of everything the ESP8266 sender reports via GET /api/status.
 *
 * ESP JSON format:
 * {
 *   "motorOn":          true,
 *   "current":          4.2,       // ACS712 (A); threshold = 0.10 A on ESP, not app
 *   "isRunning":        true,      // derived on ESP: motorOn OR current > 0.10 A
 *   "waterDetected":    true,      // pipe-end sensor active-LOW; true = water flowing
 *   "espNowConnected":  true       // receiver heard within last 10 s
 * }
 *
 * Keys absent in the response stay null; UI shows "—".
 * Motor ON/OFF banner is on the dashboard; CurrentModule shows current value only.
 */
data class MotorState(
    val motorOn: Boolean = false,

    // MODULE A: 3-phase voltages (sensors not fitted — keys absent, show "—")
    val voltageR: Float? = null,
    val voltageY: Float? = null,
    val voltageB: Float? = null,

    // MODULE B: ACS712 current — threshold (0.10 A) lives on ESP, not in app
    val current: Float? = null,

    // MODULE C: Pipe-end water sensor (active-LOW; LOW = conducting = water)
    val waterDetected: Boolean? = null,

    // ESP-NOW link health: true if receiver status packet arrived < 10 s ago
    val espNowConnected: Boolean = false,

    // Connection metadata
    val isConnected:   Boolean = false,
    val lastUpdatedMs: Long    = 0L,
    val errorMessage:  String? = null
)
