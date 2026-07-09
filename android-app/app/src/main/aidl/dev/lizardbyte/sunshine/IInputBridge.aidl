// IInputBridge.aidl
// Interface exposed by the Shizuku UserService (running as shell) to the host app. The UserService
// holds android.permission.INJECT_EVENTS (granted to the shell user) and synthesizes the actual
// MotionEvent/KeyEvent objects — so every method executes in the shell process.
package dev.lizardbyte.sunshine;

interface IInputBridge {
    /**
     * Open and configure a uinput virtual mouse device (as the shell user). Returns an open fd
     * that the app can pass directly to the native write() calls, giving the cursor visibility.
     * Returns null when /dev/uinput is inaccessible.
     */
    ParcelFileDescriptor openUinputMouse();

    /**
     * Open and configure a uinput virtual keyboard device (as the shell user).
     * Returns null when /dev/uinput is inaccessible.
     */
    ParcelFileDescriptor openUinputKeyboard();
}
