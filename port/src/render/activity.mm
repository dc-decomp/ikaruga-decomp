#include "render/activity.h"

#import <Foundation/Foundation.h>

namespace render {

void keep_running(bool unattended) {
    // App Nap slows the timers and lowers the priority of an app whose windows cannot be seen. An
    // activity the user started is exempt, so a run keeps its speed behind other windows. Only an
    // unattended run also keeps the Mac from sleeping when idle.
    const NSActivityOptions options =
        (unattended ? NSActivityUserInitiated : NSActivityUserInitiatedAllowingIdleSystemSleep) |
        NSActivityLatencyCritical;
    static id<NSObject> activity =
        [[NSProcessInfo processInfo] beginActivityWithOptions:options reason:@"running the game"];
    (void)activity;
}

}  // namespace render
