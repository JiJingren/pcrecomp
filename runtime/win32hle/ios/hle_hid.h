/*
 * hle_hid.h - USB HID usage codes as Win32 keys.
 *
 * A keyboard arrives at an app as a stream of usage codes (UIKit calls them
 * UIKeyboardHIDUsage; the values are the USB HID Keyboard/Keypad page). The
 * shims want the PC pair instead, because that is what a Win32 key message
 * carries: the virtual key for the message and the PC/AT set-1 scan code that
 * goes in lParam. The scan codes are the same ones screen.c's table already
 * lists for the desktop build.
 *
 * Plain C, no UIKit: the usage codes are just numbers, and a host that does
 * have presses to report (see ios/README.md on why the iOS 6 SDK has none) can
 * use this without the rest of the port.
 */
#ifndef HLE_HID_H
#define HLE_HID_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fills vk and scan from a HID usage. Returns 0, and leaves both at 0, for a
 * usage with no PC equivalent (a modifier Apple added, say). */
int hle_hid_to_win(int hid, int *vk, int *scan);

#ifdef __cplusplus
}
#endif

#endif /* HLE_HID_H */
