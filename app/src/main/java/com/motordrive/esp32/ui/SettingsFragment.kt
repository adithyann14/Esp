package com.motordrive.esp32.ui

import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.fragment.findNavController
import com.motordrive.esp32.FeatureConfig
import com.motordrive.esp32.R
import com.motordrive.esp32.data.ConnectionConfig
import com.motordrive.esp32.databinding.FragmentSettingsBinding
import com.motordrive.esp32.viewmodel.DashboardViewModel

class SettingsFragment : Fragment(R.layout.fragment_settings) {

    private var _b: FragmentSettingsBinding? = null
    private val b get() = _b!!

    private val vm: DashboardViewModel by activityViewModels()

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentSettingsBinding.bind(view)

        setupToolbar()
        setupServerModeToggle()
        populateFields()
        setupSaveButton()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _b = null
    }

    private fun setupToolbar() {
        b.toolbar.setNavigationOnClickListener { findNavController().navigateUp() }
    }

    private fun setupServerModeToggle() {
        // MODULE D: hide server section entirely if disabled at compile-time
        if (!FeatureConfig.ENABLE_SERVER_MODE) {
            b.serverModeSection.isVisible = false
            b.serverUrlLayout.isVisible   = false
            return
        }
        b.switchServerMode.setOnCheckedChangeListener { _, checked ->
            b.serverUrlLayout.isVisible = checked
        }
    }

    private fun populateFields() {
        val cfg = vm.config.value
        b.editIp.setText(cfg.directIp)
        b.editPort.setText(cfg.port.toString())
        b.editPollInterval.setText(cfg.pollIntervalSeconds.toString())

        if (FeatureConfig.ENABLE_SERVER_MODE) {
            b.switchServerMode.isChecked = cfg.useServerMode
            b.serverUrlLayout.isVisible  = cfg.useServerMode
            b.editServerUrl.setText(cfg.serverUrl)
        }
    }

    private fun setupSaveButton() {
        b.btnSave.setOnClickListener {
            val ip   = b.editIp.text?.toString()?.trim() ?: ""
            val port = b.editPort.text?.toString()?.toIntOrNull() ?: 80
            val poll = b.editPollInterval.text?.toString()?.toIntOrNull() ?: 3

            if (ip.isBlank()) {
                b.ipLayout.error = "Enter a valid IP or hostname"
                return@setOnClickListener
            }
            b.ipLayout.error = null

            val useServer = FeatureConfig.ENABLE_SERVER_MODE && b.switchServerMode.isChecked
            val serverUrl = b.editServerUrl.text?.toString()?.trim() ?: ""

            if (useServer && serverUrl.isBlank()) {
                b.serverUrlLayout.error = "Enter the server URL"
                return@setOnClickListener
            }
            b.serverUrlLayout.error = null

            val cfg = ConnectionConfig(
                directIp            = ip,
                port                = port.coerceIn(1, 65535),
                useServerMode       = useServer,
                serverUrl           = serverUrl,
                pollIntervalSeconds = poll.coerceIn(1, 60)
            )
            vm.updateConfig(cfg)
            Toast.makeText(requireContext(), "Settings saved", Toast.LENGTH_SHORT).show()
            findNavController().navigateUp()
        }
    }
}
