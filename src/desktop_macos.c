#include "desktop_macos.h"
#include "json_util.h"
#include "../vendor/yyjson.h"

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__) && !defined(DSCO_DESKTOP_NO_MACOS)
#define DESKTOP_HAS_MACOS 1
#include <ApplicationServices/ApplicationServices.h>
#else
#define DESKTOP_HAS_MACOS 0
#endif

static pthread_mutex_t s_desktop_lock = PTHREAD_MUTEX_INITIALIZER;
void desktop_input_lock(void) { pthread_mutex_lock(&s_desktop_lock); }
void desktop_input_unlock(void) { pthread_mutex_unlock(&s_desktop_lock); }

typedef struct {
    const char *action;
    const char *permission;
    uint32_t window_id;
    int pid, limit, max_depth, max_nodes;
    bool include_offscreen, include_titles;
    double x, y, width, height;
} desktop_request_t;

static bool finish(jbuf_t *b, char *out, size_t cap, bool ok) {
    if (!out || cap == 0) { jbuf_free(b); return false; }
    if (!b->data || b->len >= cap) {
        const char *error = "{\"ok\":false,\"error\":\"result_buffer_too_small\"}";
        if (strlen(error) < cap) memcpy(out, error, strlen(error) + 1);
        else out[0] = '\0';
        jbuf_free(b);
        return false;
    }
    memcpy(out, b->data, b->len + 1);
    jbuf_free(b);
    return ok;
}

static bool error_result(char *out, size_t cap, const char *code, const char *detail) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":false,\"error\":");
    jbuf_append_json_str(&b, code);
    if (detail) {
        jbuf_append(&b, ",\"detail\":");
        jbuf_append_json_str(&b, detail);
    }
    jbuf_append(&b, "}");
    return finish(&b, out, cap, false);
}

static bool known_action(const char *action) {
    static const char *const names[] = {
        "status", "list", "inspect", "snapshot", "focus", "move", "resize",
        "set_bounds", "request_permission", NULL
    };
    for (int i = 0; names[i]; ++i)
        if (!strcmp(action, names[i])) return true;
    return false;
}

static bool number(yyjson_val *obj, const char *key, bool required, bool integral,
                   double lo, double hi, double fallback, double *out) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!v) { *out = fallback; return !required; }
    if (!yyjson_is_num(v) || (integral && !yyjson_is_int(v))) return false;
    double n = yyjson_get_num(v);
    if (!isfinite(n) || n < lo || n > hi) return false;
    *out = n;
    return true;
}

/* Validate the complete request before reaching any desktop APIs. Unknown and
 * duplicate fields are rejected, so a typo cannot silently choose a target. */
static bool parse_request(yyjson_val *obj, desktop_request_t *r, char *why, size_t cap) {
    if (!yyjson_is_obj(obj)) { snprintf(why, cap, "request must be a JSON object"); return false; }
    static const char *const keys[] = {
        "action", "permission", "window_id", "pid", "x", "y", "width", "height",
        "limit", "include_offscreen", "include_titles", "max_depth", "max_nodes", NULL
    };
    unsigned seen = 0;
    size_t idx, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(obj, idx, max, key, value) {
        (void)value;
        int which = -1;
        for (int i = 0; keys[i]; ++i)
            if (yyjson_equals_str(key, keys[i])) { which = i; break; }
        if (which < 0 || (seen & (1u << which))) {
            snprintf(why, cap, "unknown or duplicate field"); return false;
        }
        seen |= 1u << which;
    }
    yyjson_val *av = yyjson_obj_get(obj, "action");
    r->action = yyjson_get_str(av);
    if (!r->action || strlen(r->action) != yyjson_get_len(av) || !known_action(r->action)) {
        snprintf(why, cap, "action is missing or unsupported"); return false;
    }
    bool target = strcmp(r->action, "status") && strcmp(r->action, "list") &&
                  strcmp(r->action, "request_permission");
    bool move = !strcmp(r->action, "move") || !strcmp(r->action, "set_bounds");
    bool resize = !strcmp(r->action, "resize") || !strcmp(r->action, "set_bounds");
    double n;
#define NUM(field, required, integral, low, high, fallback, dest) do { \
    if (!number(obj, field, required, integral, low, high, fallback, &n)) { \
        snprintf(why, cap, "invalid or missing %s", field); return false; \
    } \
    dest = n; \
} while (0)
    NUM("window_id", target, true, 1, UINT32_MAX, 0, r->window_id);
    NUM("pid", target, true, 1, INT_MAX, 0, r->pid);
    NUM("limit", false, true, 1, 256, 100, r->limit);
    NUM("max_depth", false, true, 0, 8, 3, r->max_depth);
    NUM("max_nodes", false, true, 1, 256, 80, r->max_nodes);
    NUM("x", move, false, -100000, 100000, 0, r->x);
    NUM("y", move, false, -100000, 100000, 0, r->y);
    NUM("width", resize, false, 1, 100000, 0, r->width);
    NUM("height", resize, false, 1, 100000, 0, r->height);
