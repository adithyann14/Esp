package com.motordrive.esp32.modules

import android.content.res.ColorStateList
import android.view.View
import com.google.android.material.color.MaterialColors
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleWaterFlowBinding

/**
 * MODULE C — Pipe-end water sensor.
 *
 * Sensor is active-LOW (conducting = water detected = digital LOW on D2).
 * The receiver ESP sets waterDetected=true in its status packet when it reads LOW.
 * Cut-off logic (30 s no-water) runs on the receiver, not here.
 *
 * To DISABLE: set FeatureConfig.ENABLE_WATER_FLOW = false
 */
class WaterFlowModule(view: View) {

    private val b = ModuleWaterFlowBinding.bind(view)

    fun update(state: MotorState) {
        val detected = state.waterDetected   // true = water at pipe end

        b.waterStatusText.text = when (detected) {
            true  -> "Water Detected"
            false -> "No Water"
            null  -> "No Data"
        }
        b.waterSubtext.text = when (detected) {
            true  -> "Flow confirmed at pipe end"
            false -> "No flow at pipe end — motor auto-off in 30 s"
            null  -> "Sensor not connected or not responding"
        }

        val colorAttr = when (detected) {
            true  -> androidx.appcompat.R.attr.colorPrimary
            false -> androidx.appcompat.R.attr.colorError
            null  -> com.google.android.material.R.attr.colorOutline
        }
        val color = MaterialColors.getColor(b.root, colorAttr, 0)
        b.waterIcon.imageTintList = ColorStateList.valueOf(color)
        b.waterStatusText.setTextColor(color)
    }
}
