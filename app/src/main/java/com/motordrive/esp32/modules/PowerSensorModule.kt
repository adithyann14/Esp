package com.motordrive.esp32.modules

import android.view.View
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModulePowerSensorBinding

/**
 * MODULE A — 3-Phase Voltage + Current
 *
 * Inflate module_power_sensor.xml into a container, pass the root View here.
 * Call update(state) from the Fragment whenever MotorState changes.
 *
 * To DISABLE this module: set FeatureConfig.ENABLE_POWER_SENSORS = false
 */
class PowerSensorModule(view: View) {

    private val b = ModulePowerSensorBinding.bind(view)

    fun update(state: MotorState) {
        b.valueVoltageR.text = state.voltageR?.let { "%.1f V".format(it) } ?: "—"
        b.valueVoltageY.text = state.voltageY?.let { "%.1f V".format(it) } ?: "—"
        b.valueVoltageB.text = state.voltageB?.let { "%.1f V".format(it) } ?: "—"
        b.valueCurrent.text  = state.current?.let  { "%.2f A".format(it) } ?: "—"

        // Warn if any phase voltage looks wrong ( < 180 V or > 260 V )
        listOf(
            state.voltageR to b.valueVoltageR,
            state.voltageY to b.valueVoltageY,
            state.voltageB to b.valueVoltageB,
        ).forEach { (v, tv) ->
            if (v != null) {
                val ok = v in 180f..260f
                tv.setTextColor(
                    if (ok) tv.context.getColor(com.motordrive.esp32.R.color.value_normal)
                    else    tv.context.getColor(com.motordrive.esp32.R.color.value_warn)
                )
            }
        }
    }
}