#undef NUM
    yyjson_val *offscreen = yyjson_obj_get(obj, "include_offscreen");
    yyjson_val *titles = yyjson_obj_get(obj, "include_titles");
    if ((offscreen && !yyjson_is_bool(offscreen)) || (titles && !yyjson_is_bool(titles))) {
        snprintf(why, cap, "include_offscreen and include_titles must be booleans"); return false;
    }
    r->include_offscreen = yyjson_get_bool(offscreen);
    r->include_titles = yyjson_get_bool(titles);
    yyjson_val *pv = yyjson_obj_get(obj, "permission");
    r->permission = yyjson_get_str(pv);
    if (pv && (!r->permission || strlen(r->permission) != yyjson_get_len(pv) ||
               (strcmp(r->permission, "accessibility") && strcmp(r->permission, "screen_recording")))) {
        snprintf(why, cap, "permission must be accessibility or screen_recording"); return false;
    }
    if (!strcmp(r->action, "request_permission") && !r->permission) {
        snprintf(why, cap, "request_permission requires permission"); return false;
    }
    return true;
}

#if DESKTOP_HAS_MACOS

typedef struct {
    CGWindowID id;
    pid_t pid;
    int layer;
    CGRect bounds;
    CFStringRef title;
} desktop_window_t;

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void append_rect(jbuf_t *b, CGRect r) {
    jbuf_appendf(b, "{\"x\":%.3f,\"y\":%.3f,\"width\":%.3f,\"height\":%.3f}",
                 r.origin.x, r.origin.y, r.size.width, r.size.height);
}

static void append_cf_string(jbuf_t *b, CFStringRef s) {
    char text[2049] = "";
    if (s && CFGetTypeID(s) == CFStringGetTypeID()) {
        CFIndex units = CFStringGetLength(s);
        if (units > 512) units = 512;
        CFIndex bytes = 0;
        CFStringGetBytes(s, CFRangeMake(0, units), kCFStringEncodingUTF8, '?', false,
                         (UInt8 *)text, sizeof(text) - 1, &bytes);
        text[bytes] = '\0';
    }
    jbuf_append_json_str(b, text);
}

static bool dictionary_number(CFDictionaryRef d, CFStringRef key, int64_t *out) {
    CFTypeRef v = CFDictionaryGetValue(d, key);
    return v && CFGetTypeID(v) == CFNumberGetTypeID() &&
           CFNumberGetValue(v, kCFNumberSInt64Type, out);
}

static bool window_from_dictionary(CFDictionaryRef d, desktop_window_t *w) {
    int64_t id, pid, layer;
    if (!dictionary_number(d, kCGWindowNumber, &id) || id <= 0 || id > UINT32_MAX ||
        !dictionary_number(d, kCGWindowOwnerPID, &pid) || pid <= 0 || pid > INT_MAX ||
        !dictionary_number(d, kCGWindowLayer, &layer) || layer < INT_MIN || layer > INT_MAX) return false;
    CFTypeRef bounds = CFDictionaryGetValue(d, kCGWindowBounds);
    if (!bounds || CFGetTypeID(bounds) != CFDictionaryGetTypeID() ||
        !CGRectMakeWithDictionaryRepresentation(bounds, &w->bounds)) return false;
    w->id = (CGWindowID)id;
    w->pid = (pid_t)pid;
    w->layer = (int)layer;
    CFTypeRef title = CFDictionaryGetValue(d, kCGWindowName);
    w->title = title && CFGetTypeID(title) == CFStringGetTypeID() ? title : NULL;
    return true;
}

