#include "app_control.h"
#include "json_util.h"
#include "process_capture.h"
#include "tool_content.h"
#include "vision_ground.h"
#include "../vendor/yyjson.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) && !defined(DSCO_APP_CONTROL_NO_MACOS)
#define APP_CONTROL_HAS_MACOS 1
#include <ApplicationServices/ApplicationServices.h>
#else
#define APP_CONTROL_HAS_MACOS 0
#endif

#define LABEL_MAX 160
#define VALUE_MAX 200
#define WALK_NODE_BUDGET 4000
#define WALK_DEPTH_MAX 40
#define WALK_SECONDS 3.0
#define FIND_RESULTS 5
#define FIND_AX_GOOD_ENOUGH 0.7
#define VISION_DEFAULT_MIN_CONFIDENCE 0.5
#define VISION_IMAGE_LONG_EDGE "1280"
#define VISION_IMAGE_MIN_SIDE 280 /* TypeSafe minimum per side */

bool app_control_action_mutates(const char *action) {
    static const char *const mutating[] = {"press", "set_value", "focus", "type", "key", "menu"};
    for (size_t i = 0; action && i < sizeof(mutating) / sizeof(*mutating); i++)
        if (!strcmp(action, mutating[i])) return true;
    return false;
}

/* ── find scoring (portable) ─────────────────────────────────────────────── */

static void lower_copy(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (; in && in[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = '\0';
}

typedef struct { const char *word; const char *roles; } role_word_t;
static const role_word_t ROLE_WORDS[] = {
    {"button", "button,menubutton,popupbutton"},  {"btn", "button"},
    {"field", "textfield,textarea,combobox"},     {"textbox", "textfield,textarea"},
    {"input", "textfield,textarea,combobox"},     {"box", "textfield,textarea,combobox,checkbox"},
    {"checkbox", "checkbox"},                     {"check", "checkbox"},
    {"toggle", "checkbox,switch"},                {"switch", "checkbox,switch"},
    {"link", "link"},                             {"menu", "menuitem,menubutton,menubaritem,popupbutton"},
    {"item", "menuitem,row,cell"},                {"tab", "radiobutton,tab"},
    {"radio", "radiobutton"},                     {"dropdown", "popupbutton,combobox"},
    {"popup", "popupbutton"},                     {"slider", "slider"},
    {"row", "row,cell"},                          {"image", "image"},
    {"icon", "image,button"},                     {"text", "statictext,textfield,textarea"},
    {"label", "statictext"},                      {"search", "textfield,searchfield"},
};

static bool role_in_list(const char *role_lc, const char *list) {
    size_t n = strlen(role_lc);
    for (const char *p = list; *p;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == n && !strncmp(p, role_lc, n)) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

double app_control_match_score(const char *query, const char *role, const char *label,
                               const char *value, bool enabled) {
    char q[256], r[64], l[LABEL_MAX + 1], v[VALUE_MAX + 1];
    lower_copy(query, q, sizeof(q));
    lower_copy(role && !strncmp(role, "AX", 2) ? role + 2 : role, r, sizeof(r));
    lower_copy(label, l, sizeof(l));
    lower_copy(value, v, sizeof(v));
    /* Split the query into content words and role hints. */
    char content[256] = "";
    int tokens = 0, hits = 0, role_hints = 0;
    bool role_ok = false;
    char scratch[256];
    snprintf(scratch, sizeof(scratch), "%s", q);
    char *save = NULL;
    for (char *t = strtok_r(scratch, " \t\n\"'`.,:;!?()[]{}", &save); t;
         t = strtok_r(NULL, " \t\n\"'`.,:;!?()[]{}", &save)) {
        if (!strcmp(t, "the") || !strcmp(t, "a") || !strcmp(t, "an") || !strcmp(t, "named") ||
            !strcmp(t, "called") || !strcmp(t, "labeled") || !strcmp(t, "labelled")) continue;
        bool hint = false;
        for (size_t i = 0; i < sizeof(ROLE_WORDS) / sizeof(*ROLE_WORDS); i++) {
            if (!strcmp(t, ROLE_WORDS[i].word)) {
                hint = true;
                role_hints++;
                if (r[0] && role_in_list(r, ROLE_WORDS[i].roles)) role_ok = true;
            }
        }
        /* A role word can also be literal label text ("Search", "Menu"). */
        if (hint && !strstr(l, t)) continue;
        if (content[0]) strncat(content, " ", sizeof(content) - strlen(content) - 1);
        strncat(content, t, sizeof(content) - strlen(content) - 1);
        tokens++;
        if (strstr(l, t) || strstr(v, t)) hits++;
    }
    double score = 0.0;
    if (!tokens) score = role_ok ? 0.3 : 0.0;
    else if (l[0] && !strcmp(l, content)) score = 1.0;
    else if (l[0] && strstr(l, content)) score = 0.85;
    else if (hits == tokens && l[0] && strstr(l, content) == NULL) score = 0.75;
    else if (hits == tokens) score = 0.65; /* matched only through value text */
    else score = 0.5 * (double)hits / (double)tokens;
    if (role_hints) score = role_ok ? fmin(1.0, score + 0.1) : score * 0.6;
    if (!enabled) score *= 0.9;
    return score;
}

#if APP_CONTROL_HAS_MACOS

/* ── element cache ───────────────────────────────────────────────────────── */

typedef struct {
    AXUIElementRef ref;
    char role[48];
    char label[LABEL_MAX + 1];
    char value[VALUE_MAX + 1];
    CGRect frame;
    bool has_frame, enabled, focused, pressable;
} element_t;

static pthread_mutex_t s_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static element_t s_elements[APP_CONTROL_MAX_ELEMENTS];
static int s_count;
static pid_t s_pid;
static unsigned s_generation;

static void cache_clear_locked(void) {
    for (int i = 0; i < s_count; i++)
        if (s_elements[i].ref) CFRelease(s_elements[i].ref);
    memset(s_elements, 0, sizeof(s_elements));
    s_count = 0;
}

/* ── small helpers ───────────────────────────────────────────────────────── */

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static bool finish(jbuf_t *b, char *out, size_t cap, bool ok) {
    if (!out || !cap) { jbuf_free(b); return false; }
    if (!b->data || b->len >= cap) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"result_buffer_too_small\"}");
        jbuf_free(b);
        return false;
    }
    memcpy(out, b->data, b->len + 1);
    jbuf_free(b);
    return ok;
}

static bool fail(char *out, size_t cap, const char *code, const char *detail) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":false,\"error\":");
    jbuf_append_json_str(&b, code);
    if (detail) { jbuf_append(&b, ",\"detail\":"); jbuf_append_json_str(&b, detail); }
    jbuf_append(&b, "}");
    return finish(&b, out, cap, false);
}

static bool cf_to_utf8(CFTypeRef v, char *out, size_t cap) {
    out[0] = '\0';
    if (!v) return false;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        CFIndex units = CFStringGetLength(v), used = 0;
        /* Converts whole characters only, so a full buffer never ends mid-sequence. */
        CFStringGetBytes(v, CFRangeMake(0, units), kCFStringEncodingUTF8, '?', false,
                         (UInt8 *)out, (CFIndex)cap - 1, &used);
        out[used] = '\0';
        for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        return out[0] != '\0';
    }
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        double d = 0;
        if (CFNumberGetValue(v, kCFNumberDoubleType, &d)) snprintf(out, cap, "%g", d);
        return out[0] != '\0';
    }
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        snprintf(out, cap, "%s", CFBooleanGetValue(v) ? "true" : "false");
        return true;
    }
    return false;
}

