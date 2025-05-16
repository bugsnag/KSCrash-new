//
//  KSCrashMonitorContextHelper.m
//  KSCrash
//
//  Created by Daria Bialobrzeska on 16/05/2025.
//

#import "KSLogger.h"
#import <mach/mach.h>

#if __has_include(<UIKit/UIKit.h>)
#import <UIKit/UIKit.h>
#define UIAPPLICATION   NSClassFromString(@"UIApplication")
#endif

#if __has_include(<WatchKit/WatchKit.h>)
#import <WatchKit/WatchKit.h>
#endif

// Daemons and other processes running in non-UI sessions should not link against AppKit.
// These macros exist to allow the use of AppKit without adding a link-time dependency on it.

// Calling code should be prepared for classes to not be found when AppKit is not linked.
#if __has_include(<AppKit/AppKit.h>)
#import <AppKit/AppKit.h>
#define NSAPPLICATION   NSClassFromString(@"NSApplication")
#endif


static bool isRunningInAppExtension(void)
{
    // From "Information Property List Key Reference" > "App Extension Keys"
    // https://developer.apple.com/library/archive/documentation/General/Reference/InfoPlistKeyReference/Articles/AppExtensionKeys.html
    //
    // NSExtensionPointIdentifier
    // String - iOS, macOS. Specifies the extension point that supports an app extension, in reverse-DNS notation.
    // This key is required for every app extension, and must be placed as an immediate child of the NSExtension key.
    // Each Xcode app extension template is preconfigured with the appropriate extension point identifier key.
    return NSBundle.mainBundle.infoDictionary[@"NSExtension"][@"NSExtensionPointIdentifier"] != nil;
}

#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_VISION

static UIApplication * getUIApplication(void) {
    // +sharedApplication is unavailable to app extensions
    if (isRunningInAppExtension()) {
        return nil;
    }
    // Using performSelector: to avoid a compile-time check that
    // +sharedApplication is not called from app extensions
    return [UIAPPLICATION performSelector:@selector(sharedApplication)];
}
#endif

static bool getIsForeground(void) {
#if TARGET_OS_OSX
    return [[NSAPPLICATION sharedApplication] isActive];
#endif

#if TARGET_OS_IOS
    //
    // Work around unreliability of -[UIApplication applicationState] which
    // always returns UIApplicationStateBackground during the launch of UIScene
    // based apps (until the first scene has been created.)
    //
    task_category_policy_data_t policy;
    mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
    boolean_t get_default = FALSE;
    // task_policy_get() is prohibited on tvOS and watchOS
    kern_return_t kr = task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY,
                                       (void *)&policy, &count, &get_default);
    if (kr == KERN_SUCCESS) {
        // TASK_FOREGROUND_APPLICATION  -> normal foreground launch
        // TASK_NONUI_APPLICATION       -> background launch
        // TASK_DARWINBG_APPLICATION    -> iOS 15 prewarming launch
        // TASK_UNSPECIFIED             -> iOS 9 Simulator
        if (!get_default && policy.role == TASK_FOREGROUND_APPLICATION) {
            return true;
        }
    } else {
        KSLOG_ERROR(@"task_policy_get failed: %s", mach_error_string(kr));
    }
#endif

#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_VISION
    UIApplication *application = getUIApplication();

    // There will be no UIApplication if UIApplicationMain() has not yet been
    // called - e.g. from a SwiftUI app's init() function or UIKit app's main()
    if (!application) {
        return false;
    }

    __block UIApplicationState applicationState;
    if ([[NSThread currentThread] isMainThread]) {
        applicationState = [application applicationState];
    } else {
        // -[UIApplication applicationState] is a main thread-only API
        dispatch_sync(dispatch_get_main_queue(), ^{
            applicationState = [application applicationState];
        });
    }

    return applicationState != UIApplicationStateBackground;
#endif

#if TARGET_OS_WATCH
    if (isRunningInAppExtension()) {
        WKExtension *ext = [WKExtension sharedExtension];
        return ext && ext.applicationState != WKApplicationStateBackground;
    } else if (@available(watchOS 7.0, *)) {
        WKApplication *app = [WKApplication sharedApplication];
        return app && app.applicationState == WKApplicationStateBackground;
    } else {
        return true;
    }
#endif

#if TARGET_OS_VISION
    return true;
#endif
}

bool ksmc_isInForeground(void)
{
    return getIsForeground();
}
