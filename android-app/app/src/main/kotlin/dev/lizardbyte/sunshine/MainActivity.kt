package dev.lizardbyte.sunshine

import android.app.Activity
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/**
 * Minimal control UI: request screen-capture consent and start/stop the Sunshine service.
 */
class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

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

    private fun requestProjection() {
        val mpm = getSystemService(MediaProjectionManager::class.java)
        startActivityForResult(mpm.createScreenCaptureIntent(), REQ_PROJECTION)
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
        private const val REQ_PROJECTION = 1001
    }
}