static void append_window(jbuf_t *b, const desktop_window_t *w, bool titles) {
    jbuf_appendf(b, "{\"window_id\":%u,\"pid\":%d,\"layer\":%d,\"bounds\":", w->id, w->pid, w->layer);
    append_rect(b, w->bounds);
    if (titles) {
        jbuf_append(b, ",\"title\":");
        if (w->title) append_cf_string(b, w->title);
        else jbuf_append(b, "null");
    }
    jbuf_append(b, "}");
}

/* The returned array retains the dictionary backing w->title. */
static CFArrayRef exact_window(const desktop_request_t *r, desktop_window_t *w,
                               const char **error) CF_RETURNS_RETAINED;
static CFArrayRef exact_window(const desktop_request_t *r, desktop_window_t *w,
                               const char **error) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, r->window_id);
    if (!windows) { *error = "window_inventory_unavailable"; return NULL; }
    bool found = false;
    for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(windows, i);
        if (window_from_dictionary(d, w) && w->id == r->window_id) { found = true; break; }
    }
    if (!found || w->pid != r->pid) {
        *error = found ? "window_pid_mismatch" : "window_not_found";
        CFRelease(windows);
        return NULL;
    }
    return windows;
}

static bool ax_bounds(AXUIElementRef element, CGRect *out) {
    CFTypeRef pos = NULL, size = NULL;
    CGPoint p;
    CGSize s;
    bool ok = AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &pos) == kAXErrorSuccess &&
              AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size) == kAXErrorSuccess &&
              pos && size && CFGetTypeID(pos) == AXValueGetTypeID() &&
              CFGetTypeID(size) == AXValueGetTypeID() &&
              AXValueGetValue(pos, kAXValueCGPointType, &p) &&
              AXValueGetValue(size, kAXValueCGSizeType, &s);
    if (pos) CFRelease(pos);
    if (size) CFRelease(size);
    if (ok) *out = (CGRect){p, s};
    return ok;
}

static bool same_rect(CGRect a, CGRect b) {
    return fabs(a.origin.x - b.origin.x) <= 1.0 && fabs(a.origin.y - b.origin.y) <= 1.0 &&
           fabs(a.size.width - b.size.width) <= 1.0 && fabs(a.size.height - b.size.height) <= 1.0;
}

/* Public AX has no documented CGWindowID accessor. Resolve only a unique
 * same-process window with matching geometry and title (when CG supplies it).
 * Never guess between overlapping/indistinguishable AX windows. */
static AXUIElementRef resolve_ax(AXUIElementRef app, const desktop_window_t *w,
                                const char **error) CF_RETURNS_RETAINED;
static AXUIElementRef resolve_ax(AXUIElementRef app, const desktop_window_t *w,
                                const char **error) {
    CFTypeRef windows = NULL;
    if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &windows) != kAXErrorSuccess ||
        !windows || CFGetTypeID(windows) != CFArrayGetTypeID()) {
        if (windows) CFRelease(windows);
        *error = "accessibility_windows_unavailable";
        return NULL;
    }
    AXUIElementRef found = NULL;
    int count = 0;
    CFIndex n = CFArrayGetCount(windows);
    double deadline = monotonic_seconds() + 2.0;
    for (CFIndex i = 0; i < n && i < 256; ++i) {
        if (monotonic_seconds() >= deadline) { count = -1; break; }
        AXUIElementRef candidate = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);
        if (CFGetTypeID(candidate) != AXUIElementGetTypeID()) continue;
        AXUIElementSetMessagingTimeout(candidate, 0.2f);
        CGRect rect;
        pid_t pid = 0;
        if (AXUIElementGetPid(candidate, &pid) != kAXErrorSuccess || pid != w->pid ||
            !ax_bounds(candidate, &rect) || !same_rect(w->bounds, rect)) continue;
        if (w->title && CFStringGetLength(w->title) > 0) {
            CFTypeRef title = NULL;
            bool match = AXUIElementCopyAttributeValue(candidate, kAXTitleAttribute, &title) == kAXErrorSuccess &&
                         title && CFGetTypeID(title) == CFStringGetTypeID() && CFEqual(w->title, title);
            if (title) CFRelease(title);
            if (!match) continue;
        }
        ++count;
        if (!found) found = (AXUIElementRef)CFRetain(candidate);
    }
    CFRelease(windows);
    if (count != 1 || n > 256) {
        if (found) CFRelease(found);
        *error = count < 0 || n > 256 ? "window_identity_budget_exceeded" :
                 count > 1 ? "ambiguous_accessibility_window" : "accessibility_window_not_found";
        return NULL;
    }
    return found;
}