static bool copy_text(AXUIElementRef e, CFStringRef attr, char *out, size_t cap) {
    CFTypeRef v = NULL;
    bool ok = AXUIElementCopyAttributeValue(e, attr, &v) == kAXErrorSuccess && cf_to_utf8(v, out, cap);
    if (v) CFRelease(v);
    if (!ok) out[0] = '\0';
    return ok;
}

static bool copy_bool(AXUIElementRef e, CFStringRef attr, bool fallback) {
    CFTypeRef v = NULL;
    bool result = fallback;
    if (AXUIElementCopyAttributeValue(e, attr, &v) == kAXErrorSuccess && v &&
        CFGetTypeID(v) == CFBooleanGetTypeID())
        result = CFBooleanGetValue(v);
    if (v) CFRelease(v);
    return result;
}

static bool ax_frame(AXUIElementRef e, CGRect *out) {
    CFTypeRef pos = NULL, size = NULL;
    CGPoint p;
    CGSize s;
    bool ok = AXUIElementCopyAttributeValue(e, kAXPositionAttribute, &pos) == kAXErrorSuccess &&
              AXUIElementCopyAttributeValue(e, kAXSizeAttribute, &size) == kAXErrorSuccess &&
              pos && size && CFGetTypeID(pos) == AXValueGetTypeID() &&
              CFGetTypeID(size) == AXValueGetTypeID() &&
              AXValueGetValue(pos, kAXValueCGPointType, &p) &&
              AXValueGetValue(size, kAXValueCGSizeType, &s);
    if (pos) CFRelease(pos);
    if (size) CFRelease(size);
    if (ok) *out = (CGRect){p, s};
    return ok;
}

static bool has_action(AXUIElementRef e, CFStringRef action) {
    CFArrayRef names = NULL;
    bool found = false;
    if (AXUIElementCopyActionNames(e, &names) == kAXErrorSuccess && names) {
        found = CFArrayContainsValue(names, CFRangeMake(0, CFArrayGetCount(names)), action);
        CFRelease(names);
    }
    return found;
}

static bool is_pressable(AXUIElementRef e) {
    return has_action(e, kAXPressAction) || has_action(e, kAXConfirmAction) ||
           has_action(e, kAXPickAction) || has_action(e, CFSTR("AXOpen"));
}

static const char *const ACTIONABLE_ROLES[] = {
    "AXButton", "AXTextField", "AXTextArea", "AXCheckBox", "AXRadioButton", "AXPopUpButton",
    "AXComboBox", "AXMenuButton", "AXLink", "AXMenuItem", "AXSlider", "AXTab", "AXIncrementor",
    "AXDisclosureTriangle", "AXSearchField", "AXSwitch", "AXColorWell", "AXSegmentedControl",
    "AXRow", "AXCell",
};

static bool actionable_role(const char *role) {
    for (size_t i = 0; i < sizeof(ACTIONABLE_ROLES) / sizeof(*ACTIONABLE_ROLES); i++)
        if (!strcmp(role, ACTIONABLE_ROLES[i])) return true;
    return false;
}

static void element_label(AXUIElementRef e, char *out, size_t cap) {
    if (copy_text(e, kAXTitleAttribute, out, cap)) return;
    if (copy_text(e, kAXDescriptionAttribute, out, cap)) return;
    CFTypeRef title_el = NULL;
    if (AXUIElementCopyAttributeValue(e, kAXTitleUIElementAttribute, &title_el) == kAXErrorSuccess &&
        title_el && CFGetTypeID(title_el) == AXUIElementGetTypeID()) {
        bool ok = copy_text(title_el, kAXValueAttribute, out, cap) ||
                  copy_text(title_el, kAXTitleAttribute, out, cap);
        CFRelease(title_el);
        if (ok) return;
    } else if (title_el) CFRelease(title_el);
    if (copy_text(e, kAXPlaceholderValueAttribute, out, cap)) return;
    copy_text(e, kAXHelpAttribute, out, cap);
}

/* Fill a cache slot for e (retained). */
static bool describe(AXUIElementRef e, element_t *out) {
    memset(out, 0, sizeof(*out));
    if (!copy_text(e, kAXRoleAttribute, out->role, sizeof(out->role))) return false;
    element_label(e, out->label, sizeof(out->label));
    /* Secure text fields never expose their value; do not try. */
    char subrole[48] = "";
    copy_text(e, kAXSubroleAttribute, subrole, sizeof(subrole));
    if (strcmp(subrole, "AXSecureTextField")) copy_text(e, kAXValueAttribute, out->value, sizeof(out->value));
    /* Window chrome (close/minimize/zoom) is only identified by subrole. */
    if (!out->label[0] && subrole[0] && strcmp(subrole, "AXStandardWindow"))
        snprintf(out->label, sizeof(out->label), "%s", strncmp(subrole, "AX", 2) ? subrole : subrole + 2);
    out->has_frame = ax_frame(e, &out->frame);
    out->enabled = copy_bool(e, kAXEnabledAttribute, true);
    out->focused = copy_bool(e, kAXFocusedAttribute, false);
    out->pressable = is_pressable(e);
    out->ref = (AXUIElementRef)CFRetain(e);
    return true;
}

typedef struct {
    int max, visited;
    double deadline;
    bool include_text, truncated;
} walk_t;

static void walk(AXUIElementRef node, int depth, walk_t *w) {
    if (s_count >= w->max || w->visited >= WALK_NODE_BUDGET || now_seconds() > w->deadline) {
        w->truncated = true;
        return;
    }
    w->visited++;
    AXUIElementSetMessagingTimeout(node, 0.25f);
    char role[48] = "";
    copy_text(node, kAXRoleAttribute, role, sizeof(role));
    bool listed = actionable_role(role);
    if (!listed && w->include_text && !strcmp(role, "AXStaticText")) {
        char v[8];
        listed = copy_text(node, kAXValueAttribute, v, sizeof(v));
    }
    if (!listed && strcmp(role, "AXWindow") && strcmp(role, "AXGroup") &&
        strcmp(role, "AXScrollArea") && strcmp(role, "AXSplitGroup") && strcmp(role, "AXToolbar") &&
        strcmp(role, "AXLayoutArea") && strcmp(role, "AXList") && strcmp(role, "AXTable") &&
        strcmp(role, "AXOutline") && strcmp(role, "AXWebArea") && strcmp(role, "AXSheet"))
        listed = is_pressable(node);
    if (listed && describe(node, &s_elements[s_count])) {
        /* Skip invisible zero-size elements; they cannot be acted on reliably. */
        element_t *e = &s_elements[s_count];
        if (e->has_frame && (e->frame.size.width < 1 || e->frame.size.height < 1)) {
            CFRelease(e->ref);
            memset(e, 0, sizeof(*e));
        } else s_count++;
    }
    if (depth >= WALK_DEPTH_MAX) { w->truncated = true; return; }
    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(node, kAXChildrenAttribute, (CFTypeRef *)&children) != kAXErrorSuccess ||
        !children || CFGetTypeID(children) != CFArrayGetTypeID()) {
        if (children) CFRelease(children);
        return;
    }
    for (CFIndex i = 0; i < CFArrayGetCount(children); i++) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
        if (CFGetTypeID(child) == AXUIElementGetTypeID()) walk(child, depth + 1, w);
        if (w->truncated && (s_count >= w->max || now_seconds() > w->deadline)) break;
    }
    CFRelease(children);
}

/* ── target resolution ───────────────────────────────────────────────────── */

