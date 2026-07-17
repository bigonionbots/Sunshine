package dev.lizardbyte.sunshine

import android.graphics.Bitmap
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.util.Log
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import android.view.InputDevice
import android.view.InputEvent
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import java.lang.reflect.Method

/**
 * Shizuku UserService running as shell (uid 2000). Creates real uinput kernel devices via
 * [SunshineInputNative] (libsunshine_input.so — tiny, safe to load here) and writes evdev events
 * directly, giving a visible mouse cursor and proper gamepad registration. Falls back to
 * InputManager.injectInputEvent() reflection if uinput creation fails.
 *
 * Must NOT access [SunshineNative] or libsunshine.so — loading that 155 MB library in the
 * Shizuku shell process crashes it.
 */
class InputUserService : IInputBridge.Stub() {

    // uinput fds created as shell (-1 = unavailable, use injectInputEvent fallback)
    private var mouseFd = -1
    private var keyboardFd = -1
    private val gamepadFds = IntArray(4) { -1 }

    // injectInputEvent fallback (used when uinput creation fails)
    private val inputManagerObj: Any by lazy {
        Class.forName("android.hardware.input.InputManager")
            .getMethod("getInstance").invoke(null)!!
    }
    private val injectMethod: Method by lazy {
        inputManagerObj.javaClass
            .getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
            .also { it.isAccessible = true }
    }

    // cursor position tracked for the injectInputEvent fallback only
    private var curX = 500f
    private var curY = 500f
    private var buttonState = 0
    private var downTime = SystemClock.uptimeMillis()

    init {
        mouseFd = try {
            SunshineInputNative.createUinputMouse().also { fd ->
                Log.i(TAG, "uinput mouse fd=$fd")
            }
        } catch (e: Exception) {
            Log.e(TAG, "uinput mouse failed, using injectInputEvent", e)
            -1
        }
        keyboardFd = try {
            SunshineInputNative.createUinputKeyboard().also { fd ->
                Log.i(TAG, "uinput keyboard fd=$fd")
            }
        } catch (e: Exception) {
            Log.e(TAG, "uinput keyboard failed, using injectInputEvent", e)
            -1
        }
    }

    // -----------------------------------------------------------------------------------------
    // evdev event helpers

    private fun ev(fd: Int, type: Int, code: Int, value: Int) =
        SunshineInputNative.writeUinputEvent(fd, type.toShort(), code.toShort(), value)

    private fun syn(fd: Int) = ev(fd, EV_SYN, SYN_REPORT, 0)

    // -----------------------------------------------------------------------------------------
    // Keyboard

    override fun injectKey(vkCode: Int, pressed: Boolean) {
        val v = if (pressed) 1 else 0
        if (keyboardFd >= 0) {
            val linuxKey = vkToLinux[vkCode] ?: return
            ev(keyboardFd, EV_KEY, linuxKey, v)
            syn(keyboardFd)
        } else {
            val keyCode = vkToKeyCode[vkCode] ?: return
            val now = SystemClock.uptimeMillis()
            val action = if (pressed) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP
            injectFallback(KeyEvent(now, now, action, keyCode, 0, 0,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
                KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_VIRTUAL_HARD_KEY,
                InputDevice.SOURCE_KEYBOARD))
        }
    }

    // -----------------------------------------------------------------------------------------
    // Mouse

    override fun injectMouseMove(dx: Float, dy: Float) {
        if (mouseFd >= 0) {
            ev(mouseFd, EV_REL, REL_X, dx.toInt())
            ev(mouseFd, EV_REL, REL_Y, dy.toInt())
            syn(mouseFd)
        } else {
            curX += dx; curY += dy
            val now = SystemClock.uptimeMillis()
            val action = if (buttonState != 0) MotionEvent.ACTION_MOVE else MotionEvent.ACTION_HOVER_MOVE
            motionFallback(now, action, curX, curY, MotionEvent.AXIS_RELATIVE_X to dx, MotionEvent.AXIS_RELATIVE_Y to dy)
        }
    }