static void append_status(jbuf_t *b) {
    uint32_t count = 0;
    CGError display_result = CGGetActiveDisplayList(0, NULL, &count);
    jbuf_appendf(b, "{\"ok\":true,\"supported\":true,\"platform\":\"macos\","
                    "\"accessibility_trusted\":%s,\"screen_recording_allowed\":%s,"
                    "\"display_inventory_available\":%s,\"active_displays\":%u,"
                    "\"coordinate_space\":\"global_display_points\","
                    "\"window_identity\":\"window_id+pid+unique_ax_geometry_and_available_title\","
                    "\"permission_prompted\":false}",
                 AXIsProcessTrusted() ? "true" : "false",
                 CGPreflightScreenCaptureAccess() ? "true" : "false",
                 display_result == kCGErrorSuccess ? "true" : "false", count);
}

static bool list_desktop(const desktop_request_t *r, char *out, size_t cap) {
    CGWindowListOption options = kCGWindowListExcludeDesktopElements |
        (r->include_offscreen ? kCGWindowListOptionAll : kCGWindowListOptionOnScreenOnly);
    CFArrayRef windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID);
    if (!windows) return error_result(out, cap, "window_inventory_unavailable", NULL);
    jbuf_t b;
    jbuf_init(&b, 8192);
    jbuf_append(&b, "{\"ok\":true,\"coordinate_space\":\"global_display_points\",\"displays\":[");
    CGDirectDisplayID ids[32];
    uint32_t count = 0;
    CGError rc = CGGetActiveDisplayList(32, ids, &count);
    if (rc == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; ++i) {
            if (i) jbuf_append(&b, ",");
            jbuf_appendf(&b, "{\"display_id\":%u,\"main\":%s,\"bounds\":", ids[i],
                         ids[i] == CGMainDisplayID() ? "true" : "false");
            append_rect(&b, CGDisplayBounds(ids[i]));
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
            if (mode) {
                jbuf_appendf(&b, ",\"pixel_width\":%zu,\"pixel_height\":%zu",
                             CGDisplayModeGetPixelWidth(mode), CGDisplayModeGetPixelHeight(mode));
                CFRelease(mode);
            }
            jbuf_append(&b, "}");
        }
    }
    jbuf_appendf(&b, "],\"display_inventory_available\":%s,\"windows\":[", rc == kCGErrorSuccess ? "true" : "false");
    int emitted = 0;
    bool truncated = false;
    for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
        desktop_window_t w;
        if (!window_from_dictionary(CFArrayGetValueAtIndex(windows, i), &w) ||
            (r->pid && r->pid != w.pid)) continue;
        if (emitted >= r->limit) { truncated = true; break; }
        if (emitted++) jbuf_append(&b, ",");
        append_window(&b, &w, r->include_titles);
    }
    CFRelease(windows);
    jbuf_appendf(&b, "],\"count\":%d,\"truncated\":%s,\"titles_included\":%s}",
                 emitted, truncated ? "true" : "false", r->include_titles ? "true" : "false");
    return finish(&b, out, cap, true);
}

typedef struct {
    int max_depth, max_nodes, nodes;
    double deadline;
    bool truncated;
} snapshot_budget_t;

static bool snapshot_exhausted(snapshot_budget_t *s, const jbuf_t *b) {
    if (s->nodes >= s->max_nodes || b->len > 192 * 1024 || monotonic_seconds() >= s->deadline) {
        s->truncated = true;
        return true;
    }
    return false;
}

