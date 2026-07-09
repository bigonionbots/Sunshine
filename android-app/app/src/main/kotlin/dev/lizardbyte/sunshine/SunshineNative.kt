package dev.lizardbyte.sunshine

/**
 * Bridge to the native Sunshine core (libsunshine.so).
 */
object SunshineNative {
    init {
        System.loadLibrary("sunshine")
    }

    /**
     * Boot the Sunshine core. Blocks until shutdown, so call on a dedicated thread.
     *
     * @param dataDir App-private data directory holding config, state, and assets.
     * @return Sunshine's exit code.
     */
    external fun nativeStart(dataDir: String): Int

    /**
     * Request a graceful shutdown, unblocking [nativeStart].
     */
    external fun nativeStop()

    /**
     * Notify the core that MediaProjection capture has started at the given size.
     */
    external fun nativeCaptureStarted(width: Int, height: Int)

    /**
     * Notify the core that MediaProjection capture has stopped.
     */
    external fun nativeCaptureStopped()

    /**
     * Deliver one captured RGBA_8888 frame. [buffer] must be a direct ByteBuffer.
     */
    external fun nativePushFrame(buffer: java.nio.ByteBuffer, width: Int, height: Int, rowStride: Int)

    /**
     * Notify the core that AudioPlaybackCapture has started at the given format.
     */
    external fun nativeAudioStarted(sampleRate: Int, channels: Int)

    /**
     * Notify the core that AudioPlaybackCapture has stopped.
     */
    external fun nativeAudioStopped()

    /**
     * Deliver interleaved float PCM captured from playback. [buffer] must be a direct ByteBuffer;
     * [count] is the number of float samples it holds (frames * channels).
     */
    external fun nativePushAudio(buffer: java.nio.ByteBuffer, count: Int)

    /**
     * Register (or clear with null) the IInputBridge AIDL object from the Shizuku UserService.
     * When set, the native input backend routes keyboard/mouse events through it instead of uinput.
     */
    external fun nativeSetInputBridge(bridge: Any?)

    /**
     * Create and configure a virtual uinput mouse device. Called from the Shizuku UserService
     * (running as shell) which has permission to open /dev/uinput. Returns the open file
     * descriptor on success or -1 on failure.
     */
    external fun nativeCreateUinputMouse(): Int

    /**
     * Create and configure a virtual uinput keyboard device. Same context as nativeCreateUinputMouse.
     * Returns the open file descriptor on success or -1 on failure.
     */
    external fun nativeCreateUinputKeyboard(): Int

    /**
     * Hand pre-opened uinput file descriptors to the native input backend. Called from the app
     * after receiving the fds from the Shizuku UserService over Binder. Passing -1 for either
     * leaves that device unset.
     */
    external fun nativeSetUinputFds(mouseFd: Int, keyboardFd: Int)

    /**
     * Register (or clear with null) the object that services MediaCodec hardware encode. It must
     * expose startEncoder(Int,Int,Int,Int,Int):Boolean, stopEncoder(), requestKeyframe(), and
     * setBitrate(Int); the native encoder session calls these.
     */
    external fun nativeSetVideoBridge(bridge: Any?)

    /**
     * Deliver one encoded access unit from MediaCodec. [buffer] must be a direct ByteBuffer holding
     * [size] bytes. [flags] bit 0 = keyframe, bit 1 = codec config. [ptsUs] is the presentation
     * timestamp in microseconds.
     */
    external fun nativePushEncoded(buffer: java.nio.ByteBuffer, size: Int, flags: Int, ptsUs: Long)
}