typedef struct {
    pid_t pid;
    uint32_t window_id;
    char app_name[128];
    AXUIElementRef app, window; /* retained */
    CGRect frame;
} target_t;

static void target_free(target_t *t) {
    if (t->window) CFRelease(t->window);
    if (t->app) CFRelease(t->app);
    memset(t, 0, sizeof(*t));
}

static bool dict_int(CFDictionaryRef d, CFStringRef key, int64_t *out) {
    CFTypeRef v = CFDictionaryGetValue(d, key);
    return v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberSInt64Type, out);
}

static bool dict_bounds(CFDictionaryRef d, CGRect *out) {
    CFTypeRef b = CFDictionaryGetValue(d, kCGWindowBounds);
    return b && CFGetTypeID(b) == CFDictionaryGetTypeID() && CGRectMakeWithDictionaryRepresentation(b, out);
}

static bool near_rect(CGRect a, CGRect b) {
    return fabs(a.origin.x - b.origin.x) <= 2 && fabs(a.origin.y - b.origin.y) <= 2 &&
           fabs(a.size.width - b.size.width) <= 2 && fabs(a.size.height - b.size.height) <= 2;
}

/* Resolve app name -> pid among apps with normal on-screen windows. */
static pid_t pid_for_app(const char *name, char *err, size_t errlen) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly |
                                                 kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) { snprintf(err, errlen, "window_inventory_unavailable"); return 0; }
    pid_t exact = 0, prefix = 0;
    bool exact_ambiguous = false, prefix_ambiguous = false;
    size_t n = strlen(name);
    for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
        int64_t pid = 0, layer = 1;
        char owner[128];
        if (!dict_int(d, kCGWindowOwnerPID, &pid) || !dict_int(d, kCGWindowLayer, &layer) || layer != 0 ||
            !cf_to_utf8(CFDictionaryGetValue(d, kCGWindowOwnerName), owner, sizeof(owner))) continue;
        if (!strcasecmp(owner, name)) {
            if (exact && exact != (pid_t)pid) exact_ambiguous = true;
            exact = (pid_t)pid;
        } else if (!strncasecmp(owner, name, n)) {
            if (prefix && prefix != (pid_t)pid) prefix_ambiguous = true;
            prefix = (pid_t)pid;
        }
    }
    CFRelease(list);
    if (exact && !exact_ambiguous) return exact;
    if (!exact && prefix && !prefix_ambiguous) return prefix;
    snprintf(err, errlen, exact || prefix ? "ambiguous_app_name; pass pid" : "app_not_found_on_screen");
    return 0;
}

/* Find the CG window id for an AX window frame, or verify a requested id. */
static uint32_t window_id_for(pid_t pid, CGRect frame, uint32_t wanted, CGRect *wanted_frame,
                              char *owner, size_t owner_cap) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    uint32_t found = 0;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
        int64_t id = 0, p = 0, layer = 1;
        CGRect b;
        if (!dict_int(d, kCGWindowNumber, &id) || !dict_int(d, kCGWindowOwnerPID, &p) || p != pid ||
            !dict_int(d, kCGWindowLayer, &layer) || layer != 0 || !dict_bounds(d, &b)) continue;
        if (owner && owner_cap && !owner[0]) cf_to_utf8(CFDictionaryGetValue(d, kCGWindowOwnerName), owner, owner_cap);
        if (wanted ? (uint32_t)id == wanted : near_rect(b, frame)) {
            found = (uint32_t)id;
            if (wanted_frame) *wanted_frame = b;
            break;
        }
    }
    if (list) CFRelease(list);
    return found;
}

static bool resolve_target(yyjson_val *root, bool need_window, target_t *t, char *err, size_t errlen) {
    memset(t, 0, sizeof(*t));
    yyjson_val *pid_v = yyjson_obj_get(root, "pid");
    const char *app = yyjson_get_str(yyjson_obj_get(root, "app"));
    if (yyjson_is_uint(pid_v) && yyjson_get_uint(pid_v) > 0 && yyjson_get_uint(pid_v) <= INT32_MAX)
        t->pid = (pid_t)yyjson_get_uint(pid_v);
    else if (app && *app) t->pid = pid_for_app(app, err, errlen);
    else { snprintf(err, errlen, "pid_or_app_required"); return false; }
    if (!t->pid) return false;
    if (t->pid == getpid()) { snprintf(err, errlen, "refusing_to_control_self"); return false; }
    t->app = AXUIElementCreateApplication(t->pid);
    if (!t->app) { snprintf(err, errlen, "app_not_accessible"); return false; }
    AXUIElementSetMessagingTimeout(t->app, 1.0f);
    if (!need_window) return true;

    yyjson_val *wid_v = yyjson_obj_get(root, "window_id");
    uint32_t wanted = yyjson_is_uint(wid_v) ? (uint32_t)yyjson_get_uint(wid_v) : 0;
    CGRect wanted_frame = CGRectZero;
    if (wanted && !window_id_for(t->pid, CGRectZero, wanted, &wanted_frame, t->app_name, sizeof(t->app_name))) {
        snprintf(err, errlen, "window_not_found_for_pid");
        target_free(t);
        return false;
    }
    if (wanted) {
        CFArrayRef windows = NULL;
        if (AXUIElementCopyAttributeValue(t->app, kAXWindowsAttribute, (CFTypeRef *)&windows) == kAXErrorSuccess &&
            windows && CFGetTypeID(windows) == CFArrayGetTypeID()) {
            int matches = 0;
            for (CFIndex i = 0; i < CFArrayGetCount(windows); i++) {
                AXUIElementRef w = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);
                CGRect f;
                if (CFGetTypeID(w) == AXUIElementGetTypeID() && ax_frame(w, &f) && near_rect(f, wanted_frame)) {
                    if (!matches++) t->window = (AXUIElementRef)CFRetain(w);
                }
            }
            if (matches > 1) { CFRelease(t->window); t->window = NULL; snprintf(err, errlen, "ambiguous_accessibility_window"); }
        }
        if (windows) CFRelease(windows);
    } else {
        const CFStringRef order[] = {kAXFocusedWindowAttribute, kAXMainWindowAttribute};
        for (size_t i = 0; i < 2 && !t->window; i++) {
            CFTypeRef w = NULL;
            if (AXUIElementCopyAttributeValue(t->app, order[i], &w) == kAXErrorSuccess && w &&
                CFGetTypeID(w) == AXUIElementGetTypeID()) t->window = (AXUIElementRef)w;
            else if (w) CFRelease(w);
        }
        if (!t->window) {
            CFArrayRef windows = NULL;
            if (AXUIElementCopyAttributeValue(t->app, kAXWindowsAttribute, (CFTypeRef *)&windows) == kAXErrorSuccess &&
                windows && CFGetTypeID(windows) == CFArrayGetTypeID() && CFArrayGetCount(windows) > 0)
                t->window = (AXUIElementRef)CFRetain(CFArrayGetValueAtIndex(windows, 0));
            if (windows) CFRelease(windows);
        }
    }
    if (!t->window) {
        if (!err[0]) snprintf(err, errlen, AXIsProcessTrusted() ? "no_accessible_window" : "accessibility_permission_required");
        target_free(t);
        return false;
    }
    if (!ax_frame(t->window, &t->frame)) {
        snprintf(err, errlen, "window_frame_unavailable");
        target_free(t);
        return false;
    }
    t->window_id = wanted ? wanted : window_id_for(t->pid, t->frame, 0, NULL, t->app_name, sizeof(t->app_name));
    return true;
}

/* ── output ──────────────────────────────────────────────────────────────── */

