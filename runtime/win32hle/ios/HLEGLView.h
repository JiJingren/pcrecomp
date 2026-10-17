/*
 * HLEGLView.h - the picture and the touches, on the main thread.
 *
 * screen_ios.c has already turned the game's primary surface into an RGBA8888
 * buffer and published it (see screen_ios.h); this view is the EAGL layer that
 * uploads that buffer, and the responder that turns fingers into the window
 * messages a DirectDraw game understands. The guest itself never comes near
 * OpenGL or UIKit: it runs on a worker pthread and reaches the screen through
 * screen_ios.c alone.
 */
#import <UIKit/UIKit.h>

@class CADisplayLink;

@interface HLEGLView : UIView

/* Creates the GL context, the framebuffer and the display link, and starts the
 * link running on the main run loop. Returns nil if there is no GLES 2.0, in
 * which case the view is otherwise inert. */
- (CADisplayLink *)startPresenter;

/* Draws one frame now, without waiting for the link. For a host that drives its
 * own pacing. */
- (void)render;

/* A short label over the picture, for the title at launch and for the scaling
 * mode when a three-finger tap changes it. Fades out by itself. */
- (void)showHud:(NSString *)text;

/* Pushes one key, given its USB HID usage, the way a keyboard would. Wired to
 * nothing here: iOS 6 has no public key input (see ios/README.md), but a host
 * built against a fuller SDK can call this. */
- (void)pressHidUsage:(int)usage down:(BOOL)down;

@end
