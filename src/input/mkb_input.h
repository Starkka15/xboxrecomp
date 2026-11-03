/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * The goal: let a recompiled title be played like a native PC FPS --
 * raw mouse deltas drive the aim stick, WASD drives movement, mouse
 * buttons drive the triggers -- regardless of which path the title
 * takes to read its pad (the emulated USB device or the kernel's
 * XInput ordinals), because the merge happens in xbox_InputGetState,
 * which both go through.
 *
 * All knobs are env vars (and an optional mkb.ini), so no rebuild is
 * needed to tune the feel:
 *
 *   RECOMP_MKB=0|1        master switch            (default off, opt-in)
 *   RECOMP_MKB_SENS=f     mouse sensitivity        (default 1.0)
 *   RECOMP_MKB_FULL=n     mouse counts for a full  (default 8)
 *                         stick throw, per poll
 *   RECOMP_MKB_DZ=f       assumed game deadzone    (default 0.18)
 *   RECOMP_MKB_ACC=f      aim acceleration         (default 1.0 = off)
 *   RECOMP_MKB_INVERT_Y=1 invert vertical aim      (default off)
 *   RECOMP_MKB_INI=path   config file (same keys, one per line)
 *
 * F9 toggles mouse/keyboard capture at runtime. Capture is only
 * applied while the recompiled game's process has the foreground
 * window, so the mouse keeps working normally in other apps.
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the layer is enabled (RECOMP_MKB not "0"). Cheap; cached. */
int mkb_enabled(void);

/*
 * Merge the mouse+keyboard virtual pad on top of *pState, in place.
 * Buttons OR in, analog buttons take the larger value, and the aim
 * stick is taken over by the mouse whenever the mouse moved since the
 * last poll. Bumps dwPacketNumber when the virtual pad contributed
 * anything, so a title watching for edges sees the presses.
 *
 * Returns 1 when the virtual pad is active and produced a usable
 * state (so a caller treating "no host pad" as an error can still
 * report a connected controller), 0 otherwise.
 */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

/*
 * Bindings. Every control of the Xbox pad can be bound in mkb.ini to up
 * to four keys or mouse buttons, by the names below ("A = Space, Enter").
 * Unlisted controls keep the built-in layout; a listed one with nothing
 * after the '=' is unbound. The mouse itself drives the camera stick.
 */
enum {
    MKB_A, MKB_B, MKB_X, MKB_Y, MKB_BLACK, MKB_WHITE, MKB_LT, MKB_RT,
    MKB_START, MKB_BACK, MKB_LS, MKB_RS,
    MKB_DUP, MKB_DDOWN, MKB_DLEFT, MKB_DRIGHT,
    MKB_MOVE_UP, MKB_MOVE_DOWN, MKB_MOVE_LEFT, MKB_MOVE_RIGHT,
    MKB_CAM_UP, MKB_CAM_DOWN, MKB_CAM_LEFT, MKB_CAM_RIGHT,
    MKB_NCONTROLS
};

/* The display name of the first input bound to a control ("Space",
 * "Mouse1", "W"), or NULL when it has none. Loads the config if needed. */
const char *mkb_binding_label(int control);

/* Which device the player touched last, for on-screen button prompts:
 * 1 = keyboard or mouse, 0 = a controller. Starts as the keyboard when no
 * controller is plugged in. */
int mkb_last_device_keyboard(void);

/* Called by the controller backend when a real pad was used. */
void mkb_note_pad_activity(void);

/* Counts key and mouse-button presses, so a caller can tell whether the
 * player pressed anything since it last looked. */
unsigned mkb_activity(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */
