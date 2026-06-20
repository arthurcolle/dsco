#ifndef DSCO_TOUCHID_H
#define DSCO_TOUCHID_H

/* ── Touch ID / Local Authentication ──────────────────────────────────────
 *
 * Thin C wrapper around macOS LocalAuthentication.framework.
 * The reply callback is invoked on an arbitrary background thread —
 * callers must be prepared to signal or wake the main input loop.
 *
 * Usage:
 *   if (touchid_available()) {
 *       touchid_authenticate("Unlock dsco", my_cb, ctx);
 *   }
 * ────────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>

/* Callback: success=true means biometry or device passcode accepted.
 * err_msg is NULL on success; a UTF-8 description on failure. */
typedef void (*touchid_cb_t)(bool success, const char *err_msg, void *ctx);

/* True if Touch ID (or Face ID / Watch) is enrolled and usable. */
bool touchid_available(void);

/* Begin an asynchronous authentication request.  Returns immediately;
 * cb is called on a background thread when the user responds. */
void touchid_authenticate(const char *reason, touchid_cb_t cb, void *ctx);

/* Block the calling thread until auth completes; returns true on success.
 * Convenience wrapper for use in the lock overlay input loop. */
bool touchid_authenticate_sync(const char *reason);

#endif /* DSCO_TOUCHID_H */
