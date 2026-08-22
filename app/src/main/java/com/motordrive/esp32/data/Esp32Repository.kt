package com.motordrive.esp32.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * All HTTP calls to the ESP32 (or server proxy).
 * Uses only HttpURLConnection — no extra dependencies, no conflicts.
 *
 * ── ESP32 Arduino endpoints to implement ────────────────────────
 *
 *  GET  /api/status          → JSON with motorOn, voltages, current,
 *                               vibration, waterFlow
 *  POST /api/motor/on        → {"success":true}
 *  POST /api/motor/off       → {"success":true}
 *
 * ────────────────────────────────────────────────────────────────
 */
class Esp32Repository(private val config: ConnectionConfig) {

    companion object {
        private const val CONNECT_TIMEOUT = 5_000   // ms
        private const val READ_TIMEOUT    = 5_000
    }

    // ── Public API ───────────────────────────────────────────────

    suspend fun getStatus(): Result<MotorState> = io {
        val json = httpGet("${config.baseUrl}/api/status")
        parseStatus(json)
    }

    suspend fun motorOn():  Result<Unit> = io { httpPost("${config.baseUrl}/api/motor/on");  Unit }
    suspend fun motorOff(): Result<Unit> = io { httpPost("${config.baseUrl}/api/motor/off"); Unit }

    // ── Helpers ──────────────────────────────────────────────────

    private suspend fun <T> io(block: () -> T): Result<T> =
        withContext(Dispatchers.IO) { runCatching(block) }

    @Throws(IOException::class)
    private fun httpGet(urlStr: String): String {
        val conn = openConnection(urlStr, "GET")
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw IOException("HTTP $code from $urlStr")
            return conn.inputStream.bufferedReader().readText()
        } finally {
            conn.disconnect()
        }
    }

    @Throws(IOException::class)
    private fun httpPost(urlStr: String): String {
        val conn = openConnection(urlStr, "POST").apply {
            doOutput = true
            setRequestProperty("Content-Length", "0")
            outputStream.close()
        }
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw IOException("HTTP $code from $urlStr")
            return runCatching { conn.inputStream.bufferedReader().readText() }.getOrDefault("")
        } finally {
            conn.disconnect()
        }
    }

    private fun openConnection(urlStr: String, method: String): HttpURLConnection =
        (URL(urlStr).openConnection() as HttpURLConnection).apply {
            requestMethod    = method
            connectTimeout   = CONNECT_TIMEOUT
            readTimeout      = READ_TIMEOUT
            setRequestProperty("Accept", "application/json")
        }

    // ── JSON parsing ─────────────────────────────────────────────

    private fun parseStatus(json: String): MotorState {
        val o = JSONObject(json)
        return MotorState(
            motorOn           = o.optBoolean("motorOn", false),
            // MODULE A
            voltageR          = o.floatOrNull("voltageR"),
            voltageY          = o.floatOrNull("voltageY"),
            voltageB          = o.floatOrNull("voltageB"),
            current           = o.floatOrNull("current"),
            // MODULE B
            vibrationDetected = o.boolOrNull("vibration"),
            // MODULE C
            waterFlowing      = o.boolOrNull("waterFlow"),
            isConnected       = true,
            lastUpdatedMs     = System.currentTimeMillis(),
            errorMessage      = null
        )
    }

    private fun JSONObject.floatOrNull(key: String): Float? {
        if (!has(key) || isNull(key)) return null
        val d = optDouble(key, Double.NaN)
        return if (d.isNaN()) null else d.toFloat()
    }

    private fun JSONObject.boolOrNull(key: String): Boolean? =
        if (has(key) && !isNull(key)) optBoolean(key) else null
}
