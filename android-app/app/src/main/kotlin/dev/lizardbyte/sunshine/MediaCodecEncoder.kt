package dev.lizardbyte.sunshine

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import kotlin.concurrent.thread

/**
 * Hardware H.264/HEVC encoder built on MediaCodec with a Surface input. The capture VirtualDisplay
 * renders directly into [inputSurface], so frames are composited straight into the encoder on the
 * GPU with no CPU readback. Encoded access units are delivered to [onEncoded] from a dedicated
 * output thread.
 *
 * @param width Encoded width in pixels.
 * @param height Encoded height in pixels.
 * @param codec Codec index (0 = H.264, 1 = HEVC; anything else falls back to H.264).
 * @param fps Target frame rate.
 * @param bitrateBps Target bitrate in bits per second.
 * @param onEncoded Receives (directBuffer, size, flags, ptsUs) for each access unit. Flags: bit 0 =
 *   keyframe, bit 1 = codec config. The buffer is only valid for the duration of the call.
 */
class MediaCodecEncoder(
    width: Int,
    height: Int,
    codec: Int,
    fps: Int,
    bitrateBps: Int,
    private val onEncoded: (ByteBuffer, Int, Int, Long) -> Unit
) {
    val inputSurface: Surface
    private val codecObj: MediaCodec
    private var outputThread: Thread? = null
    @Volatile
    private var running = false
    private var scratch: ByteBuffer = ByteBuffer.allocateDirect(1 shl 20)

    init {
        val mime = when (codec) {
            1 -> MediaFormat.MIMETYPE_VIDEO_HEVC
            else -> MediaFormat.MIMETYPE_VIDEO_AVC
        }
        val format = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            // Effectively-infinite GOP: Sunshine drives IDRs on demand via requestKeyframe(), so we
            // don't want the codec inserting its own periodic keyframes.
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 3600)
            // Keep the stream flowing on static content by repeating the last frame at ~frame cadence.
            setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 1_000_000L / fps)
            if (Build.VERSION.SDK_INT >= 30) {
                // Carry SPS/PPS(/VPS) on every keyframe so the native side needn't track codec config.
                setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
        }

        codecObj = MediaCodec.createEncoderByType(mime)
        codecObj.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        inputSurface = codecObj.createInputSurface()
        codecObj.start()

        running = true
        outputThread = thread(name = "sunshine-encoder", isDaemon = true) { drainLoop() }
        Log.i(TAG, "MediaCodec started: $mime ${width}x$height @ ${fps}fps ${bitrateBps / 1000}kbps")
    }

    /** Pull encoded access units and forward them until stopped. */
    private fun drainLoop() {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val index = try {
                codecObj.dequeueOutputBuffer(info, 100_000)
            } catch (e: IllegalStateException) {
                break
            }
            if (index < 0) {
                continue  // INFO_TRY_AGAIN_LATER / format change; nothing to emit
            }
            try {
                val out = codecObj.getOutputBuffer(index)
                if (out != null && info.size > 0) {
                    out.position(info.offset)
                    out.limit(info.offset + info.size)
                    if (scratch.capacity() < info.size) {
                        scratch = ByteBuffer.allocateDirect(info.size)
                    }
                    scratch.clear()
                    scratch.put(out)
                    var flags = 0
                    if (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0) flags = flags or 0x1
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) flags = flags or 0x2
                    onEncoded(scratch, info.size, flags, info.presentationTimeUs)
                }
            } catch (e: Exception) {
                Log.e(TAG, "output drain failed", e)
            } finally {
                try {
                    codecObj.releaseOutputBuffer(index, false)
                } catch (_: Exception) {
                }
            }
        }
    }

    /** Request that the next encoded frame be an IDR/sync frame. */
    fun requestKeyframe() {
        try {
            codecObj.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            })
        } catch (e: Exception) {
            Log.e(TAG, "requestKeyframe failed", e)
        }
    }

    /** Update the target bitrate (bits per second). */
    fun setBitrate(bps: Int) {
        try {
            codecObj.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps)
            })
        } catch (e: Exception) {
            Log.e(TAG, "setBitrate failed", e)
        }
    }

    /** Stop the encoder and release all resources. */
    fun stop() {
        running = false
        outputThread?.join(500)
        outputThread = null
        try {
            codecObj.stop()
        } catch (_: Exception) {
        }
        try {
            codecObj.release()
        } catch (_: Exception) {
        }
        inputSurface.release()
        Log.i(TAG, "MediaCodec stopped")
    }

    companion object {
        private const val TAG = "SunshineEncoder"
    }
}
