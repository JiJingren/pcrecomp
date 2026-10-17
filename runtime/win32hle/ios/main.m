/*
 * main.m - the entry point, and nothing else.
 *
 * The app delegate starts the guest on its own thread and owns the window; see
 * HLEAppDelegate.h. The drawing and the touch handling are in HLEGLView.h.
 */
#import <UIKit/UIKit.h>

#import "HLEAppDelegate.h"

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([HLEAppDelegate class]));
    }
}
