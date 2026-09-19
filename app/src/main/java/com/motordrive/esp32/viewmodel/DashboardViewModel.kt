package com.motordrive.esp32.viewmodel

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.motordrive.esp32.AppLogger
import com.motordrive.esp32.data.ConnectionConfig
import com.motordrive.esp32.data.Esp32Repository
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.data.PendingAlert
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Which sensor cards the user wants visible on the dashboard.
 * Saved to SharedPreferences immediately when a toggle changes.
 */
data class SensorVisibility(
    val showVoltage: Boolean = true,   // MODULE A
    val showCurrent: Boolean = true,   // MODULE B
    val showWater:   Boolean = true    // MODULE C
)

class DashboardViewModel(app: Application) : AndroidViewModel(app) {

    private val prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    // ── Public state ──────────────────────────────────────────────────────
    private val _motorState = MutableStateFlow(MotorState())
    val motorState: StateFlow<MotorState> = _motorState.asStateFlow()

    private val _config = MutableStateFlow(loadConfig())
    val config: StateFlow<ConnectionConfig> = _config.asStateFlow()

    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()

    private val _sensorVisibility = MutableStateFlow(loadSensorVisibility())
    val sensorVisibility: StateFlow<SensorVisibility> = _sensorVisibility.asStateFlow()

    private val _pendingAlerts = MutableStateFlow<List<PendingAlert>>(emptyList())
    val pendingAlerts: StateFlow<List<PendingAlert>> = _pendingAlerts.asStateFlow()

    /** Lines fetched from the ESP sender's /api/logs endpoint. */
    private val _espLogs = MutableStateFlow<List<String>>(emptyList())
    val espLogs: StateFlow<List<String>> = _espLogs.asStateFlow()

    // ── Polling ───────────────────────────────────────────────────────────
    private var pollJob: Job? = null

    init {
        AppLogger.log("VM", "ViewModel started")
        startPolling()
    }

    fun startPolling() {
        pollJob?.cancel()
        pollJob = viewModelScope.launch {
            while (true) {
                poll()
                delay(_config.value.pollIntervalSeconds * 1_000L)
            }
        }
    }

    fun stopPolling() { pollJob?.cancel() }

    private suspend fun poll() {
        repo().getStatus()
            .onSuccess { new ->
                val prev = _motorState.value
                _motorState.value = new

                // Log meaningful state transitions for the in-app logcat
                if (!prev.isConnected && new.isConnected)
                    AppLogger.log("CONN", "Connected to ESP8266 at ${_config.value.baseUrl}")
                if (prev.isConnected && !new.isConnected)
                    AppLogger.log("CONN", "Lost connection to ESP8266")
                if (prev.motorOn != new.motorOn)
                    AppLogger.log("MOTOR", "State changed → ${if (new.motorOn) "ON" else "OFF"}")
                if (prev.waterDetected != new.waterDetected && new.waterDetected != null)
                    AppLogger.log("WATER", "Detection → ${if (new.waterDetected) "WATER" else "DRY"}")
                if (prev.espNowConnected != new.espNowConnected)
                    AppLogger.log("ESP-NOW", "RF link ${if (new.espNowConnected) "UP ✓" else "DOWN ✗"}")
            }
            .onFailure { err ->
                val msg = err.message ?: "Connection failed"
                if (_motorState.value.isConnected)
                    AppLogger.log("CONN", "Poll error: $msg")
                _motorState.value = _motorState.value.copy(
                    isConnected  = false,
                    errorMessage = msg
                )
            }

        repo().getAlerts()
            .onSuccess { alerts ->
                if (alerts.isNotEmpty()) _pendingAlerts.value = alerts
            }
    }