static void snapshot_node(jbuf_t *b, AXUIElementRef node, int depth, snapshot_budget_t *s) {
    ++s->nodes;
    AXUIElementSetMessagingTimeout(node, 0.15f);
    jbuf_appendf(b, "{\"node_id\":%d", s->nodes);
    const CFStringRef attrs[] = {kAXRoleAttribute, kAXSubroleAttribute, kAXTitleAttribute, kAXDescriptionAttribute};
    const char *keys[] = {"role", "subrole", "title", "description"};
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); ++i) {
        if (monotonic_seconds() >= s->deadline) { s->truncated = true; break; }
        CFTypeRef value = NULL;
        if (AXUIElementCopyAttributeValue(node, attrs[i], &value) == kAXErrorSuccess && value &&
            CFGetTypeID(value) == CFStringGetTypeID()) {
            jbuf_appendf(b, ",\"%s\":", keys[i]);
            append_cf_string(b, value);
        }
        if (value) CFRelease(value);
    }
    if (monotonic_seconds() < s->deadline) {
        CGRect bounds;
        if (ax_bounds(node, &bounds)) { jbuf_append(b, ",\"bounds\":"); append_rect(b, bounds); }
    }
    if (monotonic_seconds() < s->deadline) {
        CFTypeRef enabled = NULL;
        if (AXUIElementCopyAttributeValue(node, kAXEnabledAttribute, &enabled) == kAXErrorSuccess &&
            enabled && CFGetTypeID(enabled) == CFBooleanGetTypeID())
            jbuf_appendf(b, ",\"enabled\":%s", CFBooleanGetValue(enabled) ? "true" : "false");
        if (enabled) CFRelease(enabled);
    }
    jbuf_append(b, ",\"children\":[");
    CFIndex count = 0;
    if (monotonic_seconds() < s->deadline &&
        AXUIElementGetAttributeValueCount(node, kAXChildrenAttribute, &count) == kAXErrorSuccess && count > 0) {
        if (depth >= s->max_depth || snapshot_exhausted(s, b)) s->truncated = true;
        else {
            CFIndex take = count;
            int room = s->max_nodes - s->nodes;
            if (take > room) take = room;
            CFArrayRef children = NULL;
            if (AXUIElementCopyAttributeValues(node, kAXChildrenAttribute, 0, take, &children) == kAXErrorSuccess && children) {
                int emitted = 0;
                for (CFIndex i = 0; i < CFArrayGetCount(children); ++i) {
                    if (snapshot_exhausted(s, b)) break;
                    AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
                    if (CFGetTypeID(child) != AXUIElementGetTypeID()) continue;
                    if (emitted++) jbuf_append(b, ",");
                    snapshot_node(b, child, depth + 1, s);
                }
                CFRelease(children);
            } else s->truncated = true;
            if (take < count) s->truncated = true;
        }
    }
    if (monotonic_seconds() >= s->deadline) s->truncated = true;
    jbuf_append(b, "]}");
}

static bool settable(AXUIElementRef element, CFStringRef attr) {
    Boolean value = false;
    return AXUIElementIsAttributeSettable(element, attr, &value) == kAXErrorSuccess && value;
}

static bool focused(AXUIElementRef app, AXUIElementRef window) {
    CFTypeRef front = NULL, focus = NULL;
    bool ok = AXUIElementCopyAttributeValue(app, kAXFrontmostAttribute, &front) == kAXErrorSuccess &&
              front && CFGetTypeID(front) == CFBooleanGetTypeID() && CFBooleanGetValue(front) &&
              AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, &focus) == kAXErrorSuccess &&
              focus && CFEqual(focus, window);
    if (front) CFRelease(front);
    if (focus) CFRelease(focus);
    return ok;
}

