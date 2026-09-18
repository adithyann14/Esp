package com.motordrive.esp32.modules

import android.view.View
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleCurrentBinding

/**
 * MODULE B — Current Sensor
 *
 * Shows the ACS712 current reading (A).
 * Motor running status is already shown in the top banner — not repeated here.
 *
 * To DISABLE: set FeatureConfig.ENABLE_CURRENT_SENSOR = false
 */
class CurrentModule(view: View) {

    private val b = ModuleCurrentBinding.bind(view)

    fun update(state: MotorState) {
        b.valueCurrentA.text = state.current?.let { "%.2f A".format(it) } ?: "—"
    }
}
