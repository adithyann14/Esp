package com.motordrive.esp32.modules

import android.content.res.ColorStateList
import android.view.View
import com.google.android.material.color.MaterialColors
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleWaterFlowBinding

/**
 * MODULE C — Water Flow Sensor (Pipe End)
 * To DISABLE: set FeatureConfig.ENABLE_WATER_FLOW = false
 *
 * Same R.attr split as VibrationModule — see note there.
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
            true  -> androidx.appcompat.R.attr.colorPrimary
            false -> androidx.appcompat.R.attr.colorError
            null  -> com.google.android.material.R.attr.colorOutline
        }
        val color = MaterialColors.getColor(b.root, colorAttr, 0)
        b.waterIcon.imageTintList = ColorStateList.valueOf(color)
        b.waterStatusText.setTextColor(color)
    }
}