    override fun injectMouseButton(button: Int, pressed: Boolean) {
        if (mouseFd >= 0) {
            val btn = when (button) {
                1 -> BTN_LEFT; 2 -> BTN_RIGHT; 4 -> BTN_MIDDLE
                8 -> BTN_SIDE; 16 -> BTN_EXTRA; else -> return
            }
            ev(mouseFd, EV_KEY, btn, if (pressed) 1 else 0)
            syn(mouseFd)
        } else {
            val ab = when (button) {
                1 -> MotionEvent.BUTTON_PRIMARY; 2 -> MotionEvent.BUTTON_SECONDARY
                4 -> MotionEvent.BUTTON_TERTIARY; 8 -> MotionEvent.BUTTON_BACK
                16 -> MotionEvent.BUTTON_FORWARD; else -> return
            }
            val now = SystemClock.uptimeMillis()
            if (pressed) {
                if (buttonState == 0) downTime = now
                buttonState = buttonState or ab
                emit2Fallback(now, MotionEvent.ACTION_DOWN, MotionEvent.ACTION_BUTTON_PRESS)
            } else {
                emit2Fallback(now, MotionEvent.ACTION_BUTTON_RELEASE,
                    if (buttonState and ab.inv() == 0) MotionEvent.ACTION_UP else MotionEvent.ACTION_MOVE)
                buttonState = buttonState and ab.inv()
            }
        }
    }

    override fun injectScroll(distance: Int) {
        if (mouseFd >= 0) {
            ev(mouseFd, EV_REL, REL_WHEEL_HI_RES, distance)
            ev(mouseFd, EV_REL, REL_WHEEL, Math.round(distance / 120f))
            syn(mouseFd)
        } else {
            val now = SystemClock.uptimeMillis()
            motionFallback(now, MotionEvent.ACTION_SCROLL, curX, curY, MotionEvent.AXIS_VSCROLL to distance / 120f)
        }
    }

    override fun injectHScroll(distance: Int) {
        if (mouseFd >= 0) {
            ev(mouseFd, EV_REL, REL_HWHEEL_HI_RES, distance)
            ev(mouseFd, EV_REL, REL_HWHEEL, Math.round(distance / 120f))
            syn(mouseFd)
        } else {
            val now = SystemClock.uptimeMillis()
            motionFallback(now, MotionEvent.ACTION_SCROLL, curX, curY, MotionEvent.AXIS_HSCROLL to distance / 120f)
        }
    }

    // -----------------------------------------------------------------------------------------
    // Gamepad

    override fun allocGamepad(slot: Int) {
        if (slot < 0 || slot >= 4) return
        freeGamepad(slot)
        val fd = try {
            SunshineInputNative.createUinputGamepad(slot)
        } catch (e: Exception) {
            Log.e(TAG, "uinput gamepad $slot failed", e)
            -1
        }
        gamepadFds[slot] = fd
        Log.i(TAG, "allocGamepad($slot) fd=$fd")
    }

    override fun freeGamepad(slot: Int) {
        if (slot < 0 || slot >= 4) return
        val fd = gamepadFds[slot]
        if (fd >= 0) {
            SunshineInputNative.destroyUinputDevice(fd)
            gamepadFds[slot] = -1
        }
    }

