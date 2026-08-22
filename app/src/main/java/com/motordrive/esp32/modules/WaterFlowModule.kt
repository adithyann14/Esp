package com.motordrive.esp32.modules

import android.view.View
import com.google.android.material.color.MaterialColors
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleWaterFlowBinding

/**
 * MODULE C — Water Flow Sensor (Pipe End)
 *
 * A flow/contact sensor is connected via a long wire from the ESP32 receiver
 * and placed at the far end of the discharge pipe. This confirms that water
 * is actually coming out — the motor may be running but the pipe could be
 * blocked, dry, or the pump could be airbound.
 *
 * To DISABLE: set FeatureConfig.ENABLE_WATER_FLOW = false
 */
class WaterFlowModule(view: View) {

    private val b = ModuleWaterFlowBinding.bind(view)

    fun update(state: MotorState) {
        val flowing = state.waterFlowing
        b.waterStatusText.text = when (flowing) {
            true  -> "Water Flowing"
            false -> "No Water Flow"
            null  -> "No Data"
        }
        b.waterSubtext.text = when (flowing) {
            true  -> "Discharge detected at pipe end"
            false -> "No discharge — check pump / pipe"
            null  -> "Sensor not connected or not responding"
        }
        val colorAttr = when (flowing) {
            true  -> com.google.android.material.R.attr.colorPrimary
            false -> com.google.android.material.R.attr.colorError
            null  -> com.google.android.material.R.attr.colorOutline
        }
        val color = MaterialColors.getColor(b.root, colorAttr)
        b.waterIcon.imageTintList = android.content.res.ColorStateList.valueOf(color)
        b.waterStatusText.setTextColor(color)
    }
}