static void append_target(jbuf_t *b, const target_t *t) {
    jbuf_appendf(b, "\"pid\":%d", t->pid);
    if (t->app_name[0]) { jbuf_append(b, ",\"app\":"); jbuf_append_json_str(b, t->app_name); }
    if (t->window_id) jbuf_appendf(b, ",\"window_id\":%u", t->window_id);
    if (t->window)
        jbuf_appendf(b, ",\"window_frame\":[%d,%d,%d,%d]", (int)lround(t->frame.origin.x),
                     (int)lround(t->frame.origin.y), (int)lround(t->frame.size.width),
                     (int)lround(t->frame.size.height));
}

static void append_element(jbuf_t *b, int index, const element_t *e, const CGRect *window) {
    jbuf_appendf(b, "{\"id\":\"e%d\",\"role\":", index + 1);
    jbuf_append_json_str(b, strncmp(e->role, "AX", 2) ? e->role : e->role + 2);
    if (e->label[0]) { jbuf_append(b, ",\"label\":"); jbuf_append_json_str(b, e->label); }
    if (e->value[0] && strcmp(e->value, e->label)) { jbuf_append(b, ",\"value\":"); jbuf_append_json_str(b, e->value); }
    if (e->has_frame && window)
        jbuf_appendf(b, ",\"frame\":[%d,%d,%d,%d]", (int)lround(e->frame.origin.x - window->origin.x),
                     (int)lround(e->frame.origin.y - window->origin.y), (int)lround(e->frame.size.width),
                     (int)lround(e->frame.size.height));
    if (!e->enabled) jbuf_append(b, ",\"enabled\":false");
    if (e->focused) jbuf_append(b, ",\"focused\":true");
    if (e->pressable) jbuf_append(b, ",\"pressable\":true");
    jbuf_append(b, "}");
}

/* ── window image ────────────────────────────────────────────────────────── */

static bool capture_window(uint32_t window_id, char *path, size_t cap, int *w, int *h) {
    if (!window_id || !CGPreflightScreenCaptureAccess()) return false;
    char tmp[] = "/tmp/dsco_window_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    close(fd);
    char wid[32];
    snprintf(wid, sizeof(wid), "-l%u", window_id);
    process_capture_t cap_out;
    char *shot[] = {"/usr/sbin/screencapture", "-x", "-o", "-t", "jpg", wid, tmp, NULL};
    bool ok = process_capture(shot[0], shot, 5000, 1024, &cap_out) && cap_out.exit_code == 0;
    process_capture_free(&cap_out);
    if (ok) {
        char *resize[] = {"/usr/bin/sips", "-Z", VISION_IMAGE_LONG_EDGE, tmp, NULL};
        ok = process_capture(resize[0], resize, 5000, 1024, &cap_out) && cap_out.exit_code == 0;
        process_capture_free(&cap_out);
    }
    if (ok) {
        char *info[] = {"/usr/bin/sips", "-g", "pixelWidth", "-g", "pixelHeight", tmp, NULL};
        ok = process_capture(info[0], info, 5000, 4096, &cap_out) && cap_out.exit_code == 0 && cap_out.output;
        const char *pw = ok ? strstr(cap_out.output, "pixelWidth:") : NULL;
        const char *ph = ok ? strstr(cap_out.output, "pixelHeight:") : NULL;
        ok = pw && ph && sscanf(pw, "pixelWidth: %d", w) == 1 && sscanf(ph, "pixelHeight: %d", h) == 1 &&
             *w > 0 && *h > 0;
        process_capture_free(&cap_out);
    }
    if (!ok || strlen(tmp) >= cap) { unlink(tmp); return false; }
    snprintf(path, cap, "%s", tmp);
    return true;
}

/* ── actions ─────────────────────────────────────────────────────────────── */

static bool action_apps(char *out, size_t cap) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly |
                                                 kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return fail(out, cap, "window_inventory_unavailable", NULL);
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"ok\":true,\"windows\":[");
    int emitted = 0;
    pid_t self = getpid();
    for (CFIndex i = 0; i < CFArrayGetCount(list) && emitted < 64; i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
        int64_t id = 0, pid = 0, layer = 1;
        CGRect r;
        char owner[128] = "", title[LABEL_MAX + 1] = "";
        if (!dict_int(d, kCGWindowNumber, &id) || !dict_int(d, kCGWindowOwnerPID, &pid) || pid == self ||
            !dict_int(d, kCGWindowLayer, &layer) || layer != 0 || !dict_bounds(d, &r) ||
            r.size.width < 40 || r.size.height < 40) continue;
        cf_to_utf8(CFDictionaryGetValue(d, kCGWindowOwnerName), owner, sizeof(owner));
        cf_to_utf8(CFDictionaryGetValue(d, kCGWindowName), title, sizeof(title));
        if (emitted++) jbuf_append(&b, ",");
        jbuf_appendf(&b, "{\"pid\":%lld,\"window_id\":%lld,\"app\":", (long long)pid, (long long)id);
        jbuf_append_json_str(&b, owner);
        if (title[0]) { jbuf_append(&b, ",\"title\":"); jbuf_append_json_str(&b, title); }
        jbuf_appendf(&b, ",\"frame\":[%d,%d,%d,%d]}", (int)r.origin.x, (int)r.origin.y,
                     (int)r.size.width, (int)r.size.height);
    }
    CFRelease(list);
    jbuf_appendf(&b, "],\"count\":%d,\"note\":\"front-to-back order; pass pid or app to other actions\"}", emitted);
    return finish(&b, out, cap, true);
}

/* Walk the target window into a fresh cache generation. Caller holds lock. */
static walk_t rebuild_cache_locked(const target_t *t, int max, bool include_text) {
    cache_clear_locked();
    s_pid = t->pid;
    s_generation++;
    walk_t w = {.max = max, .deadline = now_seconds() + WALK_SECONDS, .include_text = include_text};
    walk(t->window, 0, &w);
    return w;
}

static int int_field(yyjson_val *root, const char *key, int fallback, int lo, int hi) {
    yyjson_val *v = yyjson_obj_get(root, key);
    if (!yyjson_is_int(v)) return fallback;
    int64_t n = yyjson_get_sint(v);
    return n < lo ? lo : n > hi ? hi : (int)n;
}

static bool bool_field(yyjson_val *root, const char *key, bool fallback) {
    yyjson_val *v = yyjson_obj_get(root, key);
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : fallback;
}

static bool action_observe(yyjson_val *root, char *out, size_t cap) {
    char err[160] = "";
    target_t t;
    if (!resolve_target(root, true, &t, err, sizeof(err))) return fail(out, cap, err, NULL);
    int max = int_field(root, "max_elements", 150, 1, APP_CONTROL_MAX_ELEMENTS);
    bool include_text = bool_field(root, "include_text", true);
    bool screenshot = bool_field(root, "screenshot", false);
    pthread_mutex_lock(&s_cache_lock);
    walk_t w = rebuild_cache_locked(&t, max, include_text);
    jbuf_t b;
    jbuf_init(&b, 16384);
    jbuf_append(&b, "{\"ok\":true,");
    append_target(&b, &t);
    jbuf_append(&b, ",\"window_title\":");
    char title[LABEL_MAX + 1] = "";
    copy_text(t.window, kAXTitleAttribute, title, sizeof(title));
    jbuf_append_json_str(&b, title);
    jbuf_appendf(&b, ",\"observation\":%u,\"elements\":[", s_generation);
    int emitted = 0;
    bool truncated = w.truncated;
    for (int i = 0; i < s_count; i++) {
        if (b.len + 1024 > cap) { truncated = true; break; }
        if (emitted++) jbuf_append(&b, ",");
        append_element(&b, i, &s_elements[i], &t.frame);
    }
    pthread_mutex_unlock(&s_cache_lock);
    jbuf_appendf(&b, "],\"count\":%d,\"truncated\":%s,\"frames\":\"window-relative points [x,y,w,h]\"",
                 emitted, truncated ? "true" : "false");
    if (screenshot) {
        char path[64];
        int iw = 0, ih = 0;
        if (capture_window(t.window_id, path, sizeof(path), &iw, &ih)) {
            bool attached = tool_content_add_image_file(path, "image/jpeg");
            unlink(path);
            jbuf_appendf(&b, ",\"screenshot_attached\":%s,\"image_size\":[%d,%d]", attached ? "true" : "false", iw, ih);
        } else jbuf_append(&b, ",\"screenshot_attached\":false,\"screenshot_error\":\"window_capture_failed_or_screen_recording_not_allowed\"");
    }
    jbuf_append(&b, "}");
    target_free(&t);
    return finish(&b, out, cap, true);
}