static bool mutate_window(const desktop_request_t *r, desktop_window_t *w, AXUIElementRef app,
                           AXUIElementRef window, char *out, size_t cap) {
    bool focus = !strcmp(r->action, "focus");
    bool move = !strcmp(r->action, "move") || !strcmp(r->action, "set_bounds");
    bool resize = !strcmp(r->action, "resize") || !strcmp(r->action, "set_bounds");
    if ((move && !settable(window, kAXPositionAttribute)) ||
        (resize && !settable(window, kAXSizeAttribute)))
        return error_result(out, cap, "attribute_not_settable", "target window does not allow the requested bounds change");
    const char *error = NULL;
    desktop_window_t fresh;
    CFArrayRef live = exact_window(r, &fresh, &error);
    if (!live) return error_result(out, cap, error, NULL);
    AXUIElementRef same = resolve_ax(app, &fresh, &error);
    bool matches = same && CFEqual(window, same) && same_rect(w->bounds, fresh.bounds);
    if (same) CFRelease(same);
    CFRelease(live);
    if (!matches) return error_result(out, cap, "window_identity_changed", error);

    AXError rc = kAXErrorSuccess;
    bool changed = false;
    CGRect wanted = w->bounds;
    if (focus) {
        if (settable(window, kAXMainAttribute)) {
            rc = AXUIElementSetAttributeValue(window, kAXMainAttribute, kCFBooleanTrue);
            changed = rc == kAXErrorSuccess;
        }
        if (rc == kAXErrorSuccess) {
            rc = AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
            changed = changed || rc == kAXErrorSuccess;
        }
        if (rc == kAXErrorSuccess) rc = AXUIElementPerformAction(window, kAXRaiseAction);
        /* Some applications expose focusedWindow as read-only. Raising their
         * main window is sufficient if the postcondition confirms exact focus. */
        if (rc == kAXErrorSuccess && settable(app, kAXFocusedWindowAttribute))
            rc = AXUIElementSetAttributeValue(app, kAXFocusedWindowAttribute, window);
    } else {
        if (resize) {
            wanted.size = CGSizeMake(r->width, r->height);
            AXValueRef value = AXValueCreate(kAXValueCGSizeType, &wanted.size);
            rc = value ? AXUIElementSetAttributeValue(window, kAXSizeAttribute, value) : kAXErrorFailure;
            if (value) CFRelease(value);
            changed = rc == kAXErrorSuccess;
        }
        if (move && rc == kAXErrorSuccess) {
            wanted.origin = CGPointMake(r->x, r->y);
            AXValueRef value = AXValueCreate(kAXValueCGPointType, &wanted.origin);
            rc = value ? AXUIElementSetAttributeValue(window, kAXPositionAttribute, value) : kAXErrorFailure;
            if (value) CFRelease(value);
            changed = changed || rc == kAXErrorSuccess;
        }
    }
    bool verified = false;
    CGRect observed = w->bounds;
    bool have_bounds = false;
    double deadline = monotonic_seconds() + 0.8;
    do {
        have_bounds = ax_bounds(window, &observed);
        verified = focus ? focused(app, window) : have_bounds && same_rect(wanted, observed);
        if (verified && !focus) {
            /* CG inventory can trail AX by a compositor tick. Acknowledging
             * geometry only after both agree keeps the next exact-ID action
             * from rejecting the window using its previous CG rectangle. */
            desktop_window_t cg_window;
            const char *inventory_error = NULL;
            CFArrayRef inventory = exact_window(r, &cg_window, &inventory_error);
            verified = inventory && same_rect(wanted, cg_window.bounds);
            if (inventory) CFRelease(inventory);
        }
        if (verified || rc != kAXErrorSuccess) break;
        struct timespec delay = {0, 20000000};
        nanosleep(&delay, NULL);
    } while (monotonic_seconds() < deadline);
    bool ok = rc == kAXErrorSuccess && verified;
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_appendf(&b, "{\"ok\":%s,\"action\":", ok ? "true" : "false");
    jbuf_append_json_str(&b, r->action);
    jbuf_appendf(&b, ",\"window_id\":%u,\"pid\":%d,\"mutation_started\":%s,\"verified\":%s,\"ax_error\":%d",
                 r->window_id, r->pid, changed ? "true" : "false", verified ? "true" : "false", rc);
    if (!ok) jbuf_append(&b, rc == kAXErrorSuccess ? ",\"error\":\"postcondition_failed\"" : ",\"error\":\"accessibility_mutation_failed\"");
    if (have_bounds) { jbuf_append(&b, ",\"observed_bounds\":"); append_rect(&b, observed); }
    jbuf_append(&b, "}");
    return finish(&b, out, cap, ok);
}

