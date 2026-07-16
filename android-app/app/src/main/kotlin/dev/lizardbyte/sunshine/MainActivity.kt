package dev.lizardbyte.sunshine

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView
import rikka.shizuku.Shizuku

/**
 * Minimal control UI: request screen-capture consent and start/stop the Sunshine service.
 */
class MainActivity : Activity() {

    private val prefs get() = getSharedPreferences("sunshine", MODE_PRIVATE)

    private val shizukuPermissionListener = Shizuku.OnRequestPermissionResultListener { requestCode, grantResult ->
        if (requestCode == REQ_SHIZUKU) {
            if (grantResult != PackageManager.PERMISSION_GRANTED) {
                Log.w(TAG, "Shizuku permission denied; input will be unavailable on non-rooted device")
            }
            requestAudioThenProjection()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Shizuku.addRequestPermissionResultListener(shizukuPermissionListener)

        // POST_NOTIFICATIONS requires a runtime request on Android 13+; without it the
        // foreground service notification is silently suppressed.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIF)
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(64, 128, 64, 64)
        }

        root.addView(TextView(this).apply {
            text = "Sunshine — Android host"
            textSize = 20f
        })
        root.addView(TextView(this).apply {
            text = "Tap Start, grant screen capture, then open the Settings page to pair Moonlight."
            textSize = 14f
            setPadding(0, 24, 0, 24)
        })

        // Privacy mode: uses the screencap tool instead of MediaProjection, so no screen-recording
        // indicator appears and the MediaProjection consent dialog is skipped. Trade-off: slower
        // (~3–10 fps), software encode only, and audio capture is unavailable.
        val privacyCheck = CheckBox(this).apply {
            text = "Privacy mode (no recording indicator; slower, no audio)"
            isChecked = prefs.getBoolean(SunshineService.PREF_SCREENCAP, false)
            setPadding(0, 0, 0, 32)
            setOnCheckedChangeListener { _, checked ->
                prefs.edit().putBoolean(SunshineService.PREF_SCREENCAP, checked).apply()
            }
        }
        root.addView(privacyCheck)

        root.addView(Button(this).apply {
            text = "Start Sunshine"
            setOnClickListener { requestProjection() }
        })
        root.addView(Button(this).apply {
            text = "Stop Sunshine"
            setOnClickListener {
                stopService(Intent(this@MainActivity, SunshineService::class.java))
            }
        })
        root.addView(Button(this).apply {
            text = "Open Settings (localhost:47990)"
            setPadding(0, 32, 0, 0)
            setOnClickListener { openWebUi() }
        })

        setContentView(root)
    }

    override fun onDestroy() {
        Shizuku.removeRequestPermissionResultListener(shizukuPermissionListener)
        super.onDestroy()
    }

    /**
     * Open the Sunshine web UI in the system default browser.
     */
    fun openWebUi() {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://localhost:47990")))
    }

    /**
     * Request Shizuku permission (for non-root input), then RECORD_AUDIO, then screen-capture
     * consent. In privacy (screencap) mode the MediaProjection flow is skipped entirely.
     */
    private fun requestProjection() {
        if (Shizuku.pingBinder() &&
            Shizuku.checkSelfPermission() != PackageManager.PERMISSION_GRANTED) {
            Shizuku.requestPermission(REQ_SHIZUKU)
            return
        }
        requestAudioThenProjection()
    }

    private fun requestAudioThenProjection() {
        // In screencap mode, AudioPlaybackCapture (which also requires MediaProjection) is
        // skipped, so audio will be silent. Start the service directly without a token.
        if (prefs.getBoolean(SunshineService.PREF_SCREENCAP, false)) {
            startSunshineService(0, null)
            return
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_AUDIO)
            return
        }
        requestProjectionConsent()
    }

    private fun requestProjectionConsent() {
        val mpm = getSystemService(MediaProjectionManager::class.java)
        startActivityForResult(mpm.createScreenCaptureIntent(), REQ_PROJECTION)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        when (requestCode) {
            REQ_AUDIO -> requestProjectionConsent()
            REQ_NOTIF -> {} // nothing to do; the notification will show on next service start
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_PROJECTION && resultCode == RESULT_OK && data != null) {
            startSunshineService(resultCode, data)
        }
    }

    private fun startSunshineService(resultCode: Int, resultData: Intent?) {
        val intent = Intent(this, SunshineService::class.java).apply {
            putExtra(SunshineService.EXTRA_RESULT_CODE, resultCode)
            if (resultData != null) putExtra(SunshineService.EXTRA_RESULT_DATA, resultData)
        }
        startForegroundService(intent)
    }

    companion object {
        private const val TAG = "MainActivity"
        private const val REQ_PROJECTION = 1001
        private const val REQ_AUDIO = 1002
        private const val REQ_SHIZUKU = 1003
        private const val REQ_NOTIF = 1004
    }
}
