package dev.lizardbyte.sunshine

import android.os.SystemClock
import android.util.Log
import android.view.InputDevice
import android.view.InputEvent
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import java.lang.reflect.Method

/**
 * Shizuku UserService that runs as the shell user (uid 2000), which holds
 * android.permission.INJECT_EVENTS. Every AIDL method call executes in this process, so the
 * InputManager.injectInputEvent() reflection call is made under the shell SELinux context.
 */
class InputUserService : IInputBridge.Stub() {

    // InputManager and its injectInputEvent() method are both @hide; full reflection is required.
    private val inputManagerObj: Any by lazy {
        Class.forName("android.hardware.input.InputManager")
            .getMethod("getInstance")
            .invoke(null)!!
    }
    private val injectMethod: Method by lazy {
        inputManagerObj.javaClass.getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
            .also { it.isAccessible = true }
    }

    private var curX = 500f
    private var curY = 500f
    private var buttonState = 0
    private var downTime = SystemClock.uptimeMillis()

    private fun inject(event: InputEvent) {
        try {
            injectMethod.invoke(inputManagerObj, event, 0 /* INJECT_INPUT_EVENT_MODE_ASYNC */)
        } catch (e: Exception) {
            Log.e(TAG, "injectInputEvent failed", e)
        }
    }

    // -----------------------------------------------------------------------------------------
    // Keyboard

    override fun injectKey(vkCode: Int, pressed: Boolean) {
        val keyCode = vkToKeyCode[vkCode] ?: run {
            Log.v(TAG, "unmapped VK 0x${vkCode.toString(16)}")
            return
        }
        val now = SystemClock.uptimeMillis()
        val action = if (pressed) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP
        val event = KeyEvent(now, now, action, keyCode, 0, 0,
            KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
            KeyEvent.FLAG_FROM_SYSTEM or KeyEvent.FLAG_VIRTUAL_HARD_KEY,
            InputDevice.SOURCE_KEYBOARD)
        inject(event)
    }

    // -----------------------------------------------------------------------------------------
    // Mouse movement

    override fun injectMouseMove(dx: Float, dy: Float) {
        curX += dx
        curY += dy
        val now = SystemClock.uptimeMillis()
        val action = if (buttonState != 0) MotionEvent.ACTION_MOVE else MotionEvent.ACTION_HOVER_MOVE
        val props = arrayOf(MotionEvent.PointerProperties().apply {
            id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE
        })
        val coords = arrayOf(MotionEvent.PointerCoords().apply {
            x = curX; y = curY
            setAxisValue(MotionEvent.AXIS_RELATIVE_X, dx)
            setAxisValue(MotionEvent.AXIS_RELATIVE_Y, dy)
        })
        val event = MotionEvent.obtain(downTime, now, action, 1, props, coords,
            0, buttonState, 1f, 1f, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
            InputDevice.SOURCE_MOUSE, 0)
        inject(event)
        event.recycle()
    }

    // -----------------------------------------------------------------------------------------
    // Mouse buttons

    override fun injectMouseButton(button: Int, pressed: Boolean) {
        val androidButton = when (button) {
            1  -> MotionEvent.BUTTON_PRIMARY
            2  -> MotionEvent.BUTTON_SECONDARY
            4  -> MotionEvent.BUTTON_TERTIARY
            8  -> MotionEvent.BUTTON_BACK
            16 -> MotionEvent.BUTTON_FORWARD
            else -> return
        }
        val now = SystemClock.uptimeMillis()
        if (pressed) {
            if (buttonState == 0) downTime = now
            buttonState = buttonState or androidButton
            emitButtonEvent(now, MotionEvent.ACTION_DOWN, MotionEvent.ACTION_BUTTON_PRESS)
        } else {
            emitButtonEvent(now, MotionEvent.ACTION_BUTTON_RELEASE,
                if (buttonState and androidButton.inv() == 0) MotionEvent.ACTION_UP else MotionEvent.ACTION_MOVE)
            buttonState = buttonState and androidButton.inv()
        }
    }