static bool target_action(const desktop_request_t *r, char *out, size_t cap) {
    const char *error = NULL;
    desktop_window_t w;
    CFArrayRef inventory = exact_window(r, &w, &error);
    if (!inventory) return error_result(out, cap, error, NULL);
    bool inspect = !strcmp(r->action, "inspect");
    bool snapshot = !strcmp(r->action, "snapshot");
    if (!AXIsProcessTrusted() && !inspect) {
        CFRelease(inventory);
        return error_result(out, cap, "accessibility_permission_required", "use status to inspect readiness; no permission was requested");
    }
    AXUIElementRef app = NULL, window = NULL;
    if (AXIsProcessTrusted()) {
        app = AXUIElementCreateApplication(w.pid);
        if (app) {
            AXUIElementSetMessagingTimeout(app, 0.2f);
            window = resolve_ax(app, &w, &error);
        } else error = "accessibility_application_unavailable";
    } else error = "accessibility_permission_required";
    bool ok;
    if (inspect || (snapshot && window)) {
        jbuf_t b;
        jbuf_init(&b, 4096);
        jbuf_append(&b, "{\"ok\":true,\"window\":");
        append_window(&b, &w, r->include_titles);
        jbuf_appendf(&b, ",\"accessibility_available\":%s", window ? "true" : "false");
        if (error) { jbuf_append(&b, ",\"accessibility_error\":"); jbuf_append_json_str(&b, error); }
        if (window) {
            jbuf_appendf(&b, ",\"focused\":%s,\"movable\":%s,\"resizable\":%s",
                         focused(app, window) ? "true" : "false",
                         settable(window, kAXPositionAttribute) ? "true" : "false",
                         settable(window, kAXSizeAttribute) ? "true" : "false");
            if (snapshot) {
                snapshot_budget_t budget = {r->max_depth, r->max_nodes, 0, monotonic_seconds() + 3.0, false};
                jbuf_append(&b, ",\"tree\":");
                snapshot_node(&b, window, 0, &budget);
                jbuf_appendf(&b, ",\"nodes\":%d,\"truncated\":%s,\"node_ids\":\"snapshot_local\",\"values_included\":false",
                             budget.nodes, budget.truncated ? "true" : "false");
            }
        }
        jbuf_append(&b, "}");
        ok = finish(&b, out, cap, true);
    } else if (!window) ok = error_result(out, cap, error ? error : "accessibility_window_unavailable", NULL);
    else ok = mutate_window(r, &w, app, window, out, cap);
    if (window) CFRelease(window);
    if (app) CFRelease(app);
    CFRelease(inventory);
    return ok;
}

static bool run_macos(const desktop_request_t *r, char *out, size_t cap) {
    if (!strcmp(r->action, "status")) {
        jbuf_t b;
        jbuf_init(&b, 1024);
        append_status(&b);
        return finish(&b, out, cap, true);
    }
    if (!strcmp(r->action, "list")) return list_desktop(r, out, cap);
    if (!strcmp(r->action, "request_permission")) {
        bool granted;
        desktop_input_lock();
        if (!strcmp(r->permission, "accessibility")) {
            const void *keys[] = {kAXTrustedCheckOptionPrompt};
            const void *vals[] = {kCFBooleanTrue};
            CFDictionaryRef options = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            granted = options && AXIsProcessTrustedWithOptions(options);
            if (options) CFRelease(options);
        } else granted = CGRequestScreenCaptureAccess();
        desktop_input_unlock();
        jbuf_t b;
        jbuf_init(&b, 256);
        jbuf_append(&b, "{\"ok\":true,\"permission\":");
        jbuf_append_json_str(&b, r->permission);
        jbuf_appendf(&b, ",\"request_issued\":true,\"granted\":%s,\"ready\":%s}",
                     granted ? "true" : "false", granted ? "true" : "false");
        return finish(&b, out, cap, true);
    }
    desktop_input_lock();
    bool ok = target_action(r, out, cap);
    desktop_input_unlock();
    return ok;
}
#endif

bool tool_desktop(const char *input_json, char *result, size_t result_len) {
    if (!result || result_len == 0) return false;
    result[0] = '\0';
    if (!input_json || strlen(input_json) > 16384)
        return error_result(result, result_len, "invalid_request", "request missing or too large");
    yyjson_doc *doc = yyjson_read(input_json, strlen(input_json), 0);
    if (!doc) return error_result(result, result_len, "invalid_request", "malformed JSON");
    desktop_request_t request = {0};
    char why[160];
    bool ok;
    if (!parse_request(yyjson_doc_get_root(doc), &request, why, sizeof(why)))
        ok = error_result(result, result_len, "invalid_request", why);
    else {
#if DESKTOP_HAS_MACOS
        ok = run_macos(&request, result, result_len);
#else
        if (!strcmp(request.action, "status")) {
            jbuf_t b;
            jbuf_init(&b, 256);
            jbuf_append(&b, "{\"ok\":true,\"supported\":false,\"platform\":\"unsupported\",\"permission_prompted\":false}");
            ok = finish(&b, result, result_len, true);
        } else ok = error_result(result, result_len, "unsupported_platform", "desktop requires macOS ApplicationServices");
#endif
    }
    yyjson_doc_free(doc);
    return ok;
}
