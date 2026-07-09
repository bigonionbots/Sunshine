package dev.lizardbyte.sunshine

/**
 * Bridge to libsunshine_input.so — a tiny uinput helper library that is safe to load in the
 * Shizuku UserService (shell) process. Intentionally separate from [SunshineNative] / libsunshine.so
 * which carries heavy initialization that crashes when loaded in a second process.
 */
object SunshineInputNative {
    init {
        System.loadLibrary("sunshine_input")
    }

    /** Create and configure a uinput virtual mouse. Returns an open fd or -1. */
    external fun createUinputMouse(): Int

    /** Create and configure a uinput virtual keyboard. Returns an open fd or -1. */
    external fun createUinputKeyboard(): Int

    /**
     * Create and configure a uinput virtual gamepad for the given slot (0–3).
     * Returns an open fd or -1.
     */
    external fun createUinputGamepad(slot: Int): Int

    /** Destroy a uinput device and close its fd. */
    external fun destroyUinputDevice(fd: Int)

    /**
     * Write one evdev event to an open uinput fd. Calls are made directly in the shell process,
     * so the kernel receives events without going through InputManager.injectInputEvent().
     */
    external fun writeUinputEvent(fd: Int, type: Short, code: Short, value: Int)
}
