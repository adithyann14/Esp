package com.motordrive.esp32.ui

import android.content.res.ColorStateList
import android.os.Bundle
import android.view.View
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.navigation.fragment.findNavController
import com.motordrive.esp32.FeatureConfig
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.FragmentDashboardBinding
import com.motordrive.esp32.modules.PowerSensorModule
import com.motordrive.esp32.modules.VibrationModule
import com.motordrive.esp32.modules.WaterFlowModule
import com.motordrive.esp32.viewmodel.DashboardViewModel
import kotlinx.coroutines.launch

class DashboardFragment : Fragment(R.layout.fragment_dashboard) {

    private var _b: FragmentDashboardBinding? = null
    private val b get() = _b!!

    // FIX BUG 2: was viewModels() → Fragment-scoped instance that SettingsFragment
    // could never reach. activityViewModels() gives the same instance to both
    // fragments, so settings saved in SettingsFragment actually take effect here.
    private val vm: DashboardViewModel by activityViewModels()

    private var powerModule:     PowerSensorModule? = null
    private var vibrationModule: VibrationModule?   = null
    private var waterModule:     WaterFlowModule?   = null

    // ── Lifecycle ─────────────────────────────────────────────────

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentDashboardBinding.bind(view)

        setupToolbar()
        inflateModules()
        setupButtons()
        observeVm()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _b = null
    }

    // ── Toolbar ───────────────────────────────────────────────────

    private fun setupToolbar() {
        b.toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_settings -> {
                    findNavController().navigate(R.id.action_dashboard_to_settings)
                    true
                }
                R.id.action_refresh -> {
                    vm.refresh()
                    true
                }
                else -> false
            }
        }
    }

    // ── Module inflation ──────────────────────────────────────────

    private fun inflateModules() {
        val inf = layoutInflater

        if (FeatureConfig.ENABLE_POWER_SENSORS) {
            val v = inf.inflate(R.layout.module_power_sensor, b.modulePowerContainer, false)
            b.modulePowerContainer.addView(v)
            powerModule = PowerSensorModule(v)
        } else {
            b.modulePowerContainer.isVisible = false
        }

        if (FeatureConfig.ENABLE_VIBRATION_SENSOR) {
            val v = inf.inflate(R.layout.module_vibration, b.moduleVibrationContainer, false)
            b.moduleVibrationContainer.addView(v)
            vibrationModule = VibrationModule(v)
        } else {
            b.moduleVibrationContainer.isVisible = false
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
                        powerModule?.update(state)
                        vibrationModule?.update(state)
                        waterModule?.update(state)
                    }
                }

                launch {
                    vm.isLoading.collect { loading ->
                        b.progressBar.isVisible = loading

                        // FIX BUG 3: was setting isEnabled = !loading for BOTH buttons,
                        // which re-enabled the button that should stay disabled based on
                        // motor ON/OFF state (causing a race with updateStatusCard).
                        // Now: only DISABLE both during a command. Re-enabling is handled
                        // exclusively by updateStatusCard() based on actual motor state.
                        if (loading) {
                            b.btnMotorOn.isEnabled  = false
                            b.btnMotorOff.isEnabled = false
                        }
                        // When loading=false, updateStatusCard() (triggered by the
                        // motorState poll that follows the command) sets the correct
                        // enabled state. No conflict, no flicker.
                    }
                }

                launch {
                    vm.config.collect { cfg ->
                        b.connectionUrlText.text = cfg.baseUrl
                    }
                }
            }
        }
    }

    // ── Status card update ────────────────────────────────────────

    private fun updateStatusCard(state: MotorState) {
        // Connection chip
        if (state.isConnected) {
            b.chipConnection.text = "● Connected"
            b.chipConnection.chipBackgroundColor = ColorStateList.valueOf(
                requireContext().getColor(R.color.status_connected_bg)
            )
            b.chipConnection.setTextColor(requireContext().getColor(R.color.status_connected_text))
        } else {
            b.chipConnection.text = "○ ${state.errorMessage?.take(25) ?: "Disconnected"}"
            b.chipConnection.chipBackgroundColor = ColorStateList.valueOf(
                requireContext().getColor(R.color.status_error_bg)
            )
            b.chipConnection.setTextColor(requireContext().getColor(R.color.status_error_text))
        }

        // Last updated
        if (state.lastUpdatedMs > 0L) {
            val secs = (System.currentTimeMillis() - state.lastUpdatedMs) / 1000
            b.lastUpdatedText.text = "Updated ${secs}s ago"
        } else {
            b.lastUpdatedText.text = ""
        }

        // Motor state banner + button enabled state
        // This is the SINGLE source of truth for button isEnabled — isLoading
        // collector only ever disables; this method does all the re-enabling.
        if (state.motorOn) {
            b.motorStateBanner.text = "● MOTOR ON"
            b.motorStateBanner.setBackgroundColor(requireContext().getColor(R.color.motor_on))
            b.motorStateBanner.setTextColor(requireContext().getColor(R.color.on_white))
            b.btnMotorOn.isEnabled  = false   // already on — can't turn on again
            b.btnMotorOff.isEnabled = true
        } else {
            b.motorStateBanner.text = "○ MOTOR OFF"
            b.motorStateBanner.setBackgroundColor(requireContext().getColor(R.color.motor_off))
            b.motorStateBanner.setTextColor(requireContext().getColor(R.color.on_white))
            b.btnMotorOn.isEnabled  = true
            b.btnMotorOff.isEnabled = false   // already off — can't turn off again
        }
    }
}
