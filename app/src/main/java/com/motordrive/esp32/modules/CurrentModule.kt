package com.motordrive.esp32.modules

import android.view.View
import com.google.android.material.color.MaterialColors
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleCurrentBinding

/**
 * MODULE B — Current Sensor + Motor Running Detection
 *
 * Shows the ACS712 current reading (A).
 * Derives motor running state from: current > RUNNING_THRESHOLD_A
 * No vibration sensor — current is the sole indicator of motor activity.
 *
 * To DISABLE: set FeatureConfig.ENABLE_CURRENT_SENSOR = false
 */
class CurrentModule(view: View) {

    private val b = ModuleCurrentBinding.bind(view)

    companion object {
        /** Any reading above this is treated as motor actively running. */
        const val RUNNING_THRESHOLD_A = 0.1f
    }

    fun update(state: MotorState) {
        val c = state.current

        // ── Current value ─────────────────────────────────────
        b.valueCurrentA.text = c?.let { "%.2f A".format(it) } ?: "—"

        // ── Running status derived from current > threshold ───
        when (c?.let { it > RUNNING_THRESHOLD_A }) {
            true -> {
                b.valueRunningStatus.text = "● RUNNING"
                b.valueRunningStatus.setTextColor(
                    b.root.context.getColor(R.color.motor_on)
                )
            }
            false -> {
                b.valueRunningStatus.text = "○ STOPPED"
                b.valueRunningStatus.setTextColor(
                    MaterialColors.getColor(
                        b.root,
                        com.google.android.material.R.attr.colorOutline,
                        0
                    )
                )
            }
            null -> {
                b.valueRunningStatus.text = "—"
                b.valueRunningStatus.setTextColor(
                    MaterialColors.getColor(
                        b.root,
                        com.google.android.material.R.attr.colorOutline,
                        0
                    )
                )
            }
        }
    }
}
