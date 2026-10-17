/*
 * HLEAppDelegate.m - the window, and the thread the game runs on.
 *
 * The order matters and is short: the window and the GL view go up first, the
 * view is announced (which releases the wait inside the guest's
 * hle_screen_open), then the guest is started on a worker pthread, then the
 * display link is. Starting the guest before the view exists would only make it
 * wait; starting the link last means the first frames it draws are already the
 * guest's.
 */
#import "HLEAppDelegate.h"

#import <QuartzCore/CADisplayLink.h>
#import <pthread.h>
#include <stdint.h>

#import "HLEGLView.h"
#import "HLEViewController.h"
#include "screen_ios.h"

/* Whoever links this supplies the guest: the smoke app now, redalert2-recomp's
 * own host (src/linux/main.c, renamed) once there is a lift to run. It runs to
 * completion on its own thread and ends by calling hle_exit(). */
extern int hle_ios_run_guest(void);

static void *guest_thread(void *arg) {
    (void)arg;
    return (void *)(intptr_t)hle_ios_run_guest();
}

@implementation HLEAppDelegate {
    UIWindow *_window;
    CADisplayLink *_link;
    HLEGLView *_view;
}

- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    (void)app; (void)opts;
    _window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    HLEViewController *vc = [[HLEViewController alloc] init];

    _view = [[HLEGLView alloc] initWithFrame:_window.bounds];
    _view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _view.multipleTouchEnabled = YES;
    _view.opaque = YES;
    _view.backgroundColor = [UIColor blackColor];
    vc.view = _view;
    _window.rootViewController = vc;
    [_window makeKeyAndVisible];

    /* The view exists, so the guest's wait can end, and it has a size to map
     * touches into even before the game picks one. */
    hle_ios_view_created();
    hle_ios_view_size((int)lround(_view.bounds.size.width), (int)lround(_view.bounds.size.height));

    pthread_t t;
    if (pthread_create(&t, NULL, guest_thread, NULL) == 0) pthread_detach(t);
    else fprintf(stderr, "[ios] no thread for the guest\n");

    _link = [_view startPresenter];
    if (!_link) {
        [_view showHud:@"no OpenGL ES 2.0"];
        return YES;
    }
    const char *title = hle_ios_title();
    [_view showHud:[NSString stringWithFormat:@"%s", (title && *title) ? title : "win32hle"]];
    return YES;
}

/* Nothing here can be suspended and resumed - there is no state to save and no
 * time to skip - so going home closes the game, the way closing the window would
 * have on the desktop. */
- (void)applicationDidEnterBackground:(UIApplication *)app {
    (void)app;
    hle_ios_want_exit();
}

- (void)applicationWillTerminate:(UIApplication *)app {
    (void)app;
    hle_ios_want_exit();
}

@end