static bool lookup_element(yyjson_val *root, element_t **out, char *err, size_t errlen) {
    const char *id = yyjson_get_str(yyjson_obj_get(root, "element"));
    if (!id || id[0] != 'e') { snprintf(err, errlen, "element_id_required"); return false; }
    char *end = NULL;
    long n = strtol(id + 1, &end, 10);
    if (!end || *end || n < 1 || n > s_count) { snprintf(err, errlen, "unknown_element; run observe or find again"); return false; }
    yyjson_val *pid_v = yyjson_obj_get(root, "pid");
    if (yyjson_is_uint(pid_v) && (pid_t)yyjson_get_uint(pid_v) != s_pid) {
        snprintf(err, errlen, "element_belongs_to_another_pid");
        return false;
    }
    *out = &s_elements[n - 1];
    return true;
}

/* Vision fallback: Box on the window image, then AX hit-test at its center so
 * the result is still a real element that can be pressed without the cursor. */
static void vision_candidate(const target_t *t, const char *query, double min_confidence, jbuf_t *b,
                             int *emitted) {
    char path[64];
    int iw = 0, ih = 0;
    if (!vision_ground_available()) { jbuf_append(b, ",\"vision\":\"not_configured\""); return; }
    if (!capture_window(t->window_id, path, sizeof(path), &iw, &ih)) {
        jbuf_append(b, ",\"vision\":\"window_capture_failed\"");
        return;
    }
    if (iw < VISION_IMAGE_MIN_SIDE || ih < VISION_IMAGE_MIN_SIDE) {
        unlink(path);
        jbuf_append(b, ",\"vision\":\"window_too_small\"");
        return;
    }
    vision_box_t box;
    char why[200] = "";
    bool ok = vision_ground_box(path, query, &box, why, sizeof(why));
    unlink(path);
    if (!ok) {
        jbuf_append(b, ",\"vision\":");
        jbuf_append_json_str(b, why);
        return;
    }
    double cx = t->frame.origin.x + (box.left + box.right) / 2.0 * t->frame.size.width;
    double cy = t->frame.origin.y + (box.top + box.bottom) / 2.0 * t->frame.size.height;
    jbuf_appendf(b, ",\"vision\":{\"confidence\":%.2f,\"box\":[%d,%d,%d,%d],\"accepted\":%s",
                 box.confidence, (int)lround(box.left * t->frame.size.width),
                 (int)lround(box.top * t->frame.size.height),
                 (int)lround((box.right - box.left) * t->frame.size.width),
                 (int)lround((box.bottom - box.top) * t->frame.size.height),
                 box.confidence >= min_confidence ? "true" : "false");
    if (box.confidence < min_confidence) { jbuf_append(b, "}"); return; }
    AXUIElementRef hit = NULL;
    if (AXUIElementCopyElementAtPosition(t->app, (float)cx, (float)cy, &hit) != kAXErrorSuccess || !hit) {
        jbuf_append(b, ",\"hit_test\":\"no_element_at_point\"}");
        return;
    }
    /* Climb from inner text/images to the nearest pressable control. */
    AXUIElementRef cur = hit;
    for (int up = 0; up < 4 && !is_pressable(cur); up++) {
        CFTypeRef parent = NULL;
        if (AXUIElementCopyAttributeValue(cur, kAXParentAttribute, &parent) != kAXErrorSuccess || !parent ||
            CFGetTypeID(parent) != AXUIElementGetTypeID()) {
            if (parent) CFRelease(parent);
            break;
        }
        char role[48] = "";
        copy_text(parent, kAXRoleAttribute, role, sizeof(role));
        if (!strcmp(role, "AXWindow") || !strcmp(role, "AXApplication")) { CFRelease(parent); break; }
        if (cur != hit) CFRelease(cur);
        cur = (AXUIElementRef)parent;
    }
    AXUIElementRef chosen = is_pressable(cur) ? cur : hit;
    if (s_count < APP_CONTROL_MAX_ELEMENTS && describe(chosen, &s_elements[s_count])) {
        jbuf_append(b, ",\"element\":");
        append_element(b, s_count, &s_elements[s_count], &t->frame);
        s_count++;
        (*emitted)++;
    }
    if (cur != hit) CFRelease(cur);
    CFRelease(hit);
    jbuf_append(b, "}");
}

static bool action_find(yyjson_val *root, char *out, size_t cap) {
    const char *query = yyjson_get_str(yyjson_obj_get(root, "query"));
    if (!query || !*query) return fail(out, cap, "query_required", NULL);
    if (strlen(query) > VISION_GROUND_TARGET_MAX) return fail(out, cap, "query_too_long", NULL);
    char err[160] = "";
    target_t t;
    if (!resolve_target(root, true, &t, err, sizeof(err))) return fail(out, cap, err, NULL);
    bool vision = bool_field(root, "vision", true);
    yyjson_val *mc = yyjson_obj_get(root, "min_confidence");
    double min_confidence = yyjson_is_num(mc) ? yyjson_get_num(mc) : VISION_DEFAULT_MIN_CONFIDENCE;

    pthread_mutex_lock(&s_cache_lock);
    walk_t w = rebuild_cache_locked(&t, APP_CONTROL_MAX_ELEMENTS - 1, true);
    double all[APP_CONTROL_MAX_ELEMENTS];
    for (int i = 0; i < s_count; i++) {
        element_t *e = &s_elements[i];
        all[i] = app_control_match_score(query, e->role, e->label, e->value, e->enabled);
        if (e->pressable && all[i] > 0) all[i] = fmin(1.0, all[i] + 0.02); /* tie-break toward actionable */
    }
    int best[FIND_RESULTS];
    double scores[FIND_RESULTS];
    int n = 0;
    while (n < FIND_RESULTS) {
        int pick = -1;
        for (int i = 0; i < s_count; i++)
            if (all[i] > 0.2 && (pick < 0 || all[i] > all[pick])) pick = i;
        if (pick < 0) break;
        best[n] = pick;
        scores[n++] = all[pick];
        all[pick] = -1;
    }
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"ok\":true,");
    append_target(&b, &t);
    jbuf_appendf(&b, ",\"observation\":%u,\"query\":", s_generation);
    jbuf_append_json_str(&b, query);
    jbuf_append(&b, ",\"candidates\":[");
    for (int i = 0; i < n; i++) {
        if (i) jbuf_append(&b, ",");
        size_t mark = b.len;
        append_element(&b, best[i], &s_elements[best[i]], &t.frame);
        /* Splice score and source into the element object. */
        if (b.len > mark && b.data[b.len - 1] == '}') {
            b.len--;
            jbuf_appendf(&b, ",\"score\":%.2f,\"source\":\"accessibility\"}", scores[i]);
        }
    }
    jbuf_append(&b, "]");
    int emitted = n;
    if (vision && (n == 0 || scores[0] < FIND_AX_GOOD_ENOUGH)) vision_candidate(&t, query, min_confidence, &b, &emitted);
    else if (!vision) jbuf_append(&b, ",\"vision\":\"disabled\"");
    pthread_mutex_unlock(&s_cache_lock);
    jbuf_appendf(&b, ",\"scanned_truncated\":%s,\"best\":", w.truncated ? "true" : "false");
    if (n && scores[0] >= FIND_AX_GOOD_ENOUGH) jbuf_appendf(&b, "\"e%d\"", best[0] + 1);
    else jbuf_append(&b, "null");
    jbuf_append(&b, "}");
    target_free(&t);
    return finish(&b, out, cap, emitted > 0);
}