    private fun emitButtonEvent(now: Long, a1: Int, a2: Int) {
        val props = arrayOf(MotionEvent.PointerProperties().apply {
            id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE
        })
        val coords = arrayOf(MotionEvent.PointerCoords().apply { x = curX; y = curY })
        fun ev(a: Int) = MotionEvent.obtain(downTime, now, a, 1, props, coords,
            0, buttonState, 1f, 1f, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
            InputDevice.SOURCE_MOUSE, 0)
        ev(a1).also { inject(it); it.recycle() }
        ev(a2).also { inject(it); it.recycle() }
    }

    // -----------------------------------------------------------------------------------------
    // Scroll

    override fun injectScroll(distance: Int) {
        val now = SystemClock.uptimeMillis()
        val props = arrayOf(MotionEvent.PointerProperties().apply {
            id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE
        })
        val coords = arrayOf(MotionEvent.PointerCoords().apply {
            x = curX; y = curY
            setAxisValue(MotionEvent.AXIS_VSCROLL, distance / 120f)
        })
        val event = MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1, props, coords,
            0, 0, 1f, 1f, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, InputDevice.SOURCE_MOUSE, 0)
        inject(event)
        event.recycle()
    }

    override fun injectHScroll(distance: Int) {
        val now = SystemClock.uptimeMillis()
        val props = arrayOf(MotionEvent.PointerProperties().apply {
            id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE
        })
        val coords = arrayOf(MotionEvent.PointerCoords().apply {
            x = curX; y = curY
            setAxisValue(MotionEvent.AXIS_HSCROLL, distance / 120f)
        })
        val event = MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1, props, coords,
            0, 0, 1f, 1f, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, InputDevice.SOURCE_MOUSE, 0)
        inject(event)
        event.recycle()
    }

    companion object {
        private const val TAG = "InputUserService"

        val vkToKeyCode = mapOf(
            0x08 to KeyEvent.KEYCODE_DEL,
            0x09 to KeyEvent.KEYCODE_TAB,
            0x0D to KeyEvent.KEYCODE_ENTER,
            0x10 to KeyEvent.KEYCODE_SHIFT_LEFT,
            0x11 to KeyEvent.KEYCODE_CTRL_LEFT,
            0x12 to KeyEvent.KEYCODE_ALT_LEFT,
            0x14 to KeyEvent.KEYCODE_CAPS_LOCK,
            0x1B to KeyEvent.KEYCODE_ESCAPE,
            0x20 to KeyEvent.KEYCODE_SPACE,
            0x21 to KeyEvent.KEYCODE_PAGE_UP,
            0x22 to KeyEvent.KEYCODE_PAGE_DOWN,
            0x23 to KeyEvent.KEYCODE_MOVE_END,
            0x24 to KeyEvent.KEYCODE_MOVE_HOME,
            0x25 to KeyEvent.KEYCODE_DPAD_LEFT,
            0x26 to KeyEvent.KEYCODE_DPAD_UP,
            0x27 to KeyEvent.KEYCODE_DPAD_RIGHT,
            0x28 to KeyEvent.KEYCODE_DPAD_DOWN,
            0x2C to KeyEvent.KEYCODE_SYSRQ,
            0x2D to KeyEvent.KEYCODE_INSERT,
            0x2E to KeyEvent.KEYCODE_FORWARD_DEL,
            0x30 to KeyEvent.KEYCODE_0, 0x31 to KeyEvent.KEYCODE_1,
            0x32 to KeyEvent.KEYCODE_2, 0x33 to KeyEvent.KEYCODE_3,
            0x34 to KeyEvent.KEYCODE_4, 0x35 to KeyEvent.KEYCODE_5,
            0x36 to KeyEvent.KEYCODE_6, 0x37 to KeyEvent.KEYCODE_7,
            0x38 to KeyEvent.KEYCODE_8, 0x39 to KeyEvent.KEYCODE_9,
            0x41 to KeyEvent.KEYCODE_A, 0x42 to KeyEvent.KEYCODE_B,
            0x43 to KeyEvent.KEYCODE_C, 0x44 to KeyEvent.KEYCODE_D,
            0x45 to KeyEvent.KEYCODE_E, 0x46 to KeyEvent.KEYCODE_F,
            0x47 to KeyEvent.KEYCODE_G, 0x48 to KeyEvent.KEYCODE_H,
            0x49 to KeyEvent.KEYCODE_I, 0x4A to KeyEvent.KEYCODE_J,
            0x4B to KeyEvent.KEYCODE_K, 0x4C to KeyEvent.KEYCODE_L,
            0x4D to KeyEvent.KEYCODE_M, 0x4E to KeyEvent.KEYCODE_N,
            0x4F to KeyEvent.KEYCODE_O, 0x50 to KeyEvent.KEYCODE_P,
            0x51 to KeyEvent.KEYCODE_Q, 0x52 to KeyEvent.KEYCODE_R,
            0x53 to KeyEvent.KEYCODE_S, 0x54 to KeyEvent.KEYCODE_T,
            0x55 to KeyEvent.KEYCODE_U, 0x56 to KeyEvent.KEYCODE_V,
            0x57 to KeyEvent.KEYCODE_W, 0x58 to KeyEvent.KEYCODE_X,
            0x59 to KeyEvent.KEYCODE_Y, 0x5A to KeyEvent.KEYCODE_Z,
            0x5B to KeyEvent.KEYCODE_META_LEFT,
            0x5C to KeyEvent.KEYCODE_META_RIGHT,
            0x60 to KeyEvent.KEYCODE_NUMPAD_0, 0x61 to KeyEvent.KEYCODE_NUMPAD_1,
            0x62 to KeyEvent.KEYCODE_NUMPAD_2, 0x63 to KeyEvent.KEYCODE_NUMPAD_3,
            0x64 to KeyEvent.KEYCODE_NUMPAD_4, 0x65 to KeyEvent.KEYCODE_NUMPAD_5,
            0x66 to KeyEvent.KEYCODE_NUMPAD_6, 0x67 to KeyEvent.KEYCODE_NUMPAD_7,
            0x68 to KeyEvent.KEYCODE_NUMPAD_8, 0x69 to KeyEvent.KEYCODE_NUMPAD_9,
            0x6A to KeyEvent.KEYCODE_NUMPAD_MULTIPLY,
            0x6B to KeyEvent.KEYCODE_NUMPAD_ADD,
            0x6D to KeyEvent.KEYCODE_NUMPAD_SUBTRACT,
            0x6E to KeyEvent.KEYCODE_NUMPAD_DOT,
            0x6F to KeyEvent.KEYCODE_NUMPAD_DIVIDE,
            0x70 to KeyEvent.KEYCODE_F1,  0x71 to KeyEvent.KEYCODE_F2,
            0x72 to KeyEvent.KEYCODE_F3,  0x73 to KeyEvent.KEYCODE_F4,
            0x74 to KeyEvent.KEYCODE_F5,  0x75 to KeyEvent.KEYCODE_F6,
            0x76 to KeyEvent.KEYCODE_F7,  0x77 to KeyEvent.KEYCODE_F8,
            0x78 to KeyEvent.KEYCODE_F9,  0x79 to KeyEvent.KEYCODE_F10,
            0x7A to KeyEvent.KEYCODE_F11, 0x7B to KeyEvent.KEYCODE_F12,
            0x90 to KeyEvent.KEYCODE_NUM_LOCK,
            0x91 to KeyEvent.KEYCODE_SCROLL_LOCK,
            0xA0 to KeyEvent.KEYCODE_SHIFT_LEFT,
            0xA1 to KeyEvent.KEYCODE_SHIFT_RIGHT,
            0xA2 to KeyEvent.KEYCODE_CTRL_LEFT,
            0xA3 to KeyEvent.KEYCODE_CTRL_RIGHT,
            0xA4 to KeyEvent.KEYCODE_ALT_LEFT,
            0xA5 to KeyEvent.KEYCODE_ALT_RIGHT,
            0xBA to KeyEvent.KEYCODE_SEMICOLON,
            0xBB to KeyEvent.KEYCODE_EQUALS,
            0xBC to KeyEvent.KEYCODE_COMMA,
            0xBD to KeyEvent.KEYCODE_MINUS,
            0xBE to KeyEvent.KEYCODE_PERIOD,
            0xBF to KeyEvent.KEYCODE_SLASH,
            0xC0 to KeyEvent.KEYCODE_GRAVE,
            0xDB to KeyEvent.KEYCODE_LEFT_BRACKET,
            0xDC to KeyEvent.KEYCODE_BACKSLASH,
            0xDD to KeyEvent.KEYCODE_RIGHT_BRACKET,
            0xDE to KeyEvent.KEYCODE_APOSTROPHE,
        )
    }
}
