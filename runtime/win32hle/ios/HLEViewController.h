/*
 * HLEViewController.h - a view controller that only exists to hold the
 * orientation policy: the game's picture is 4:3 and sideways, so the app never
 * rotates to portrait and never shows a status bar.
 */
#import <UIKit/UIKit.h>

@interface HLEViewController : UIViewController
@end
