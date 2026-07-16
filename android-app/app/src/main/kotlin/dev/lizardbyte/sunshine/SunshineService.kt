package dev.lizardbyte.sunshine

import android.Manifest
import android.app.Notification
import android.app.PendingIntent
import android.content.ComponentName
import android.content.ServiceConnection
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
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.util.DisplayMetrics
import android.util.Log
import android.view.WindowManager
import rikka.shizuku.Shizuku
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread
import kotlin.math.max

/**
 * Foreground service that hosts the native Sunshine core and feeds it screen frames. Supports two
 * capture modes selected by [PREF_SCREENCAP] in SharedPreferences:
 *
 * - **MediaProjection** (default): VirtualDisplay → ImageReader / MediaCodec surface. Requires
 *   user consent each launch and shows a screen-recording indicator. Supports audio capture and
 *   hardware H.265 encode.
 *
 * - **Screencap** (privacy mode): the native core's built-in `/system/bin/screencap` fallback.
 *   No screen-recording indicator or consent dialog. Trade-offs: ~3–10 fps, software encode only,
 *   no audio. FLAG_SECURE windows (password dialogs, banking apps) still appear black.
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

    private var inputBridge: IInputBridge? = null
    private val inputServiceArgs = Shizuku.UserServiceArgs(
        ComponentName("dev.lizardbyte.sunshine", InputUserService::class.java.name))
        .daemon(false)
        .processNameSuffix("input")
        .debuggable(false)
        .version(2)  // increment when IInputBridge AIDL changes to force UserService restart
    private val inputServiceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val bridge = IInputBridge.Stub.asInterface(binder)
            inputBridge = bridge
            SunshineNative.nativeSetInputBridge(bridge)
            Log.i(TAG, "Shizuku input bridge connected")
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            inputBridge = null
            SunshineNative.nativeSetInputBridge(null)
            Log.i(TAG, "Shizuku input bridge disconnected")
        }
    }

    private var audioRecord: AudioRecord? = null
    private var audioThread: Thread? = null
    @Volatile
    private var audioRunning = false

    @Volatile
    private var screencapRunning = false
    private var screencapThread: Thread? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val screencapMode = getSharedPreferences("sunshine", MODE_PRIVATE)
            .getBoolean(PREF_SCREENCAP, false)

        startForegroundCompat(screencapMode)

        val resultCode = intent?.getIntExtra(EXTRA_RESULT_CODE, 0) ?: 0
        val resultData: Intent? = if (Build.VERSION.SDK_INT >= 33) {
            intent?.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
        } else {
            @Suppress("DEPRECATION") intent?.getParcelableExtra(EXTRA_RESULT_DATA)
        }

        if (!screencapMode && projection == null && resultCode != 0 && resultData != null) {
            startCapture(resultCode, resultData)
        } else if (screencapMode) {
            startScreencapCapture()
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

        // Offer MediaCodec hardware encode to the core.
        SunshineNative.nativeSetVideoBridge(this)

        // Bind the Shizuku input UserService for non-root keyboard/mouse injection.
        bindInputService()

        startAudioCapture(mp)
    }

    // --- Shizuku input bridge ----------------------------------------------------------------

    private fun bindInputService() {
        if (!Shizuku.pingBinder()) {
            Log.w(TAG, "Shizuku not running; input will be unavailable on non-rooted device")
            return
        }
        if (Shizuku.checkSelfPermission() != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "Shizuku permission not granted; input unavailable")
            return
        }
        try {
            Shizuku.bindUserService(inputServiceArgs, inputServiceConnection)
        } catch (e: Exception) {
            Log.e(TAG, "bindUserService failed", e)
        }
    }

    private fun unbindInputService() {
        SunshineNative.nativeSetInputBridge(null)
        inputBridge = null
        try {
            Shizuku.unbindUserService(inputServiceArgs, inputServiceConnection, true)
        } catch (_: Exception) {
        }
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

    /**
     * Start a screencap loop for privacy mode. [InputUserService] runs `/system/bin/screencap`
     * as shell (uid 2000) and writes the raw binary output through a pipe. A concurrent reader
     * thread drains the pipe to prevent the writer from blocking on the 64 KB kernel pipe buffer
     * (a full 1080p RGBA frame is ~8 MB). Parsed frames are pushed to the native core via
     * [SunshineNative.nativePushFrame] using the same `use_jni_capture` path as MediaProjection.
     */
    private fun startScreencapCapture() {
        val wm = getSystemService(WINDOW_SERVICE) as WindowManager
        val dm = DisplayMetrics()
        @Suppress("DEPRECATION") wm.defaultDisplay.getRealMetrics(dm)
        val w = dm.widthPixels
        val h = dm.heightPixels
        // Announce capture before nativeStart() so platf::display() sees capture_active() == true
        // and takes the use_jni_capture path rather than falling back to popen(screencap) as
        // app UID (which fails — screencap requires shell or root).
        SunshineNative.nativeCaptureStarted(w, h)
        Log.i(TAG, "screencap mode: capture announced ${w}x${h}")
        bindInputService()

        screencapRunning = true
        val initW = w
        val initH = h
        screencapThread = thread(name = "sunshine-screencap", isDaemon = true) {
            var directBuf = ByteBuffer.allocateDirect(initW * initH * 4)
            // Track what we announced to nativeCaptureStarted so we can update it when the
            // actual screencap dimensions differ (e.g. navigation bar, display cutout rounding).
            var announcedW = initW
            var announcedH = initH
            while (screencapRunning) {
                val bridge = inputBridge
                if (bridge == null) {
                    // Shizuku binds asynchronously; wait for the bridge before capturing.
                    try { Thread.sleep(100) } catch (_: InterruptedException) { break }
                    continue
                }
                try {
                    val pipe = ParcelFileDescriptor.createPipe()
                    val readFd = pipe[0]
                    val writeFd = pipe[1]

                    // Read from the pipe on a separate thread to prevent the writer from blocking
                    // when screencap output exceeds the kernel pipe buffer (~64 KB). The writer
                    // (InputUserService.screencapToFd) runs synchronously over Binder — it would
                    // deadlock if nothing is draining the pipe.
                    var raw = ByteArray(0)
                    val reader = thread(isDaemon = true) {
                        ParcelFileDescriptor.AutoCloseInputStream(readFd).use { raw = it.readBytes() }
                    }

                    try {
                        bridge.screencapToFd(writeFd)
                    } finally {
                        writeFd.close()  // signal EOF; InputUserService already closed its dup
                    }
                    reader.join()

                    if (raw.size < 12) {
                        Log.w(TAG, "screencap: output too short (${raw.size} bytes)")
                        Thread.sleep(300)
                        continue
                    }
                    val hdr = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
                    val capW = hdr.getInt(0)
                    val capH = hdr.getInt(4)
                    val pixelBytes = capW * capH * 4
                    val pixelOffset = when {
                        raw.size == 12 + pixelBytes -> 12
                        raw.size == 16 + pixelBytes -> 16  // newer Android prepends a dataspace field
                        else -> {
                            Log.w(TAG, "screencap: unexpected size ${raw.size} for ${capW}x${capH}")
                            Thread.sleep(300)
                            continue
                        }
                    }

                    // If the actual screencap dimensions differ from what we pre-announced, update
                    // nativeCaptureStarted() so the native core uses the correct size on the next
                    // reinit. The first frame with wrong dims will trigger a reinit via resize;
                    // after that the dims match and content flows through correctly.
                    if (capW != announcedW || capH != announcedH) {
                        Log.w(TAG, "screencap: actual dims ${capW}x${capH} differ from announced ${announcedW}x${announcedH}; updating")
                        SunshineNative.nativeCaptureStarted(capW, capH)
                        announcedW = capW
                        announcedH = capH
                    }

                    if (directBuf.capacity() < pixelBytes) {
                        directBuf = ByteBuffer.allocateDirect(pixelBytes)
                    }
                    directBuf.clear()
                    directBuf.put(raw, pixelOffset, pixelBytes)
                    SunshineNative.nativePushFrame(directBuf, capW, capH, capW * 4)
                } catch (e: InterruptedException) {
                    break
                } catch (e: Exception) {
                    if (screencapRunning) Log.e(TAG, "screencap: ${e.message}")
                    try { Thread.sleep(300) } catch (_: InterruptedException) { break }
                }
            }
        }
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
        screencapRunning = false
        screencapThread?.interrupt()
        screencapThread?.join(1000)
        screencapThread = null
        unbindInputService()
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

    private fun startForegroundCompat(screencapMode: Boolean) {
        // Tapping the notification opens the Sunshine web UI in the default browser.
        val webIntent = PendingIntent.getActivity(
            this, 0,
            Intent(Intent.ACTION_VIEW, Uri.parse("https://localhost:47990")),
            PendingIntent.FLAG_IMMUTABLE
        )

        val modeText = if (screencapMode) "Privacy mode (screencap)" else "MediaProjection capture"
        val notification: Notification = Notification.Builder(this, CHANNEL)
            .setContentTitle("Sunshine is running")
            .setContentText(modeText)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setOngoing(true)
            .setContentIntent(webIntent)
            .addAction(
                Notification.Action.Builder(
                    null,
                    "Open Settings",
                    webIntent
                ).build()
            )
            .build()

        if (screencapMode) {
            // Screencap mode: no MediaProjection, no microphone capture. Use dataSync type so the
            // foreground service is valid on Android 14+ without a MediaProjection token.
            startForeground(NOTIF_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            var type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
                type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            }
            startForeground(NOTIF_ID, notification, type)
        }
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
        /** SharedPreferences key: true = use screencap fallback instead of MediaProjection. */
        const val PREF_SCREENCAP = "screencap_mode"
        private const val SAMPLE_RATE = 48000  // Sunshine's Opus pipeline runs at 48 kHz.
        private const val CHANNELS = 2  // AudioPlaybackCapture is captured as stereo.
    }
}
