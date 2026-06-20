/* Objective-C — compiled with clang -fobjc-arc */
#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#include "touchid.h"
#include "audit_log.h"
#include <dispatch/dispatch.h>

bool touchid_available(void) {
    LAContext *ctx = [[LAContext alloc] init];
    NSError *err = nil;
    BOOL ok = [ctx canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics
                               error:&err];
    if (!ok) {
        const char *msg = err ? [[err localizedDescription] UTF8String] : "unknown";
        char buf[256];
        snprintf(buf, sizeof(buf), "Touch ID unavailable: %s", msg);
        audit_log("touchid", buf);
    }
    return (bool)ok;
}

void touchid_authenticate(const char *reason, touchid_cb_t cb, void *ctx) {
    LAContext *la = [[LAContext alloc] init];

    NSString *r = reason
        ? [NSString stringWithUTF8String:reason]
        : @"Unlock dsco";

    /* DeviceOwnerAuthentication = biometrics with device-passcode fallback.
     * Never biometrics-only here: if Touch ID is locked out, the sensor is
     * busy, or the lid is closed, a biometrics-only policy leaves the user
     * with no way to dismiss the lock overlay and wedges the whole TUI. The
     * passcode fallback guarantees an authentication path always exists. */
    [la evaluatePolicy:LAPolicyDeviceOwnerAuthentication
       localizedReason:r
                 reply:^(BOOL success, NSError *err) {
        const char *msg = nil;
        char errbuf[256] = {0};
        if (!success && err) {
            const char *desc = [[err localizedDescription] UTF8String];
            snprintf(errbuf, sizeof(errbuf), "%s", desc ? desc : "auth failed");
            msg = errbuf;
            audit_log("touchid", errbuf);
        } else if (success) {
            audit_log("touchid", "authenticated");
        }
        if (cb) cb((bool)success, msg, ctx);
    }];
}

/* Sync wrapper — blocks calling thread via semaphore; calls LAContext directly */
bool touchid_authenticate_sync(const char *reason) {
    __block bool result = false;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    LAContext *la = [[LAContext alloc] init];
    la.localizedFallbackTitle = @"";
    NSString *r = reason
        ? [NSString stringWithUTF8String:reason]
        : @"Unlock dsco";

    [la evaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics
       localizedReason:r
                 reply:^(BOOL ok, NSError *err) {
        (void)err;
        result = (bool)ok;
        dispatch_semaphore_signal(sem);
    }];

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    return result;
}

#else  /* !__APPLE__ */

#include "touchid.h"

bool touchid_available(void) { return false; }
void touchid_authenticate(const char *r, touchid_cb_t cb, void *ctx)
     { (void)r; if (cb) cb(false, "not supported", ctx); }
bool touchid_authenticate_sync(const char *r) { (void)r; return false; }

#endif
