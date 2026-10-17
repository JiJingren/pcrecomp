/*
 * HLEAppDelegate.h - the window, and the thread the game runs on.
 *
 * iOS will not let the guest own the main run loop: touch events and the
 * drawable EAGL context only exist there. So the guest is started here, on its
 * own pthread, once the view is up and hle_screen_open's wait can end.
 */
#import <UIKit/UIKit.h>

@interface HLEAppDelegate : UIResponder <UIApplicationDelegate>
@end