    override fun injectGamepadState(slot: Int, buttons: Int, lsX: Float, lsY: Float, rsX: Float, rsY: Float, lt: Float, rt: Float) {
        val fd = gamepadFds.getOrElse(slot) { -1 }
        if (fd < 0) return

        // Sticks (±32767). Moonlight sends Y with up=+32767 (Windows/XInput convention),
        // but Linux ABS_Y expects up=negative. Negate Y on both sticks (mirrors inputtino).
        ev(fd, EV_ABS, ABS_X,   (lsX * 32767).toInt())
        ev(fd, EV_ABS, ABS_Y,  -(lsY * 32767).toInt())
        ev(fd, EV_ABS, ABS_RX,  (rsX * 32767).toInt())
        ev(fd, EV_ABS, ABS_RY, -(rsY * 32767).toInt())

        // Triggers (0–255)
        ev(fd, EV_ABS, ABS_Z,  (lt * 255).toInt())
        ev(fd, EV_ABS, ABS_RZ, (rt * 255).toInt())

        // D-pad via HAT axes
        val hatX = when {
            buttons and 0x0008 != 0 -> 1   // DPAD_RIGHT
            buttons and 0x0004 != 0 -> -1  // DPAD_LEFT
            else -> 0
        }
        val hatY = when {
            buttons and 0x0002 != 0 -> 1   // DPAD_DOWN
            buttons and 0x0001 != 0 -> -1  // DPAD_UP
            else -> 0
        }
        ev(fd, EV_ABS, ABS_HAT0X, hatX)
        ev(fd, EV_ABS, ABS_HAT0Y, hatY)

        // Face + shoulder buttons
        for ((bit, btn) in GAMEPAD_BUTTON_MAP) {
            ev(fd, EV_KEY, btn, if (buttons and bit != 0) 1 else 0)
        }

        syn(fd)
    }

    // -----------------------------------------------------------------------------------------
    // Screen capture (shell-privilege screencap for privacy mode)

    override fun screencapToFd(fd: ParcelFileDescriptor) {
        var proc: Process? = null
        try {
            proc = Runtime.getRuntime().exec("/system/bin/screencap")
            FileOutputStream(fd.fileDescriptor).use { out ->
                proc.inputStream.copyTo(out)
            }
            proc.waitFor()
        } catch (e: Exception) {
            Log.e(TAG, "screencapToFd: ${e.message}")
        } finally {
            proc?.destroy()
            runCatching { fd.close() }
        }
    }

    override fun startScreencapDaemon(fd: ParcelFileDescriptor) {
        // detachFd() transfers ownership of the underlying file descriptor to the native daemon
        // thread, which closes it on exit. Do not close fd after detachFd().
        try {
            SunshineInputNative.nativeStartScreencapDaemon(fd.detachFd())
        } catch (e: Exception) {
            Log.e(TAG, "startScreencapDaemon: ${e.message}")
            runCatching { fd.close() }
        }
    }

    override fun stopScreencapDaemon() {
        SunshineInputNative.nativeStopScreencapDaemon()
    }

    // Pre-allocated pixel storage reused across frames to avoid per-frame GC pressure.
    @Volatile private var capturePixelBuf: ByteArray? = null
    // Whether SurfaceControl reflection has been tested on this device (null = not yet).
    @Volatile private var reflectionWorking: Boolean? = null

    override fun screencapToFdBGRA(fd: ParcelFileDescriptor, width: Int, height: Int) {
        try {
            // Skip reflection if it's already been confirmed unavailable on this device.
            val bitmap: Bitmap? = if (reflectionWorking != false) {
                captureScreenReflection(width, height).also { bmp ->
                    if (reflectionWorking == null) {
                        reflectionWorking = bmp != null
                        if (bmp == null) Log.w(TAG, "SurfaceControl reflection unavailable; screencapToFdBGRA will return empty")
                    }
                }
            } else null

            if (bitmap == null) {
                // Signal to the caller that the reflection path isn't available.
                return
            }

            val bw = bitmap.width
            val bh = bitmap.height
            val pixelBytes = bw * bh * 4

            // Reuse the pixel buffer if large enough.
            var pixBuf = capturePixelBuf
            if (pixBuf == null || pixBuf.size < pixelBytes) {
                pixBuf = ByteArray(pixelBytes)
                capturePixelBuf = pixBuf
            }

            // Bitmap.copyPixelsToBuffer gives raw BGRA bytes on ARM (kN32 = kBGRA_8888 in Skia).
            bitmap.copyPixelsToBuffer(ByteBuffer.wrap(pixBuf, 0, pixelBytes))
            bitmap.recycle()

            // Write 8-byte header (width, height as LE ints) followed by BGRA pixel data.
            val hdr = ByteArray(8).also { ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putInt(bw).putInt(bh) }
            FileOutputStream(fd.fileDescriptor).buffered(65536).use { out ->
                out.write(hdr)
                out.write(pixBuf, 0, pixelBytes)
            }
        } catch (e: Exception) {
            Log.e(TAG, "screencapToFdBGRA: ${e.message}")
        } finally {
            runCatching { fd.close() }
        }
    }