static bool still_valid(AXUIElementRef e) {
    CFTypeRef role = NULL;
    AXError rc = AXUIElementCopyAttributeValue(e, kAXRoleAttribute, &role);
    if (role) CFRelease(role);
    return rc != kAXErrorInvalidUIElement;
}

static bool action_press(yyjson_val *root, char *out, size_t cap) {
    pthread_mutex_lock(&s_cache_lock);
    element_t *e = NULL;
    char err[160] = "";
    if (!lookup_element(root, &e, err, sizeof(err))) { pthread_mutex_unlock(&s_cache_lock); return fail(out, cap, err, NULL); }
    if (!e->enabled) { pthread_mutex_unlock(&s_cache_lock); return fail(out, cap, "element_disabled", e->label); }
    const CFStringRef actions[] = {kAXPressAction, kAXConfirmAction, kAXPickAction, CFSTR("AXOpen")};
    const char *names[] = {"AXPress", "AXConfirm", "AXPick", "AXOpen"};
    const char *used = NULL;
    AXError rc = kAXErrorActionUnsupported;
    for (size_t i = 0; i < 4 && !used; i++) {
        if (!has_action(e->ref, actions[i])) continue;
        rc = AXUIElementPerformAction(e->ref, actions[i]);
        if (rc == kAXErrorSuccess) used = names[i];
    }
    if (!used) {
        pthread_mutex_unlock(&s_cache_lock);
        char detail[64];
        snprintf(detail, sizeof(detail), "AXError %d", (int)rc);
        return fail(out, cap, "press_unsupported_or_failed", detail);
    }
    usleep(150000); /* let the app process the action before readback */
    bool valid = still_valid(e->ref);
    char value[VALUE_MAX + 1] = "";
    if (valid) copy_text(e->ref, kAXValueAttribute, value, sizeof(value));
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"ok\":true,\"performed\":");
    jbuf_append_json_str(&b, used);
    jbuf_append(&b, ",\"label\":");
    jbuf_append_json_str(&b, e->label);
    jbuf_appendf(&b, ",\"element_still_present\":%s", valid ? "true" : "false");
    if (value[0]) { jbuf_append(&b, ",\"value_after\":"); jbuf_append_json_str(&b, value); }
    jbuf_append(&b, ",\"hint\":\"observe again before using other element ids if the UI changed\"}");
    pthread_mutex_unlock(&s_cache_lock);
    return finish(&b, out, cap, true);
}

static bool action_set_value(yyjson_val *root, char *out, size_t cap) {
    const char *text = yyjson_get_str(yyjson_obj_get(root, "text"));
    if (!text) return fail(out, cap, "text_required", NULL);
    pthread_mutex_lock(&s_cache_lock);
    element_t *e = NULL;
    char err[160] = "";
    if (!lookup_element(root, &e, err, sizeof(err))) { pthread_mutex_unlock(&s_cache_lock); return fail(out, cap, err, NULL); }
    Boolean settable = false;
    if (AXUIElementIsAttributeSettable(e->ref, kAXValueAttribute, &settable) != kAXErrorSuccess || !settable) {
        pthread_mutex_unlock(&s_cache_lock);
        return fail(out, cap, "value_not_settable", "use focus + type for this element");
    }
    CFStringRef s = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!s) { pthread_mutex_unlock(&s_cache_lock); return fail(out, cap, "text_not_utf8", NULL); }
    AXError rc = AXUIElementSetAttributeValue(e->ref, kAXValueAttribute, s);
    CFRelease(s);
    char readback[VALUE_MAX + 1] = "";
    copy_text(e->ref, kAXValueAttribute, readback, sizeof(readback));
    bool verified = rc == kAXErrorSuccess && !strncmp(readback, text, VALUE_MAX - 4);
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_appendf(&b, "{\"ok\":%s,\"verified\":%s,\"ax_error\":%d,\"value_after\":",
                 rc == kAXErrorSuccess ? "true" : "false", verified ? "true" : "false", (int)rc);
    jbuf_append_json_str(&b, readback);
    jbuf_append(&b, "}");
    pthread_mutex_unlock(&s_cache_lock);
    return finish(&b, out, cap, rc == kAXErrorSuccess);
}

static bool action_focus(yyjson_val *root, char *out, size_t cap) {
    pthread_mutex_lock(&s_cache_lock);
    element_t *e = NULL;
    char err[160] = "";
    if (!lookup_element(root, &e, err, sizeof(err))) { pthread_mutex_unlock(&s_cache_lock); return fail(out, cap, err, NULL); }
    AXError rc = AXUIElementSetAttributeValue(e->ref, kAXFocusedAttribute, kCFBooleanTrue);
    bool verified = copy_bool(e->ref, kAXFocusedAttribute, false);
    pthread_mutex_unlock(&s_cache_lock);
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"focused_in_app\":%s,\"ax_error\":%d,\"note\":\"app focus only; the app is not raised\"}",
             rc == kAXErrorSuccess ? "true" : "false", verified ? "true" : "false", (int)rc);
    snprintf(out, cap, "%s", buf);
    return rc == kAXErrorSuccess;
}

/* ANSI virtual keycodes indexed by code; '\1' marks non-character keys. */
static const char ANSI_KEYS[] = "asdfhgzxcv\1bqweryt123465=97-80]ou[ip\1lj'k;\\,/nm.\1\1`";
typedef struct { const char *name; CGKeyCode code; } named_key_t;
static const named_key_t NAMED_KEYS[] = {
    {"return", 36}, {"enter", 36}, {"tab", 48}, {"space", 49}, {"delete", 51}, {"backspace", 51},
    {"escape", 53}, {"esc", 53}, {"forwarddelete", 117}, {"left", 123}, {"right", 124},
    {"down", 125}, {"up", 126}, {"home", 115}, {"end", 119}, {"pageup", 116}, {"pagedown", 121},
    {"f1", 122}, {"f2", 120}, {"f3", 99}, {"f4", 118}, {"f5", 96}, {"f6", 97}, {"f7", 98},
    {"f8", 100}, {"f9", 101}, {"f10", 109}, {"f11", 103}, {"f12", 111},
};

static bool keycode_for(const char *name, CGKeyCode *out) {
    char low[32];
    lower_copy(name, low, sizeof(low));
    for (size_t i = 0; i < sizeof(NAMED_KEYS) / sizeof(*NAMED_KEYS); i++)
        if (!strcmp(low, NAMED_KEYS[i].name)) { *out = NAMED_KEYS[i].code; return true; }
    if (strlen(low) == 1)
        for (size_t i = 0; ANSI_KEYS[i]; i++)
            if (ANSI_KEYS[i] == low[0]) { *out = (CGKeyCode)i; return true; }
    return false;
}

