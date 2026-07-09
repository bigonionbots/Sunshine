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
}