    /**
     * Try multiple hidden APIs to capture the display without spawning a subprocess.
     * On Android 12+ uses [SurfaceControl.captureDisplay]; older devices fall back to
     * [SurfaceControl.screenshot]. Returns null if all attempts fail.
     */
    private fun captureScreenReflection(targetW: Int, targetH: Int): Bitmap? {
        // Approach 1: Android 12+ SurfaceControl.captureDisplay(DisplayCaptureArgs)
        try {
            val scClass = Class.forName("android.view.SurfaceControl")


            // Obtain the primary display token.
            val displayToken: IBinder = try {
                scClass.getDeclaredMethod("getInternalDisplayToken")
                    .apply { isAccessible = true }
                    .invoke(null) as? IBinder
                    ?: throw RuntimeException("getInternalDisplayToken returned null")
            } catch (e: NoSuchMethodException) {
                // Android 12+ replaced getInternalDisplayToken with getPhysicalDisplayToken.
                val ids = scClass.getDeclaredMethod("getPhysicalDisplayIds")
                    .apply { isAccessible = true }
                    .invoke(null) as? LongArray
                    ?: throw RuntimeException("getPhysicalDisplayIds returned null")
                if (ids.isEmpty()) throw RuntimeException("no physical displays")
                scClass.getDeclaredMethod("getPhysicalDisplayToken", Long::class.javaPrimitiveType)
                    .apply { isAccessible = true }
                    .invoke(null, ids[0]) as? IBinder
                    ?: throw RuntimeException("getPhysicalDisplayToken returned null")
            }

            // Build DisplayCaptureArgs via its Builder inner class.
            val builderClass = Class.forName("android.view.SurfaceControl\$DisplayCaptureArgs\$Builder")
            val builder = builderClass.getDeclaredConstructor(IBinder::class.java)
                .apply { isAccessible = true }
                .newInstance(displayToken)
            builderClass.getDeclaredMethod("setSize", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
                .apply { isAccessible = true }
                .invoke(builder, targetW, targetH)
            val captureArgs = builderClass.getDeclaredMethod("build")
                .apply { isAccessible = true }
                .invoke(builder)

            // Call SurfaceControl.captureDisplay(DisplayCaptureArgs).
            val result = scClass.getDeclaredMethod(
                "captureDisplay", Class.forName("android.view.SurfaceControl\$DisplayCaptureArgs"))
                .apply { isAccessible = true }
                .invoke(null, captureArgs)
                ?: return null

            // Extract software Bitmap from the returned ScreenshotHardwareBuffer.
            val bmp = result.javaClass.getDeclaredMethod("asBitmap")
                .apply { isAccessible = true }
                .invoke(result) as? Bitmap
                ?: return null
            // Hardware-backed Bitmaps can't be read via copyPixelsToBuffer; copy to software.
            return if (bmp.config == Bitmap.Config.HARDWARE) bmp.copy(Bitmap.Config.ARGB_8888, false) else bmp
        } catch (e: Exception) {
            Log.w(TAG, "captureDisplay reflection failed: ${e.javaClass.simpleName}: ${e.message}")
        }

        // Approach 2: android.window.ScreenCapture.captureDisplay (Android 13) with display token
        // from android.hardware.display.DisplayControl (ZUI moved these methods out of SurfaceControl).
        try {
            val displayToken: IBinder = getDisplayToken()
                ?: throw RuntimeException("no display token available")

            val builderClass = Class.forName("android.window.ScreenCapture\$DisplayCaptureArgs\$Builder")
            val builder = builderClass.getDeclaredConstructor(IBinder::class.java)
                .apply { isAccessible = true }
                .newInstance(displayToken)
            builderClass.getDeclaredMethod("setSize", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
                .apply { isAccessible = true }
                .invoke(builder, targetW, targetH)
            val captureArgs = builderClass.getDeclaredMethod("build")
                .apply { isAccessible = true }
                .invoke(builder)

            val scapClass = Class.forName("android.window.ScreenCapture")
            val dcaClass = Class.forName("android.window.ScreenCapture\$DisplayCaptureArgs")
            val result = scapClass.getDeclaredMethod("captureDisplay", dcaClass)
                .apply { isAccessible = true }
                .invoke(null, captureArgs)
                ?: return null

            val bmp = result.javaClass.getDeclaredMethod("asBitmap")
                .apply { isAccessible = true }
                .invoke(result) as? Bitmap
                ?: return null
            return if (bmp.config == Bitmap.Config.HARDWARE) bmp.copy(Bitmap.Config.ARGB_8888, false) else bmp
        } catch (e: Exception) {
            Log.w(TAG, "ScreenCapture.captureDisplay reflection failed: ${e.javaClass.simpleName}: ${e.message}")
        }

        // Approach 3: deprecated SurfaceControl.screenshot(Rect, int, int[, int]) pre-Android 12.
        try {
            val scClass = Class.forName("android.view.SurfaceControl")
            val m = try {
                scClass.getDeclaredMethod("screenshot",
                    android.graphics.Rect::class.java, Int::class.javaPrimitiveType,
                    Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
            } catch (_: NoSuchMethodException) {
                scClass.getDeclaredMethod("screenshot",
                    android.graphics.Rect::class.java, Int::class.javaPrimitiveType,
                    Int::class.javaPrimitiveType)
            }
            m.isAccessible = true
            val bmp = if (m.parameterCount == 4) {
                m.invoke(null, null, targetW, targetH, 0)
            } else {
                m.invoke(null, null, targetW, targetH)
            } as? Bitmap
            return if (bmp?.config == Bitmap.Config.HARDWARE) bmp.copy(Bitmap.Config.ARGB_8888, false) else bmp
        } catch (e: Exception) {
            Log.w(TAG, "screenshot reflection failed: ${e.javaClass.simpleName}: ${e.message}")
        }

        return null
    }

    /**
     * Obtain the primary display's token (IBinder) by trying multiple hidden APIs.
     * SurfaceControl.getPhysicalDisplayToken() was removed in ZUI; fallback paths include
     * android.hardware.display.DisplayControl and android.view.SurfaceControl inner paths.
     */
    private fun getDisplayToken(): IBinder? {
        // Try android.hardware.display.DisplayControl (some OEMs move these from SurfaceControl).
        try {
            val dcClass = Class.forName("android.hardware.display.DisplayControl")
            val ids = dcClass.getDeclaredMethod("getPhysicalDisplayIds")
                .apply { isAccessible = true }
                .invoke(null) as? LongArray
            if (ids != null && ids.isNotEmpty()) {
                return dcClass.getDeclaredMethod("getPhysicalDisplayToken", Long::class.javaPrimitiveType)
                    .apply { isAccessible = true }
                    .invoke(null, ids[0]) as? IBinder
            }
        } catch (e: Exception) {
            Log.w(TAG, "DisplayControl token failed: ${e.javaClass.simpleName}: ${e.message}")
        }

        // Try SurfaceControl.getInternalDisplayToken() (stock Android < 12 or some OEM builds).
        try {
            val scClass = Class.forName("android.view.SurfaceControl")
            return scClass.getDeclaredMethod("getInternalDisplayToken")
                .apply { isAccessible = true }
                .invoke(null) as? IBinder
        } catch (e: Exception) {
            Log.w(TAG, "getInternalDisplayToken failed: ${e.javaClass.simpleName}: ${e.message}")
        }

        // Try SurfaceControl.getPhysicalDisplayToken(long) with ID 0 as a last resort.
        try {
            val scClass = Class.forName("android.view.SurfaceControl")
            return scClass.getDeclaredMethod("getPhysicalDisplayToken", Long::class.javaPrimitiveType)
                .apply { isAccessible = true }
                .invoke(null, 0L) as? IBinder
        } catch (e: Exception) {
            Log.w(TAG, "getPhysicalDisplayToken(0L) failed: ${e.javaClass.simpleName}: ${e.message}")
        }

        return null
    }

    // -----------------------------------------------------------------------------------------
    // injectInputEvent fallback helpers

    private fun injectFallback(event: InputEvent) {
        try {
            injectMethod.invoke(inputManagerObj, event, 0)
        } catch (e: Exception) {
            Log.e(TAG, "injectInputEvent failed", e)
        }
    }

    private fun motionFallback(now: Long, action: Int, x: Float, y: Float, vararg axes: Pair<Int, Float>) {
        val props = arrayOf(MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE })
        val coords = arrayOf(MotionEvent.PointerCoords().apply {
            this.x = x; this.y = y
            for ((axis, v) in axes) setAxisValue(axis, v)
        })
        val e = MotionEvent.obtain(downTime, now, action, 1, props, coords,
            0, buttonState, 1f, 1f, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, InputDevice.SOURCE_MOUSE, 0)
        injectFallback(e); e.recycle()
    }

    private fun emit2Fallback(now: Long, a1: Int, a2: Int) {
        val props = arrayOf(MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE })
        val coords = arrayOf(MotionEvent.PointerCoords().apply { x = curX; y = curY })
        fun ev(a: Int) = MotionEvent.obtain(downTime, now, a, 1, props, coords,
            0, buttonState, 1f, 1f, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, InputDevice.SOURCE_MOUSE, 0)
        ev(a1).also { injectFallback(it); it.recycle() }
        ev(a2).also { injectFallback(it); it.recycle() }
    }

    companion object {
        private const val TAG = "InputUserService"

        // evdev constants
        private const val EV_SYN = 0; private const val SYN_REPORT = 0
        private const val EV_KEY = 1; private const val EV_REL = 2; private const val EV_ABS = 3
        private const val REL_X = 0; private const val REL_Y = 1
        private const val REL_WHEEL = 8; private const val REL_HWHEEL = 6
        private const val REL_WHEEL_HI_RES = 11; private const val REL_HWHEEL_HI_RES = 12
        private const val BTN_LEFT = 0x110; private const val BTN_RIGHT = 0x111
        private const val BTN_MIDDLE = 0x112; private const val BTN_SIDE = 0x115
        private const val BTN_EXTRA = 0x116
        private const val ABS_X = 0; private const val ABS_Y = 1
        private const val ABS_RX = 3; private const val ABS_RY = 4
        private const val ABS_Z = 2; private const val ABS_RZ = 5
        private const val ABS_HAT0X = 16; private const val ABS_HAT0Y = 17
        private const val BTN_SOUTH = 0x130; private const val BTN_EAST = 0x131
        private const val BTN_NORTH = 0x133; private const val BTN_WEST = 0x134
        private const val BTN_TL = 0x136; private const val BTN_TR = 0x137
        private const val BTN_SELECT = 0x13A; private const val BTN_START = 0x13B
        private const val BTN_MODE = 0x13C
        private const val BTN_THUMBL = 0x13D; private const val BTN_THUMBR = 0x13E

        // Moonlight buttonFlags bit → Linux BTN_* code
        val GAMEPAD_BUTTON_MAP = listOf(
            0x1000 to BTN_SOUTH,   // A
            0x2000 to BTN_EAST,    // B
            0x4000 to BTN_WEST,    // X
            0x8000 to BTN_NORTH,   // Y
            0x0100 to BTN_TL,      // LB
            0x0200 to BTN_TR,      // RB
            0x0020 to BTN_SELECT,  // Back/Select
            0x0010 to BTN_START,   // Start
            0x0400 to BTN_MODE,    // Guide/Home
            0x0040 to BTN_THUMBL,  // LS click
            0x0080 to BTN_THUMBR,  // RS click
        )

        // Linux KEY_* codes for keyboard (mirrors vk_to_linux in input.cpp)
        val vkToLinux = mapOf(
            0x08 to 14, 0x09 to 15, 0x0D to 28, 0x10 to 42, 0x11 to 29, 0x12 to 56,
            0x14 to 58, 0x1B to 1, 0x20 to 57, 0x21 to 104, 0x22 to 109, 0x23 to 107,
            0x24 to 102, 0x25 to 105, 0x26 to 103, 0x27 to 106, 0x28 to 108, 0x2C to 99,
            0x2D to 110, 0x2E to 111,
            0x30 to 11, 0x31 to 2, 0x32 to 3, 0x33 to 4, 0x34 to 5, 0x35 to 6,
            0x36 to 7, 0x37 to 8, 0x38 to 9, 0x39 to 10,
            0x41 to 30, 0x42 to 48, 0x43 to 46, 0x44 to 32, 0x45 to 18, 0x46 to 33,
            0x47 to 34, 0x48 to 35, 0x49 to 23, 0x4A to 36, 0x4B to 37, 0x4C to 38,
            0x4D to 50, 0x4E to 49, 0x4F to 24, 0x50 to 25, 0x51 to 16, 0x52 to 19,
            0x53 to 31, 0x54 to 20, 0x55 to 22, 0x56 to 47, 0x57 to 17, 0x58 to 45,
            0x59 to 21, 0x5A to 44, 0x5B to 125, 0x5C to 126,
            0x60 to 82, 0x61 to 79, 0x62 to 80, 0x63 to 81, 0x64 to 75, 0x65 to 76,
            0x66 to 77, 0x67 to 71, 0x68 to 72, 0x69 to 73, 0x6A to 55, 0x6B to 78,
            0x6D to 74, 0x6E to 83, 0x6F to 98,
            0x70 to 59, 0x71 to 60, 0x72 to 61, 0x73 to 62, 0x74 to 63, 0x75 to 64,
            0x76 to 65, 0x77 to 66, 0x78 to 67, 0x79 to 68, 0x7A to 87, 0x7B to 88,
            0x90 to 69, 0x91 to 70,
            0xA0 to 42, 0xA1 to 54, 0xA2 to 29, 0xA3 to 97, 0xA4 to 56, 0xA5 to 100,
            0xBA to 39, 0xBB to 13, 0xBC to 51, 0xBD to 12, 0xBE to 52, 0xBF to 53,
            0xC0 to 41, 0xDB to 26, 0xDC to 43, 0xDD to 27, 0xDE to 40, 0xE2 to 86,
        )

        // Android KeyEvent keycodes for injectInputEvent fallback
        val vkToKeyCode = mapOf(
            0x08 to KeyEvent.KEYCODE_DEL, 0x09 to KeyEvent.KEYCODE_TAB,
            0x0D to KeyEvent.KEYCODE_ENTER, 0x10 to KeyEvent.KEYCODE_SHIFT_LEFT,
            0x11 to KeyEvent.KEYCODE_CTRL_LEFT, 0x12 to KeyEvent.KEYCODE_ALT_LEFT,
            0x14 to KeyEvent.KEYCODE_CAPS_LOCK, 0x1B to KeyEvent.KEYCODE_ESCAPE,
            0x20 to KeyEvent.KEYCODE_SPACE, 0x21 to KeyEvent.KEYCODE_PAGE_UP,
            0x22 to KeyEvent.KEYCODE_PAGE_DOWN, 0x23 to KeyEvent.KEYCODE_MOVE_END,
            0x24 to KeyEvent.KEYCODE_MOVE_HOME, 0x25 to KeyEvent.KEYCODE_DPAD_LEFT,
            0x26 to KeyEvent.KEYCODE_DPAD_UP, 0x27 to KeyEvent.KEYCODE_DPAD_RIGHT,
            0x28 to KeyEvent.KEYCODE_DPAD_DOWN, 0x2D to KeyEvent.KEYCODE_INSERT,
            0x2E to KeyEvent.KEYCODE_FORWARD_DEL,
            0x30 to KeyEvent.KEYCODE_0, 0x31 to KeyEvent.KEYCODE_1, 0x32 to KeyEvent.KEYCODE_2,
            0x33 to KeyEvent.KEYCODE_3, 0x34 to KeyEvent.KEYCODE_4, 0x35 to KeyEvent.KEYCODE_5,
            0x36 to KeyEvent.KEYCODE_6, 0x37 to KeyEvent.KEYCODE_7, 0x38 to KeyEvent.KEYCODE_8,
            0x39 to KeyEvent.KEYCODE_9,
            0x41 to KeyEvent.KEYCODE_A, 0x42 to KeyEvent.KEYCODE_B, 0x43 to KeyEvent.KEYCODE_C,
            0x44 to KeyEvent.KEYCODE_D, 0x45 to KeyEvent.KEYCODE_E, 0x46 to KeyEvent.KEYCODE_F,
            0x47 to KeyEvent.KEYCODE_G, 0x48 to KeyEvent.KEYCODE_H, 0x49 to KeyEvent.KEYCODE_I,
            0x4A to KeyEvent.KEYCODE_J, 0x4B to KeyEvent.KEYCODE_K, 0x4C to KeyEvent.KEYCODE_L,
            0x4D to KeyEvent.KEYCODE_M, 0x4E to KeyEvent.KEYCODE_N, 0x4F to KeyEvent.KEYCODE_O,
            0x50 to KeyEvent.KEYCODE_P, 0x51 to KeyEvent.KEYCODE_Q, 0x52 to KeyEvent.KEYCODE_R,
            0x53 to KeyEvent.KEYCODE_S, 0x54 to KeyEvent.KEYCODE_T, 0x55 to KeyEvent.KEYCODE_U,
            0x56 to KeyEvent.KEYCODE_V, 0x57 to KeyEvent.KEYCODE_W, 0x58 to KeyEvent.KEYCODE_X,
            0x59 to KeyEvent.KEYCODE_Y, 0x5A to KeyEvent.KEYCODE_Z,
            0x5B to KeyEvent.KEYCODE_META_LEFT, 0x5C to KeyEvent.KEYCODE_META_RIGHT,
            0x70 to KeyEvent.KEYCODE_F1,  0x71 to KeyEvent.KEYCODE_F2,
            0x72 to KeyEvent.KEYCODE_F3,  0x73 to KeyEvent.KEYCODE_F4,
            0x74 to KeyEvent.KEYCODE_F5,  0x75 to KeyEvent.KEYCODE_F6,
            0x76 to KeyEvent.KEYCODE_F7,  0x77 to KeyEvent.KEYCODE_F8,
            0x78 to KeyEvent.KEYCODE_F9,  0x79 to KeyEvent.KEYCODE_F10,
            0x7A to KeyEvent.KEYCODE_F11, 0x7B to KeyEvent.KEYCODE_F12,
            0xA0 to KeyEvent.KEYCODE_SHIFT_LEFT,  0xA1 to KeyEvent.KEYCODE_SHIFT_RIGHT,
            0xA2 to KeyEvent.KEYCODE_CTRL_LEFT,   0xA3 to KeyEvent.KEYCODE_CTRL_RIGHT,
            0xA4 to KeyEvent.KEYCODE_ALT_LEFT,    0xA5 to KeyEvent.KEYCODE_ALT_RIGHT,
            0xBA to KeyEvent.KEYCODE_SEMICOLON, 0xBB to KeyEvent.KEYCODE_EQUALS,
            0xBC to KeyEvent.KEYCODE_COMMA, 0xBD to KeyEvent.KEYCODE_MINUS,
            0xBE to KeyEvent.KEYCODE_PERIOD, 0xBF to KeyEvent.KEYCODE_SLASH,
            0xC0 to KeyEvent.KEYCODE_GRAVE, 0xDB to KeyEvent.KEYCODE_LEFT_BRACKET,
            0xDC to KeyEvent.KEYCODE_BACKSLASH, 0xDD to KeyEvent.KEYCODE_RIGHT_BRACKET,
            0xDE to KeyEvent.KEYCODE_APOSTROPHE,
        )
    }
}
