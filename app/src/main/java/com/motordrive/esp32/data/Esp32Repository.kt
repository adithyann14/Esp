package com.motordrive.esp32.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * All HTTP calls to the ESP8266 sender (or server proxy).
 * Uses HttpURLConnection only — no extra dependencies.
 *
 * ── ESP8266 endpoints ────────────────────────────────────────────────────
 *
 *  GET  /api/status
 *       → { "motorOn": bool, "current": f, "isRunning": bool,
 *           "waterDetected": bool, "espNowConnected": bool }
 *
 *  POST /api/motor/on        → { "success": true }
 *  POST /api/motor/off       → { "success": true }
 *  GET  /api/logs            → { "logs": ["line1", "line2", ...] }
 *  GET  /api/alerts          → { "alerts": [] }
 *  POST /api/alerts/clear    → { "success": true }
 * ─────────────────────────────────────────────────────────────────────────
 */
class Esp32Repository(private val config: ConnectionConfig) {

    companion object {
        private const val CONNECT_TIMEOUT = 5_000
        private const val READ_TIMEOUT    = 5_000
    }

    // ── Motor status ──────────────────────────────────────────────────────
    suspend fun getStatus(): Result<MotorState> = io {
        parseStatus(httpGet("${config.baseUrl}/api/status"))
    }

    // ── Motor commands ────────────────────────────────────────────────────
    suspend fun motorOn():  Result<Unit> = io { httpPost("${config.baseUrl}/api/motor/on");  Unit }
    suspend fun motorOff(): Result<Unit> = io { httpPost("${config.baseUrl}/api/motor/off"); Unit }

    // ── Alerts ────────────────────────────────────────────────────────────
    suspend fun getAlerts(): Result<List<PendingAlert>> = io {
        parseAlerts(httpGet("${config.baseUrl}/api/alerts"))
    }

    suspend fun clearAlerts(): Result<Unit> = io {
        httpPost("${config.baseUrl}/api/alerts/clear")
        Unit
    }

    /**
     * Fetch the ESP sender's serial log ring-buffer (GET /api/logs).
     * Returns an empty list gracefully if the endpoint doesn't exist (older firmware).
     */
    suspend fun getLogs(): Result<List<String>> = io {
        try {
            val json = httpGet("${config.baseUrl}/api/logs")
            val arr = JSONObject(json).optJSONArray("logs") ?: return@io emptyList()
            (0 until arr.length())
                .map { arr.optString(it, "").trim() }
                .filter { it.isNotBlank() }
        } catch (_: IOException) {
            // Older firmware without /api/logs — return empty list, don't throw
            emptyList()
        }
    }

    // ── HTTP primitives ───────────────────────────────────────────────────
    private suspend fun <T> io(block: () -> T): Result<T> =
        withContext(Dispatchers.IO) { runCatching(block) }

    @Throws(IOException::class)
    private fun httpGet(urlStr: String): String {
        val conn = open(urlStr, "GET")
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw IOException("HTTP $code from $urlStr")
            return conn.inputStream.bufferedReader().readText()
        } finally { conn.disconnect() }
    }

    @Throws(IOException::class)
    private fun httpPost(urlStr: String): String {
        val conn = open(urlStr, "POST").apply {
            doOutput = true
            setRequestProperty("Content-Length", "0")
            outputStream.close()
        }
        try {
            val code = conn.responseCode
            if (code !in 200..299) throw IOException("HTTP $code from $urlStr")
            return runCatching { conn.inputStream.bufferedReader().readText() }.getOrDefault("")
        } finally { conn.disconnect() }
    }

    private fun open(urlStr: String, method: String): HttpURLConnection =
        (URL(urlStr).openConnection() as HttpURLConnection).apply {
            requestMethod  = method
            connectTimeout = CONNECT_TIMEOUT
            readTimeout    = READ_TIMEOUT
            setRequestProperty("Accept", "application/json")
        }

    // ── JSON parsing ──────────────────────────────────────────────────────
    private fun parseStatus(json: String): MotorState {
        val o = JSONObject(json)
        return MotorState(
            motorOn         = o.optBoolean("motorOn", false),
            voltageR        = o.floatOrNull("voltageR"),
            voltageY        = o.floatOrNull("voltageY"),
            voltageB        = o.floatOrNull("voltageB"),
            current         = o.floatOrNull("current"),
            waterDetected   = o.boolOrNull("waterDetected"),   // ← new key (was "waterFlow")
            espNowConnected = o.optBoolean("espNowConnected", false),
            isConnected     = true,
            lastUpdatedMs   = System.currentTimeMillis(),
            errorMessage    = null
        )
    }

    private fun parseAlerts(json: String): List<PendingAlert> {
        val arr = JSONObject(json).optJSONArray("alerts") ?: return emptyList()
        return (0 until arr.length()).map { i ->
            val o = arr.getJSONObject(i)
            PendingAlert(
                type      = o.optString("type",      "unknown"),
                timestamp = o.optLong("timestamp",   0L),
                message   = o.optString("message",   "Unknown event")
            )
        }
    }

    // ── Extension helpers ─────────────────────────────────────────────────
    private fun JSONObject.floatOrNull(key: String): Float? {
        if (!has(key) || isNull(key)) return null
        val d = optDouble(key, Double.NaN)
        return if (d.isNaN()) null else d.toFloat()
    }

    private fun JSONObject.boolOrNull(key: String): Boolean? =
        if (has(key) && !isNull(key)) optBoolean(key) else null
}
