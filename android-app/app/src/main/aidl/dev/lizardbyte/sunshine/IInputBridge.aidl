// IInputBridge.aidl
// Interface exposed by the Shizuku UserService (running as shell) to the host app. The UserService
// holds android.permission.INJECT_EVENTS (granted to the shell user) and synthesizes the actual
// MotionEvent/KeyEvent objects — so every method executes in the shell process.
package dev.lizardbyte.sunshine;

interface IInputBridge {
    /** Key press/release. vkCode is the Windows virtual-key code Moonlight sends. */
    void injectKey(int vkCode, boolean pressed);

    /** Relative mouse movement in raw screen pixels. */
    void injectMouseMove(float dx, float dy);

    /**
     * Mouse button press/release.
     * button: 1=left  2=right  4=middle  8=X1  16=X2.
     */
    void injectMouseButton(int button, boolean pressed);

    /** Vertical scroll, high-resolution 1/120-wheel-notch units (positive = up). */
    void injectScroll(int distance);

    /** Horizontal scroll, same units as injectScroll. */
    void injectHScroll(int distance);

    /** Notify the UserService that a gamepad connected on the given slot (0–3). */
    void allocGamepad(int slot);

    /** Notify the UserService that a gamepad disconnected from the given slot. */
    void freeGamepad(int slot);

    /**
     * Deliver a full gamepad state snapshot.
     * buttons: Moonlight button bitmask (DPAD_UP=1, DPAD_DOWN=2, DPAD_LEFT=4, DPAD_RIGHT=8,
     *          START=0x10, BACK=0x20, LS=0x40, RS=0x80, LB=0x100, RB=0x200, A=0x1000,
     *          B=0x2000, X=0x4000, Y=0x8000).
     * lsX/lsY/rsX/rsY: stick axes, -1.0..1.0 (Y+ = down).
     * lt/rt: trigger values, 0.0..1.0.
     */
    void injectGamepadState(int slot, int buttons, float lsX, float lsY, float rsX, float rsY, float lt, float rt);

    /**
     * Run /system/bin/screencap (which requires shell or root) and write its raw binary output to
     * the write end of a pipe. The caller must read from the read end concurrently (before calling
     * this) to avoid blocking when the frame exceeds the pipe's 64 KB kernel buffer. The fd is
     * closed before this method returns so the reader sees EOF.
     */
    void screencapToFd(in ParcelFileDescriptor fd);

    /**
     * Capture the display via SurfaceControl reflection — no subprocess per frame.
     * On success writes {width:int, height:int} (8 bytes, little-endian) followed by
     * BGRA_8888 pixels (width × height × 4 bytes) to [fd]. On failure (reflection not
     * available on this device) writes nothing and closes [fd] immediately; the caller
     * should detect the empty read and fall back to startScreencapDaemon().
     * width/height: requested output resolution (compositor scales on the GPU).
     */
    void screencapToFdBGRA(in ParcelFileDescriptor fd, int width, int height);

    /**
     * Start a persistent native screencap daemon that loops /system/bin/screencap and streams
     * raw RGBA frames over [fd].  Each frame is prefixed by an 8-byte little-endian header
     * {width:int32, height:int32} followed by width*height*4 RGBA_8888 bytes.  The daemon runs
     * until stopScreencapDaemon() is called or [fd] is closed (EPIPE causes exit).
     * The caller must close its own copy of [fd] after this call (UserService takes ownership).
     */
    void startScreencapDaemon(in ParcelFileDescriptor fd);

    /** Stop the screencap daemon started by startScreencapDaemon(). Idempotent. */
    void stopScreencapDaemon();
}
