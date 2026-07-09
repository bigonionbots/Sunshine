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
}
