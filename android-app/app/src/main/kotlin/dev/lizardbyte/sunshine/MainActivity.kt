package dev.lizardbyte.sunshine

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import rikka.shizuku.Shizuku

/**
 * Minimal control UI: request screen-capture consent and start/stop the Sunshine service.
 */
class MainActivity : Activity() {

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
            text = "Tap Start and grant screen capture, then open https://<device-ip>:47990 to pair Moonlight."
            textSize = 14f
            setPadding(0, 24, 0, 40)
        })
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
        setContentView(root)
    }

    override fun onDestroy() {
        Shizuku.removeRequestPermissionResultListener(shizukuPermissionListener)
        super.onDestroy()
    }

    /**
     * Request Shizuku permission (for non-root input), then RECORD_AUDIO, then screen-capture
     * consent. Both are optional — denial just means those features are unavailable.
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
        if (requestCode == REQ_AUDIO) {
            requestProjectionConsent()
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_PROJECTION && resultCode == RESULT_OK && data != null) {
            val intent = Intent(this, SunshineService::class.java).apply {
                putExtra(SunshineService.EXTRA_RESULT_CODE, resultCode)
                putExtra(SunshineService.EXTRA_RESULT_DATA, data)
            }
            startForegroundService(intent)
        }
    }

    companion object {
        private const val TAG = "MainActivity"
        private const val REQ_PROJECTION = 1001
        private const val REQ_AUDIO = 1002
        private const val REQ_SHIZUKU = 1003
    }
}
