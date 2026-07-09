package dev.lizardbyte.sunshine

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.DisplayMetrics
import android.util.Log
import android.view.WindowManager
import java.io.File
import kotlin.concurrent.thread
import kotlin.math.max

/**
 * Foreground service that hosts the native Sunshine core and feeds it screen frames captured via
 * MediaProjection. It extracts the bundled web UI / apps.json into app storage (where the core
 * expects SUNSHINE_ASSETS_DIR / SUNSHINE_APPDATA), sets up a VirtualDisplay + ImageReader, pushes
 * each frame to the core over JNI, and runs the core on a dedicated thread.
 */
class SunshineService : Service() {
    @Volatile
    private var coreStarted = false
    private var nativeThread: Thread? = null

    private var projection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var imageReader: ImageReader? = null
    private var captureThread: HandlerThread? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // A mediaProjection foreground service must be started before the projection is used.
        startForegroundCompat()

        val resultCode = intent?.getIntExtra(EXTRA_RESULT_CODE, 0) ?: 0
        val resultData: Intent? = if (Build.VERSION.SDK_INT >= 33) {
            intent?.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
        } else {
            @Suppress("DEPRECATION") intent?.getParcelableExtra(EXTRA_RESULT_DATA)
        }
        if (projection == null && resultCode != 0 && resultData != null) {
            startCapture(resultCode, resultData)
        }

        if (!coreStarted) {
            coreStarted = true
            extractAssets()
            nativeThread = thread(name = "sunshine-native", isDaemon = true) {
                Log.i(TAG, "starting native core, dataDir=${filesDir.absolutePath}")
                val rc = SunshineNative.nativeStart(filesDir.absolutePath)
                Log.i(TAG, "native core exited: $rc")
            }
        }
        return START_STICKY
    }

    private fun startCapture(resultCode: Int, resultData: Intent) {
        val mpm = getSystemService(MediaProjectionManager::class.java)
        val mp = mpm.getMediaProjection(resultCode, resultData) ?: run {
            Log.e(TAG, "getMediaProjection returned null")
            return
        }
        projection = mp
        mp.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                Log.i(TAG, "MediaProjection stopped by system")
                stopCapture()
            }
        }, Handler(mainLooper))

        // Capture at the device resolution, capped so the per-frame CPU copy stays cheap.
        val dm = DisplayMetrics()
        @Suppress("DEPRECATION")
        (getSystemService(WINDOW_SERVICE) as WindowManager).defaultDisplay.getRealMetrics(dm)
        var w = dm.widthPixels
        var h = dm.heightPixels
        val dpi = dm.densityDpi
        val maxDim = 1920
        if (max(w, h) > maxDim) {
            val scale = maxDim.toFloat() / max(w, h)
            w = (w * scale).toInt() and 1.inv()  // keep even
            h = (h * scale).toInt() and 1.inv()
        }

        val reader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2)
        imageReader = reader
        val ht = HandlerThread("sunshine-capture").also { it.start() }
        captureThread = ht
        val handler = Handler(ht.looper)

        reader.setOnImageAvailableListener({ r ->
            val image = r.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
                val plane = image.planes[0]
                // nativePushFrame copies synchronously, so the image can be closed right after.
                SunshineNative.nativePushFrame(plane.buffer, image.width, image.height, plane.rowStride)
            } catch (e: Exception) {
                Log.e(TAG, "pushFrame failed", e)
            } finally {
                image.close()
            }
        }, handler)

        virtualDisplay = mp.createVirtualDisplay(
            "sunshine",
            w, h, dpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            reader.surface, null, handler
        )
        SunshineNative.nativeCaptureStarted(w, h)
        Log.i(TAG, "capture started ${w}x$h @ ${dpi}dpi")
    }

    private fun stopCapture() {
        SunshineNative.nativeCaptureStopped()
        virtualDisplay?.release()
        virtualDisplay = null
        imageReader?.close()
        imageReader = null
        projection?.stop()
        projection = null
        captureThread?.quitSafely()
        captureThread = null
    }

    override fun onDestroy() {
        stopCapture()
        if (coreStarted) {
            SunshineNative.nativeStop()
        }
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, "Sunshine", NotificationManager.IMPORTANCE_LOW)
        )
    }

    private fun startForegroundCompat() {
        val notification: Notification = Notification.Builder(this, CHANNEL)
            .setContentTitle("Sunshine")
            .setContentText("Streaming host running")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setOngoing(true)
            .build()
        startForeground(NOTIF_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
    }

    private fun extractAssets() {
        try {
            copyAsset("web", File(filesDir, "assets/web"))
            copyAsset("apps.json", File(filesDir, "apps.json"))
        } catch (e: Exception) {
            Log.e(TAG, "asset extraction failed", e)
        }
    }

    private fun copyAsset(assetPath: String, dest: File) {
        val children = assets.list(assetPath) ?: emptyArray()
        if (children.isEmpty()) {
            dest.parentFile?.mkdirs()
            assets.open(assetPath).use { input ->
                dest.outputStream().use { output -> input.copyTo(output) }
            }
        } else {
            dest.mkdirs()
            for (child in children) {
                copyAsset("$assetPath/$child", File(dest, child))
            }
        }
    }

    companion object {
        private const val TAG = "SunshineService"
        private const val CHANNEL = "sunshine"
        private const val NOTIF_ID = 1
        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "resultData"
    }
}
