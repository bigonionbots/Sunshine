package dev.lizardbyte.sunshine

import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.FileDescriptor
import java.io.IOException

/**
 * Shizuku UserService running as shell (uid 2000). The shell user can open /dev/uinput and issue
 * the ioctls to create virtual input devices, which registers real pointer devices with Android's
 * EventHub — making the mouse cursor visible. The configured fd is returned to the app over
 * Binder; the app's native code then writes evdev events directly.
 *
 * The uinput setup (ioctl + write) is performed via the thin native helper in libsunshine.so,
 * which is already loaded by the host app's class loader and accessible from the UserService
 * because Shizuku loads the service from the same APK.
 */
class InputUserService : IInputBridge.Stub() {

    override fun openUinputMouse(): ParcelFileDescriptor? {
        return try {
            val fd = SunshineNative.nativeCreateUinputMouse()
            if (fd < 0) {
                Log.e(TAG, "nativeCreateUinputMouse returned $fd")
                null
            } else {
                ParcelFileDescriptor.adoptFd(fd)
            }
        } catch (e: Exception) {
            Log.e(TAG, "openUinputMouse failed", e)
            null
        }
    }

    override fun openUinputKeyboard(): ParcelFileDescriptor? {
        return try {
            val fd = SunshineNative.nativeCreateUinputKeyboard()
            if (fd < 0) {
                Log.e(TAG, "nativeCreateUinputKeyboard returned $fd")
                null
            } else {
                ParcelFileDescriptor.adoptFd(fd)
            }
        } catch (e: Exception) {
            Log.e(TAG, "openUinputKeyboard failed", e)
            null
        }
    }

    companion object {
        private const val TAG = "InputUserService"
    }
}