static CGEventFlags modifier_for(const char *t) {
    if (!strcasecmp(t, "cmd") || !strcasecmp(t, "command")) return kCGEventFlagMaskCommand;
    if (!strcasecmp(t, "ctrl") || !strcasecmp(t, "control")) return kCGEventFlagMaskControl;
    if (!strcasecmp(t, "alt") || !strcasecmp(t, "option") || !strcasecmp(t, "opt")) return kCGEventFlagMaskAlternate;
    if (!strcasecmp(t, "shift")) return kCGEventFlagMaskShift;
    return 0;
}

static void post_key(pid_t pid, CGEventSourceRef src, CGKeyCode code, CGEventFlags flags,
                     const UniChar *chars, UniCharCount count) {
    for (int down = 1; down >= 0; down--) {
        CGEventRef ev = CGEventCreateKeyboardEvent(src, code, down);
        if (!ev) continue;
        CGEventSetFlags(ev, flags);
        if (chars) CGEventKeyboardSetUnicodeString(ev, count, chars);
        CGEventPostToPid(pid, ev);
        CFRelease(ev);
    }
}

/* Keyboard events reach an app's key window. Making the target window main
 * through AX gives it one without activating the app or touching focus
 * elsewhere. */
static bool make_window_main(const target_t *t) {
    if (!t->window) return false;
    Boolean settable = false;
    if (AXUIElementIsAttributeSettable(t->window, kAXMainAttribute, &settable) != kAXErrorSuccess || !settable)
        return false;
    return AXUIElementSetAttributeValue(t->window, kAXMainAttribute, kCFBooleanTrue) == kAXErrorSuccess;
}

static bool action_key(yyjson_val *root, char *out, size_t cap) {
    const char *keys = yyjson_get_str(yyjson_obj_get(root, "keys"));
    if (!keys || !*keys || strlen(keys) > 64) return fail(out, cap, "keys_required", "e.g. cmd+s or return");
    char err[160] = "";
    target_t t;
    if (!resolve_target(root, true, &t, err, sizeof(err))) return fail(out, cap, err, NULL);
    make_window_main(&t);
    char buf[72];
    snprintf(buf, sizeof(buf), "%s", keys);
    CGEventFlags flags = 0;
    CGKeyCode code = 0;
    bool have = false;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        CGEventFlags m = modifier_for(tok);
        if (m) flags |= m;
        else if (!have && keycode_for(tok, &code)) have = true;
        else { target_free(&t); return fail(out, cap, "unknown_key", tok); }
    }
    if (!have) { target_free(&t); return fail(out, cap, "no_key_in_combo", keys); }
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStatePrivate);
    post_key(t.pid, src, code, flags, NULL, 0);
    if (src) CFRelease(src);
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":true,");
    append_target(&b, &t);
    jbuf_append(&b, ",\"keys\":");
    jbuf_append_json_str(&b, keys);
    jbuf_append(&b, ",\"delivery\":\"posted_to_pid\",\"verified\":false,"
                "\"note\":\"an inactive app has no key window and may ignore keystrokes; "
                "prefer menu (e.g. [\\\"Edit\\\",\\\"Select All\\\"]), press or set_value\"}");
    target_free(&t);
    return finish(&b, out, cap, true);
}

static bool action_type(yyjson_val *root, char *out, size_t cap) {
    const char *text = yyjson_get_str(yyjson_obj_get(root, "text"));
    if (!text) return fail(out, cap, "text_required", NULL);
    if (strlen(text) > 4096) return fail(out, cap, "text_too_long", "max 4096 bytes; use set_value");
    char err[160] = "";
    target_t t;
    if (!resolve_target(root, true, &t, err, sizeof(err))) return fail(out, cap, err, NULL);
    pthread_mutex_lock(&s_cache_lock);
    element_t *e = NULL;
    bool focused = false;
    if (yyjson_obj_get(root, "element")) {
        if (!lookup_element(root, &e, err, sizeof(err))) {
            pthread_mutex_unlock(&s_cache_lock);
            target_free(&t);
            return fail(out, cap, err, NULL);
        }
        focused = AXUIElementSetAttributeValue(e->ref, kAXFocusedAttribute, kCFBooleanTrue) == kAXErrorSuccess;
    } else {
        /* No element given: type into whatever has focus inside the app. */
        CFTypeRef f = NULL;
        if (AXUIElementCopyAttributeValue(t.app, kAXFocusedUIElementAttribute, &f) == kAXErrorSuccess && f &&
            CFGetTypeID(f) == AXUIElementGetTypeID() && s_count < APP_CONTROL_MAX_ELEMENTS && s_pid == t.pid &&
            describe(f, &s_elements[s_count]))
            e = &s_elements[s_count++];
        if (f) CFRelease(f);
    }
    CFStringRef s = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!s) { pthread_mutex_unlock(&s_cache_lock); target_free(&t); return fail(out, cap, "text_not_utf8", NULL); }
    CFIndex n = CFStringGetLength(s);
    /* Inserting at the caret through AX is exact and needs no key window. */
    Boolean selectable = false;
    if (e && AXUIElementIsAttributeSettable(e->ref, kAXSelectedTextAttribute, &selectable) == kAXErrorSuccess &&
        selectable && AXUIElementSetAttributeValue(e->ref, kAXSelectedTextAttribute, s) == kAXErrorSuccess) {
        CFRelease(s);
        usleep(50000);
        char value[VALUE_MAX + 1] = "";
        copy_text(e->ref, kAXValueAttribute, value, sizeof(value));
        jbuf_t b;
        jbuf_init(&b, 512);
        jbuf_append(&b, "{\"ok\":true,");
        append_target(&b, &t);
        jbuf_appendf(&b, ",\"characters\":%ld,\"delivery\":\"ax_insert_at_caret\",\"verified\":%s,\"value_after\":",
                     (long)n, strstr(value, text) ? "true" : "false");
        jbuf_append_json_str(&b, value);
        jbuf_append(&b, "}");
        pthread_mutex_unlock(&s_cache_lock);
        target_free(&t);
        return finish(&b, out, cap, true);
    }
    make_window_main(&t);
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStatePrivate);
    for (CFIndex i = 0; i < n; i++) {
        UniChar chars[2] = {CFStringGetCharacterAtIndex(s, i), 0};
        UniCharCount count = 1;
        if (CFStringIsSurrogateHighCharacter(chars[0]) && i + 1 < n &&
            CFStringIsSurrogateLowCharacter(CFStringGetCharacterAtIndex(s, i + 1))) {
            chars[1] = CFStringGetCharacterAtIndex(s, ++i);
            count = 2;
        }
        post_key(t.pid, src, 0, 0, chars, count);
        usleep(4000);
    }
    if (src) CFRelease(src);
    CFRelease(s);
    usleep(100000);
    char value[VALUE_MAX + 1] = "";
    if (e) copy_text(e->ref, kAXValueAttribute, value, sizeof(value));
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"ok\":true,");
    append_target(&b, &t);
    jbuf_appendf(&b, ",\"characters\":%ld,\"delivery\":\"posted_to_pid\"", (long)n);
    if (e) {
        jbuf_appendf(&b, ",\"element_focused\":%s,\"value_after\":", focused ? "true" : "false");
        jbuf_append_json_str(&b, value);
    }
    jbuf_append(&b, "}");
    pthread_mutex_unlock(&s_cache_lock);
    target_free(&t);
    return finish(&b, out, cap, true);
}

