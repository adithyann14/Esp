package com.motordrive.esp32

/**
 * ════════════════════════════════════════════════════════════════
 *  FEATURE CONFIGURATION  —  toggle modules on / off HERE
 * ════════════════════════════════════════════════════════════════
 *
 *  MODULE A  ·  ENABLE_POWER_SENSORS
 *    Shows 3 phase voltages (R, Y, B) + a single-phase current
 *    reading on the dashboard. Disable if these sensors are not
 *    physically fitted on your ESP32 receiver board.
 *
 *  MODULE B  ·  ENABLE_VIBRATION_SENSOR
 *    Shows a vibration sensor card. The SW-420 sensor is mounted
 *    on or near the motor; its state reflects motor run-status
 *    without explicit current measurement. Disable if not fitted.
 *    (MODULE A and MODULE B are independent — use one, both, or neither.)
 *
 *  MODULE C  ·  ENABLE_WATER_FLOW
 *    Shows a Water Flow card whose data comes from a sensor wired
 *    to the far end of the pipe. Indicates whether water is actually
 *    flowing out of the pump. Disable if sensor not connected.
 *
 *  MODULE D  ·  ENABLE_SERVER_MODE
 *    Adds a "Server URL" field in Settings so the app can route
 *    commands through an intermediate server (useful for global
 *    internet control). When false, only Direct Wi-Fi mode is available.
 * ════════════════════════════════════════════════════════════════
 */
object FeatureConfig {
    const val ENABLE_POWER_SENSORS    = true    // MODULE A
    const val ENABLE_VIBRATION_SENSOR = true    // MODULE B
    const val ENABLE_WATER_FLOW       = true    // MODULE C
    const val ENABLE_SERVER_MODE      = true    // MODULE D
}
