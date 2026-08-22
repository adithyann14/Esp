package com.motordrive.esp32.ui

import android.content.res.ColorStateList
import android.os.Bundle
import android.view.View
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.navigation.fragment.findNavController
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.motordrive.esp32.FeatureConfig
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.data.PendingAlert
import com.motordrive.esp32.databinding.FragmentDashboardBinding
import com.motordrive.esp32.modules.CurrentModule
import com.motordrive.esp32.modules.VoltageModule
import com.motordrive.esp32.modules.WaterFlowModule
import com.motordrive.esp32.viewmodel.DashboardViewModel
import com.motordrive.esp32.viewmodel.SensorVisibility
import kotlinx.coroutines.launch

class DashboardFragment : Fragment(R.layout.fragment_dashboard) {

    private var _b: FragmentDashboardBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()

    private var voltageModule: VoltageModule?   = null
    private var currentModule: CurrentModule?   = null
    private var waterModule:   WaterFlowModule? = null

    private var alertsShown = false

    // ── Lifecycle ─────────────────────────────────────────────────

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentDashboardBinding.bind(view)

        applyWindowInsets()
        setupToolbar()
        inflateModules()
        setupButtons()
        observeVm()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _b = null
    }

    // ── Window insets ─────────────────────────────────────────────

    private fun applyWindowInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(b.appBarLayout) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.updatePadding(top = bars.top)
            insets
        }
    }

    // ── Toolbar ───────────────────────────────────────────────────

    private fun setupToolbar() {
        b.toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_settings -> {
                    findNavController().navigate(R.id.action_dashboard_to_settings)
                    true
                }
                R.id.action_refresh -> { vm.refresh(); true }
                else -> false
            }
        }
    }

    // ── Module inflation ──────────────────────────────────────────

    private fun inflateModules() {
        val inf = layoutInflater

        if (FeatureConfig.ENABLE_VOLTAGE_SENSORS) {
            val v = inf.inflate(R.layout.module_voltage, b.moduleVoltageContainer, false)
            b.moduleVoltageContainer.addView(v)
            voltageModule = VoltageModule(v)
        } else {
            b.moduleVoltageContainer.isVisible = false
        }

        if (FeatureConfig.ENABLE_CURRENT_SENSOR) {
            val v = inf.inflate(R.layout.module_current, b.moduleCurrentContainer, false)
            b.moduleCurrentContainer.addView(v)
            currentModule = CurrentModule(v)
        } else {
            b.moduleCurrentContainer.isVisible = false
        }

        if (FeatureConfig.ENABLE_WATER_FLOW) {
            val v = inf.inflate(R.layout.module_water_flow, b.moduleWaterContainer, false)
            b.moduleWaterContainer.addView(v)
            waterModule = WaterFlowModule(v)
        } else {
            b.moduleWaterContainer.isVisible = false
        }
    }

    // ── Buttons ───────────────────────────────────────────────────

    private fun setupButtons() {
        b.btnMotorOn.setOnClickListener  { vm.motorOn()  }
        b.btnMotorOff.setOnClickListener { vm.motorOff() }
    }

    // ── Observe ───────────────────────────────────────────────────

    private fun observeVm() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {

                launch {
                    vm.motorState.collect { state ->
                        updateStatusCard(state)
                        voltageModule?.update(state)
                        currentModule?.update(state)
                        waterModule?.update(state)
                    }
                }

                launch {
                    vm.isLoading.collect { loading ->
                        b.progressBar.isVisible = loading
                        if (loading) {
                            b.btnMotorOn.isEnabled  = false
                            b.btnMotorOff.isEnabled = false
                        }
                    }
                }

                launch {
                    vm.config.collect { cfg -> b.connectionUrlText.text = cfg.baseUrl }
                }

                launch {
                    vm.sensorVisibility.collect { v -> applySensorVisibility(v) }
                }

                launch {
                    vm.pendingAlerts.collect { alerts ->
                        if (alerts.isNotEmpty() && !alertsShown) {
                            alertsShown = true
                            showAlertDialog(alerts)
                        }
                    }
                }
            }
        }
    }

    // ── Sensor visibility ─────────────────────────────────────────

    private fun applySensorVisibility(v: SensorVisibility) {
        if (FeatureConfig.ENABLE_VOLTAGE_SENSORS) {
            b.moduleVoltageContainer.isVisible = v.showVoltage
        }
        if (FeatureConfig.ENABLE_CURRENT_SENSOR) {
            b.moduleCurrentContainer.isVisible = v.showCurrent
        }
        if (FeatureConfig.ENABLE_WATER_FLOW) {
            b.moduleWaterContainer.isVisible = v.showWater
        }
    }

    // ── Alert dialog ──────────────────────────────────────────────

    private fun showAlertDialog(alerts: List<PendingAlert>) {
        val icon = when (alerts.first().type) {
            "power_loss"  -> "⚡"
            "phase_fault" -> "⚠️"
            "overload"    -> "🔥"
            else          -> "ℹ️"
        }
        val body = alerts.joinToString("\n\n") { a ->
            val ts = if (a.timestamp > 0L)
                java.text.SimpleDateFormat("dd MMM HH:mm:ss", java.util.Locale.getDefault())
                    .format(java.util.Date(a.timestamp * 1000L))
            else "Unknown time"
            "• ${a.message}\n  $ts"
        }

        MaterialAlertDialogBuilder(requireContext())
            .setTitle("$icon  ESP Alert${if (alerts.size > 1) "s" else ""}")
            .setMessage(body)
            .setPositiveButton("Acknowledge & Clear") { _, _ ->
                alertsShown = false
                vm.clearAlerts()
            }
            .setNegativeButton("Dismiss") { _, _ -> }
            .setCancelable(false)
            .show()
    }

    // ── Status card ───────────────────────────────────────────────

    private fun updateStatusCard(state: MotorState) {
        if (state.isConnected) {
            b.chipConnection.text = "● Connected"
            b.chipConnection.chipBackgroundColor = ColorStateList.valueOf(
                requireContext().getColor(R.color.status_connected_bg)
            )
            b.chipConnection.setTextColor(requireContext().getColor(R.color.status_connected_text))
        } else {
            b.chipConnection.text = "○ ${state.errorMessage?.take(28) ?: "Disconnected"}"
            b.chipConnection.chipBackgroundColor = ColorStateList.valueOf(
                requireContext().getColor(R.color.status_error_bg)
            )
            b.chipConnection.setTextColor(requireContext().getColor(R.color.status_error_text))
        }

        if (state.lastUpdatedMs > 0L) {
            val secs = (System.currentTimeMillis() - state.lastUpdatedMs) / 1000
            b.lastUpdatedText.text = "Updated ${secs}s ago"
        } else {
            b.lastUpdatedText.text = ""
        }

        if (state.motorOn) {
            b.motorStateBanner.text = "● MOTOR ON"
            b.motorStateBanner.setBackgroundColor(requireContext().getColor(R.color.motor_on))
            b.motorStateBanner.setTextColor(requireContext().getColor(R.color.on_white))
            b.btnMotorOn.isEnabled  = false
            b.btnMotorOff.isEnabled = true
        } else {
            b.motorStateBanner.text = "○ MOTOR OFF"
            b.motorStateBanner.setBackgroundColor(requireContext().getColor(R.color.motor_off))
            b.motorStateBanner.setTextColor(requireContext().getColor(R.color.on_white))
            b.btnMotorOn.isEnabled  = true
            b.btnMotorOff.isEnabled = false
        }
    }
}
