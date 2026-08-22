package com.motordrive.esp32.viewmodel

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.motordrive.esp32.data.ConnectionConfig
import com.motordrive.esp32.data.Esp32Repository
import com.motordrive.esp32.data.MotorState
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

class DashboardViewModel(app: Application) : AndroidViewModel(app) {

    private val prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    // ── Exposed state ─────────────────────────────────────────────
    private val _motorState = MutableStateFlow(MotorState())
    val motorState: StateFlow<MotorState> = _motorState.asStateFlow()

    private val _config = MutableStateFlow(loadConfig())
    val config: StateFlow<ConnectionConfig> = _config.asStateFlow()

    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()

    // ── Polling ───────────────────────────────────────────────────
    private var pollJob: Job? = null

    init { startPolling() }

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
        // Result.onSuccess/onFailure take NON-SUSPEND lambdas — safe to use here
        // because neither callback calls a suspend function.
        repo().getStatus()
            .onSuccess { _motorState.value = it }
            .onFailure { err ->
                _motorState.value = _motorState.value.copy(
                    isConnected  = false,
                    errorMessage = err.message ?: "Connection failed"
                )
            }
    }

    // ── Motor commands ────────────────────────────────────────────

    fun motorOn()  = sendCmd(true)
    fun motorOff() = sendCmd(false)

    private fun sendCmd(on: Boolean) = viewModelScope.launch {
        // FIX BUG 4: guard against double-tap firing two concurrent commands
        if (_isLoading.value) return@launch

        _isLoading.value = true

        val result = if (on) repo().motorOn() else repo().motorOff()

        // FIX BUG 1: Result.onSuccess { } takes a NON-SUSPEND lambda, so delay() and
        // poll() (both suspend functions) cannot be called inside it — compile error.
        // Replaced with a plain if/else inside the coroutine body where suspend calls are valid.
        if (result.isSuccess) {
            delay(400)   // brief wait so ESP32 state settles before re-polling
            poll()
        } else {
            _motorState.value = _motorState.value.copy(
                errorMessage = result.exceptionOrNull()?.message ?: "Command failed"
            )
        }

        _isLoading.value = false
    }

    fun refresh() = viewModelScope.launch { poll() }

    // ── Settings ──────────────────────────────────────────────────

    fun updateConfig(cfg: ConnectionConfig) {
        _config.value = cfg
        saveConfig(cfg)
        startPolling()
    }

    // ── Helpers ───────────────────────────────────────────────────

    private fun repo() = Esp32Repository(_config.value)

    private fun loadConfig() = ConnectionConfig(
        directIp            = prefs.getString(K_IP,  "192.168.4.1") ?: "192.168.4.1",
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

    companion object {
        private const val PREFS      = "mdc_prefs"
        private const val K_IP       = "ip"
        private const val K_PORT     = "port"
        private const val K_SRV_MODE = "server_mode"
        private const val K_SRV_URL  = "server_url"
        private const val K_POLL     = "poll_sec"
    }
}
