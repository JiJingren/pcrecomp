#import "HLEViewController.h"

@implementation HLEViewController

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)o {
    return o == UIInterfaceOrientationLandscapeLeft || o == UIInterfaceOrientationLandscapeRight;
}

- (NSUInteger)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (BOOL)prefersStatusBarHidden { return YES; }

@end
