package com.motordrive.esp32.modules

import android.view.View
import com.google.android.material.color.MaterialColors
import com.motordrive.esp32.data.MotorState
import com.motordrive.esp32.databinding.ModuleVibrationBinding

/**
 * MODULE B — Vibration Sensor
 *
 * The SW-420 sensor mounted on the motor body reports whether the motor
 * is mechanically active. This gives indirect confirmation that the motor
 * is actually running after a start command.
 *
 * To DISABLE: set FeatureConfig.ENABLE_VIBRATION_SENSOR = false
 */
class VibrationModule(view: View) {

    private val b = ModuleVibrationBinding.bind(view)

    fun update(state: MotorState) {
        val v = state.vibrationDetected
        b.vibrationStatusText.text = when (v) {
            true  -> "Motor Running"
            false -> "Motor Idle"
            null  -> "No Data"
        }
        b.vibrationSubtext.text = when (v) {
            true  -> "Vibration detected — motor is active"
            false -> "No vibration — motor appears stopped"
            null  -> "Sensor not responding"
        }
        val colorAttr = when (v) {
            true  -> com.google.android.material.R.attr.colorPrimary
            false -> com.google.android.material.R.attr.colorOutline
            null  -> com.google.android.material.R.attr.colorError
        }
        val color = MaterialColors.getColor(b.root, colorAttr)
        b.vibrationIcon.imageTintList = android.content.res.ColorStateList.valueOf(color)
        b.vibrationStatusText.setTextColor(color)
    }
}