static bool title_matches(AXUIElementRef e, const char *want) {
    char title[LABEL_MAX + 1];
    if (!copy_text(e, kAXTitleAttribute, title, sizeof(title))) return false;
    size_t n = strlen(title);
    /* "Save As…" matches "Save As" and "Save As..." */
    if (n >= 3 && !strcmp(title + n - 3, "\xe2\x80\xa6")) title[n - 3] = '\0';
    char want_copy[LABEL_MAX + 1];
    snprintf(want_copy, sizeof(want_copy), "%s", want);
    size_t m = strlen(want_copy);
    if (m >= 3 && !strcmp(want_copy + m - 3, "...")) want_copy[m - 3] = '\0';
    else if (m >= 3 && !strcmp(want_copy + m - 3, "\xe2\x80\xa6")) want_copy[m - 3] = '\0';
    return !strcasecmp(title, want_copy);
}

static AXUIElementRef child_titled(AXUIElementRef parent, const char *want) {
    CFArrayRef kids = NULL;
    AXUIElementRef found = NULL;
    if (AXUIElementCopyAttributeValue(parent, kAXChildrenAttribute, (CFTypeRef *)&kids) == kAXErrorSuccess &&
        kids && CFGetTypeID(kids) == CFArrayGetTypeID()) {
        for (CFIndex i = 0; i < CFArrayGetCount(kids) && !found; i++) {
            AXUIElementRef k = (AXUIElementRef)CFArrayGetValueAtIndex(kids, i);
            if (CFGetTypeID(k) == AXUIElementGetTypeID() && title_matches(k, want))
                found = (AXUIElementRef)CFRetain(k);
        }
    }
    if (kids) CFRelease(kids);
    return found;
}

/* Menus hang an AXMenu between an item and its sub-items. */
static AXUIElementRef submenu_of(AXUIElementRef item) {
    CFArrayRef kids = NULL;
    AXUIElementRef menu = NULL;
    if (AXUIElementCopyAttributeValue(item, kAXChildrenAttribute, (CFTypeRef *)&kids) == kAXErrorSuccess &&
        kids && CFGetTypeID(kids) == CFArrayGetTypeID() && CFArrayGetCount(kids) > 0) {
        AXUIElementRef k = (AXUIElementRef)CFArrayGetValueAtIndex(kids, 0);
        if (CFGetTypeID(k) == AXUIElementGetTypeID()) menu = (AXUIElementRef)CFRetain(k);
    }
    if (kids) CFRelease(kids);
    return menu;
}

static bool action_menu(yyjson_val *root, char *out, size_t cap) {
    yyjson_val *path = yyjson_obj_get(root, "path");
    size_t depth = yyjson_arr_size(path);
    if (!yyjson_is_arr(path) || depth < 2 || depth > 8) return fail(out, cap, "path_required", "e.g. [\"File\",\"Save\"]");
    char err[160] = "";
    target_t t;
    if (!resolve_target(root, false, &t, err, sizeof(err))) return fail(out, cap, err, NULL);
    CFTypeRef bar = NULL;
    if (AXUIElementCopyAttributeValue(t.app, kAXMenuBarAttribute, &bar) != kAXErrorSuccess || !bar ||
        CFGetTypeID(bar) != AXUIElementGetTypeID()) {
        if (bar) CFRelease(bar);
        target_free(&t);
        return fail(out, cap, "menu_bar_unavailable", NULL);
    }
    AXUIElementRef cur = (AXUIElementRef)bar;
    for (size_t i = 0; i < depth; i++) {
        const char *want = yyjson_get_str(yyjson_arr_get(path, i));
        AXUIElementRef next = want ? child_titled(cur, want) : NULL;
        CFRelease(cur);
        if (!next) { target_free(&t); return fail(out, cap, "menu_item_not_found", want ? want : "(non-string)"); }
        if (i + 1 == depth) { cur = next; break; }
        cur = submenu_of(next);
        CFRelease(next);
        if (!cur) { target_free(&t); return fail(out, cap, "submenu_missing", want); }
    }
    bool enabled = copy_bool(cur, kAXEnabledAttribute, true);
    AXError rc = enabled ? AXUIElementPerformAction(cur, kAXPressAction) : kAXErrorCannotComplete;
    CFRelease(cur);
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_appendf(&b, "{\"ok\":%s,", rc == kAXErrorSuccess ? "true" : "false");
    append_target(&b, &t);
    jbuf_appendf(&b, ",\"menu_item_enabled\":%s,\"ax_error\":%d}", enabled ? "true" : "false", (int)rc);
    target_free(&t);
    return finish(&b, out, cap, rc == kAXErrorSuccess);
}

static bool action_screenshot(yyjson_val *root, char *out, size_t cap) {
    char err[160] = "";
    target_t t;
    if (!resolve_target(root, true, &t, err, sizeof(err))) return fail(out, cap, err, NULL);
    char path[64];
    int iw = 0, ih = 0;
    if (!capture_window(t.window_id, path, sizeof(path), &iw, &ih)) {
        target_free(&t);
        return fail(out, cap, "window_capture_failed", "needs Screen Recording permission and an on-screen window");
    }
    bool attached = tool_content_add_image_file(path, "image/jpeg");
    unlink(path);
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_appendf(&b, "{\"ok\":%s,", attached ? "true" : "false");
    append_target(&b, &t);
    jbuf_appendf(&b, ",\"screenshot_attached\":%s,\"image_size\":[%d,%d],"
                 "\"note\":\"image covers window_frame; scale image pixels by window size / image size\"}",
                 attached ? "true" : "false", iw, ih);
    target_free(&t);
    return finish(&b, out, cap, attached);
}

bool tool_app_control(const char *input_json, char *result, size_t result_len) {
    if (!result || result_len < 256) return false;
    yyjson_doc *doc = yyjson_read(input_json ? input_json : "{}", input_json ? strlen(input_json) : 2, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *action = yyjson_get_str(yyjson_obj_get(root, "action"));
    bool ok;
    if (!yyjson_is_obj(root) || !action) ok = fail(result, result_len, "action_required", NULL);
    else if (strcmp(action, "apps") && !AXIsProcessTrusted())
        ok = fail(result, result_len, "accessibility_permission_required",
                  "grant Accessibility to this terminal in System Settings > Privacy & Security");
    else if (!strcmp(action, "apps")) ok = action_apps(result, result_len);
    else if (!strcmp(action, "observe")) ok = action_observe(root, result, result_len);
    else if (!strcmp(action, "find")) ok = action_find(root, result, result_len);
    else if (!strcmp(action, "press")) ok = action_press(root, result, result_len);
    else if (!strcmp(action, "set_value")) ok = action_set_value(root, result, result_len);
    else if (!strcmp(action, "focus")) ok = action_focus(root, result, result_len);
    else if (!strcmp(action, "type")) ok = action_type(root, result, result_len);
    else if (!strcmp(action, "key")) ok = action_key(root, result, result_len);
    else if (!strcmp(action, "menu")) ok = action_menu(root, result, result_len);
    else if (!strcmp(action, "screenshot")) ok = action_screenshot(root, result, result_len);
    else ok = fail(result, result_len, "unknown_action", action);
    yyjson_doc_free(doc);
    return ok;
}

#else

bool tool_app_control(const char *input_json, char *result, size_t result_len) {
    (void)input_json;
    if (result && result_len) snprintf(result, result_len, "{\"ok\":false,\"error\":\"app_control_requires_macos\"}");
    return false;
}

#endif
