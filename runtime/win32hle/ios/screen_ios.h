/*
 * screen_ios.h - the seam between screen_ios.c and the UIKit layer.
 *
 * The thread split is the reason this file exists at all. The game runs on a
 * worker pthread and owns its own Win32 message loop: win32hle calls the pump
 * hook from inside GetMessage/PeekMessage when the queue is empty, and that is
 * where the desktop presenter presented a frame and read the keyboard. iOS
 * will not allow that: touch events and the drawable EAGL context only exist
 * on the main thread's run loop, so neither side can be moved there.
 *
 * So the work is split at the frame and at the event instead:
 *
 *   guest thread   screen_ios.c's hle_screen_pump() - converts the primary
 *                  surface to RGBA8888 (a memcpy-shaped pass over the game
 *                  picture), publishes it, and drains the input queue into
 *                  hle_input_mouse()/hle_input_key(). No GL, no UIKit.
 *   main thread    ios/main.m's CADisplayLink - uploads the published frame to
 *                  a GLES 2.0 texture and draws it letterboxed. UIKit hands it
 *                  touches and presses through hle_ios_input_push().
 *
 * That is one memcpy of the game picture per changed frame (800x600x4 is about
 * 1.9 MB, well under a millisecond on the hardware this targets) in exchange
 * for keeping the guest's own message loop and win32hle's shim ABI exactly as
 * they are on Linux, and for not needing an EAGL context shared across threads.
 */
#ifndef HLE_SCREEN_IOS_H
#define HLE_SCREEN_IOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What hle_ios_input_push's kind is. The mouse is the touch: one finger is
 * the left button, and a second finger put down mid-gesture is the right one,
 * which is what a DirectDraw game wants for its own context menus. A tap is a
 * press and a release at the same place, a drag is a press, moves and a
 * release; the two right kinds are what a second finger or a long press sends. */
enum {
    HLE_IOS_MOUSE_MOVE = 0,
    HLE_IOS_MOUSE_DOWN = 1,
    HLE_IOS_MOUSE_UP   = 2,
    HLE_IOS_KEY_DOWN   = 3,
    HLE_IOS_KEY_UP     = 4,
    HLE_IOS_MOUSE_RDOWN = 5,
    HLE_IOS_MOUSE_RUP   = 6
};

/* ---- main thread -> screen_ios.c ---- */

/* The EAGL view exists and can be drawn into; releases hle_screen_open(). */
void hle_ios_view_created(void);

/* The drawable changed size, in pixels (retina: real pixels, not points).
 * Recomputes where the picture goes and forces the next frame to be shown. */
void hle_ios_view_size(int w, int h);

/* A touch, a press, or (x, y) of a game mouse already turned into one of the
 * kinds above. x and y are points in the view; the pump maps them to game
 * pixels. Queued, never delivered on the calling thread. */
void hle_ios_input_push(int kind, int x, int y, int vk, int scan);

/* The guest closed its window: the app should leave. Posts WM_CLOSE to the
 * guest's window first, the way screen.c answered SDL_QUIT. */
void hle_ios_want_exit(void);

/* The scaling settings a touch build has no F11/F12 for. */
int         hle_ios_scale_cycle(void);             /* sharp -> ... -> integer */
int         hle_ios_scale_set(int mode);
int         hle_ios_bars_set(int blur);
const char *hle_ios_scale_name(void);
const char *hle_ios_title(void);

/* ---- screen_ios.c -> main thread ---- */

/* The newest published frame, or NULL if none yet. RGBA8888, w*h uint32_t in
 * native order (r<<24|g<<16|b<<8|a, so GL_RGBA + GL_UNSIGNED_BYTE reads it
 * straight). Valid until the next call; the caller wants to upload it at once. */
const uint32_t *hle_ios_frame_acquire(int *w, int *h);

/* How many frames the guest has published. A presenter that uploads the picture
 * only when this moves skips the copy of a picture the game did not redraw. */
unsigned hle_ios_frame_seq(void);

/* Where the picture goes in the view, in pixels: aspect kept, centred. The
 * same numbers the pump uses to turn a touch into a game coordinate. */
void hle_ios_dst_rect(int *x, int *y, int *w, int *h);

/* The scaling mode's two properties the drawer needs: hle_screen_scale's
 * choice, and whether to darken every other line (crt). */
int hle_ios_scale_mode(void);
int hle_ios_crt_on(void);

/* The cursor to draw over the picture, if any: RGBA8888 32x32 (or the width it
 * reports), with a<<24 - a 0 is opaque, a 255 transparent, which is what the
 * guest's AND mask means. NULL when the game draws its own. */
const uint32_t *hle_ios_cursor_acquire(int *w, int *h, int *hot_x, int *hot_y);

/* Where to put it: the guest's pointer, in game pixels, mapped into the view
 * with the same numbers hle_ios_dst_rect reports. A touch screen has no OS
 * cursor, so the one the game set is drawn over the picture here. */
int hle_ios_cursor_screen_pos(int *x, int *y);

#ifdef __cplusplus
}
#endif

#endif /* HLE_SCREEN_IOS_H */