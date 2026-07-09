package dev.lizardbyte.sunshine

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
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
import java.nio.ByteBuffer
import java.nio.ByteOrder
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
    private var captureW = 0
    private var captureH = 0
    private var captureDpi = 0

    private val encoderLock = Any()
    private var encoder: MediaCodecEncoder? = null

    private var audioRecord: AudioRecord? = null
    private var audioThread: Thread? = null
    @Volatile
    private var audioRunning = false

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
                // The core owns process-global state and can't be cleanly re-initialized, and its
                // shutdown watchdog assumes the process exits after it returns. So run one core per
                // process and terminate when it stops; a future Start spawns a fresh process.
                Log.i(TAG, "native core exited ($rc); terminating process")
                android.os.Process.killProcess(android.os.Process.myPid())
            }
        }
        // NOT_STICKY: never auto-restart without a fresh MediaProjection token (a restart would
        // otherwise run with no capture).
        return START_NOT_STICKY
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
        captureW = w
        captureH = h
        captureDpi = dpi
        SunshineNative.nativeCaptureStarted(w, h)
        Log.i(TAG, "capture started ${w}x$h @ ${dpi}dpi")

        // Offer MediaCodec hardware encode to the core. When a stream launches, the core calls back
        // into startEncoder() and we point this VirtualDisplay at the encoder's input Surface.
        SunshineNative.nativeSetVideoBridge(this)

        startAudioCapture(mp)
    }

    // --- MediaCodec bridge (called from native encoder threads) -------------------------------

    /**
     * Start MediaCodec surface encode and redirect the capture VirtualDisplay into it. Called from
     * the native encode thread when a stream launches; returns false to make the core fall back to
     * software encode.
     */
    fun startEncoder(width: Int, height: Int, codec: Int, fps: Int, bitrateBps: Int): Boolean {
        synchronized(encoderLock) {
            val vd = virtualDisplay ?: return false
            try {
                val enc = MediaCodecEncoder(width, height, codec, fps, bitrateBps) { buf, size, flags, ptsUs ->
                    SunshineNative.nativePushEncoded(buf, size, flags, ptsUs)
                }
                // Render the mirrored screen into the encoder at the client-negotiated resolution.
                vd.resize(width, height, captureDpi)
                vd.setSurface(enc.inputSurface)
                encoder = enc
                Log.i(TAG, "hardware encoder started ${width}x$height codec=$codec")
                return true
            } catch (e: Exception) {
                Log.e(TAG, "startEncoder failed; falling back to software", e)
                encoder?.stop()
                encoder = null
                // Restore the preview surface so capture keeps working for the software path.
                try {
                    vd.setSurface(imageReader?.surface)
                    vd.resize(captureW, captureH, captureDpi)
                } catch (_: Exception) {
                }
                return false
            }
        }
    }

    /** Stop MediaCodec encode and restore the preview capture. */
    fun stopEncoder() {
        synchronized(encoderLock) {
            val enc = encoder ?: return
            encoder = null
            try {
                virtualDisplay?.setSurface(imageReader?.surface)
                virtualDisplay?.resize(captureW, captureH, captureDpi)
            } catch (_: Exception) {
            }
            enc.stop()
            Log.i(TAG, "hardware encoder stopped")
        }
    }

    /** Ask MediaCodec for an IDR/sync frame. */
    fun requestKeyframe() {
        synchronized(encoderLock) { encoder?.requestKeyframe() }
    }

    /** Update the MediaCodec target bitrate (bits per second). */
    fun setBitrate(bitrateBps: Int) {
        synchronized(encoderLock) { encoder?.setBitrate(bitrateBps) }
    }

    /**
     * Capture playback audio for the same MediaProjection session via AudioPlaybackCapture and
     * push interleaved float PCM to the core. Best-effort: any failure leaves the host video-only.
     */
    private fun startAudioCapture(mp: MediaProjection) {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "RECORD_AUDIO not granted; streaming without audio")
            return
        }

        val captureConfig = AudioPlaybackCaptureConfiguration.Builder(mp)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()
        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
            .setSampleRate(SAMPLE_RATE)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()

        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT
        )
        val record = try {
            AudioRecord.Builder()
                .setAudioPlaybackCaptureConfig(captureConfig)
                .setAudioFormat(format)
                .setBufferSizeInBytes(max(minBuf, SAMPLE_RATE * CHANNELS * 4 / 5))  // ~200ms
                .build()
        } catch (e: Exception) {
            Log.e(TAG, "AudioRecord build failed; streaming without audio", e)
            return
        }
        if (record.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord not initialized; streaming without audio")
            record.release()
            return
        }

        audioRecord = record
        record.startRecording()
        SunshineNative.nativeAudioStarted(SAMPLE_RATE, CHANNELS)
        audioRunning = true
        audioThread = thread(name = "sunshine-audio", isDaemon = true) {
            // 10 ms interleaved-float chunks, delivered zero-copy via a direct ByteBuffer.
            val floats = SAMPLE_RATE / 100 * CHANNELS
            val direct = ByteBuffer.allocateDirect(floats * 4).order(ByteOrder.nativeOrder())
            val fb = direct.asFloatBuffer()
            val tmp = FloatArray(floats)
            while (audioRunning) {
                val n = record.read(tmp, 0, floats, AudioRecord.READ_BLOCKING)
                if (n <= 0) {
                    if (n == AudioRecord.ERROR_INVALID_OPERATION || n == AudioRecord.ERROR_DEAD_OBJECT) {
                        Log.e(TAG, "AudioRecord.read fatal error $n; stopping audio thread")
                        break
                    }
                    continue
                }
                fb.clear()
                fb.put(tmp, 0, n)
                SunshineNative.nativePushAudio(direct, n)
            }
        }
        Log.i(TAG, "audio capture started ${SAMPLE_RATE}Hz x$CHANNELS")
    }

    private fun stopAudioCapture() {
        audioRunning = false
        audioThread?.join(500)
        audioThread = null
        audioRecord?.let {
            try {
                it.stop()
            } catch (_: Exception) {
            }
            it.release()
        }
        audioRecord = null
        SunshineNative.nativeAudioStopped()
    }

    private fun stopCapture() {
        SunshineNative.nativeSetVideoBridge(null)
        stopEncoder()
        stopAudioCapture()
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
        // MEDIA_PROJECTION carries capture; MICROPHONE covers AudioPlaybackCapture's RECORD_AUDIO.
        var type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        }
        startForeground(NOTIF_ID, notification, type)
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
        private const val SAMPLE_RATE = 48000  // Sunshine's Opus pipeline runs at 48 kHz.
        private const val CHANNELS = 2  // AudioPlaybackCapture is captured as stereo.
    }
}