    /**
     * Fetch the ESP serial log ring-buffer and update [espLogs].
     * Call this from SettingsFragment when the user taps "Refresh" in the
     * Serial Monitor panel. Non-blocking; runs on IO dispatcher.
     */
    suspend fun fetchLogs() {
        AppLogger.log("LOGS", "Fetching /api/logs …")
        repo().getLogs()
            .onSuccess { lines ->
                _espLogs.value = lines
                AppLogger.log("LOGS", "ESP log: ${lines.size} line(s)")
            }
            .onFailure { err ->
                AppLogger.log("LOGS", "Error: ${err.message}")
            }
    }

    // ── Commands ──────────────────────────────────────────────────────────
    fun motorOn()  = sendCmd(true)
    fun motorOff() = sendCmd(false)

    private fun sendCmd(on: Boolean) = viewModelScope.launch {
        if (_isLoading.value) return@launch
        _isLoading.value = true
        AppLogger.log("MOTOR", "Sending: ${if (on) "ON" else "OFF"}")
        val result = if (on) repo().motorOn() else repo().motorOff()
        if (result.isSuccess) {
            delay(400)
            poll()
        } else {
            val msg = result.exceptionOrNull()?.message ?: "Command failed"
            AppLogger.log("MOTOR", "Command failed: $msg")
            _motorState.value = _motorState.value.copy(errorMessage = msg)
        }
        _isLoading.value = false
    }

    fun refresh() = viewModelScope.launch { poll() }

    fun clearAlerts() = viewModelScope.launch {
        repo().clearAlerts()
        _pendingAlerts.value = emptyList()
    }

    // ── Settings ──────────────────────────────────────────────────────────
    fun updateConfig(cfg: ConnectionConfig) {
        AppLogger.log("CONN", "Config → ${cfg.baseUrl}")
        _config.value = cfg
        saveConfig(cfg)
        startPolling()
    }

    fun updateSensorVisibility(v: SensorVisibility) {
        _sensorVisibility.value = v
        saveSensorVisibility(v)
    }

    // ── Helpers ────────────────────────────────────────────────────────────
    private fun repo() = Esp32Repository(_config.value)

    // ── Persistence ────────────────────────────────────────────────────────
    private fun loadConfig() = ConnectionConfig(
        directIp            = prefs.getString(K_IP, "192.168.4.1") ?: "192.168.4.1",
        port                = prefs.getInt(K_PORT, 80),
        useServerMode       = prefs.getBoolean(K_SRV_MODE, false),
        serverUrl           = prefs.getString(K_SRV_URL, "") ?: "",
        pollIntervalSeconds = prefs.getInt(K_POLL, 3)
    )

    private fun saveConfig(c: ConnectionConfig) = prefs.edit()
        .putString(K_IP,        c.directIp)
        .putInt(K_PORT,         c.port)
        .putBoolean(K_SRV_MODE, c.useServerMode)
        .putString(K_SRV_URL,   c.serverUrl)
        .putInt(K_POLL,         c.pollIntervalSeconds)
        .apply()

    private fun loadSensorVisibility() = SensorVisibility(
        showVoltage = prefs.getBoolean(K_SHOW_VOLTAGE, true),
        showCurrent = prefs.getBoolean(K_SHOW_CURRENT, true),
        showWater   = prefs.getBoolean(K_SHOW_WATER,   true)
    )

    private fun saveSensorVisibility(v: SensorVisibility) = prefs.edit()
        .putBoolean(K_SHOW_VOLTAGE, v.showVoltage)
        .putBoolean(K_SHOW_CURRENT, v.showCurrent)
        .putBoolean(K_SHOW_WATER,   v.showWater)
        .apply()

    companion object {
        private const val PREFS           = "mdc_prefs"
        private const val K_IP            = "ip"
        private const val K_PORT          = "port"
        private const val K_SRV_MODE      = "server_mode"
        private const val K_SRV_URL       = "server_url"
        private const val K_POLL          = "poll_sec"
        private const val K_SHOW_VOLTAGE  = "show_voltage"
        private const val K_SHOW_CURRENT  = "show_current"
        private const val K_SHOW_WATER    = "show_water"
    }
}
