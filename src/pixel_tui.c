#define _POSIX_C_SOURCE 200809L

#include "pixel_tui.h"
#include "font_compat.h"
#include "json_util.h"
#include "kitty_graphics.h"
#include "native_composer.h"
#include "native_display.h"
#include "native_masthead.h"
#include "native_ui.h"
#include "native_windows.h"
#include "native_trace.h"
#include "native_trace_ui.h"
#include "pixel_fx.h"
#include "pixel_tui_perf.h"
#include "px_backend.h"
#include "px_theme.h"
#include "plan.h"
#include "plan_dag.h"
#include "rich_text.h"
#include "ui_motion.h"
#include "../vendor/yyjson.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <zlib.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint8_t r, g, b;
} px_color_t;
typedef struct {
    /* Layout is always expressed in logical pixels. The RGB transport uses
     * the exact backing dimensions so Kitty never has to interpolate text. */
    int width, height;
    int pixel_width, pixel_height;
    double backing_scale;
    px_color_t *pixels;
} px_canvas_t;

_Static_assert(sizeof(px_color_t) == 3, "Kitty RGB transport requires packed 24-bit pixels");

typedef struct {
    step_t *step;
    int depth;
    int parent;
    int y;
    int height;
} plan_node_t;

typedef struct {
    plan_node_t nodes[128];
    int count;
    int hidden;
} plan_layout_t;

typedef struct {
    atom_t *atom;
    step_t *step;
    int x;
    int y;
    int w;
    int h;
} plan_action_node_t;

typedef struct {
    plan_action_node_t nodes[128];
    int count;
    int hidden;
    int ready_ids[64];
    int ready_count;
    int wire_count;
} plan_action_layout_t;

/* Console palette, resolved through the active px_theme on every use so a
 * theme switch renders on the next frame with no surface-local state. The
 * classic names keep every draw site readable: C_CYAN/C_VIOLET are the
 * theme's primary/secondary accents, the tones map one-to-one. The default
 * quiet-console theme reproduces the original constants exactly. */
static inline px_color_t theme_px(px_backend_color_t c) {
    return (px_color_t){c.r, c.g, c.b};
}
#define C_BG_TOP theme_px(px_theme_active()->bg_top)
#define C_BG_BOTTOM theme_px(px_theme_active()->bg_bottom)
#define C_PANEL theme_px(px_theme_active()->panel)
#define C_PANEL_ALT theme_px(px_theme_active()->panel_alt)
#define C_TEXT theme_px(px_theme_active()->text)
#define C_DIM theme_px(px_theme_active()->dim)
#define C_CYAN theme_px(px_theme_active()->accent)
#define C_VIOLET theme_px(px_theme_active()->accent_alt)
#define C_GREEN theme_px(px_theme_active()->success)
#define C_AMBER theme_px(px_theme_active()->warning)
#define C_RED theme_px(px_theme_active()->danger)

typedef struct {
    char role[20];
    char detail[160];
    /* Hosted responses routinely exceed a small inline field. Grow only
     * messages that need it, with a firm per-event ceiling. */
    char *text;
    size_t text_len;
    size_t text_cap;
    int turn;
    uint64_t sequence;
    bool streaming;
    /* Gradual reveal: streamed assistant text surfaces at a bounded pace so
     * the current answer flows instead of jumping by whole network chunks.
     * reveal_len trails text_len; the animation thread closes the gap. */
    size_t reveal_len;
    bool reveal_pending;
    /* Compact tool row: one durable transcript line per governed tool call
     * instead of a call message plus a raw full-result dump. */
    bool tool_row;
    int8_t tool_status; /* -1 running, 0 error, 1 ok */
    float tool_elapsed_ms;
    uint32_t tool_tail_lines;  /* result lines beyond the inline preview */
    uint32_t tool_total_bytes; /* full result size before truncation */
    uint64_t tool_operation_id;
    char tool_name[48];
} pixel_message_t;

#define PIXEL_MESSAGE_CAP 512
#define PIXEL_MESSAGE_TEXT_MAX (128U * 1024U)
/* Live reasoning is a glanceable tail, not an archive; Chronicle keeps the
 * full trace. Heavy reasoners (gpt-6-astra) stream raw CoT far past what a
 * transcript can absorb, so retain roughly one screen of flowed prose. */
#define PIXEL_THINKING_TAIL_MAX 2048U
#define PIXEL_STREAM_MAILBOX_CAP PIXEL_MESSAGE_TEXT_MAX
#define PIXEL_COMMAND_CAP 160
#define PIXEL_TOOL_VIS_CAP 8
#define PIXEL_SWARM_VIS_CAP 12
#define PIXEL_TURN_VIS_CAP 10
#define PIXEL_COMPOSER_MENU_CAP 10
#define PIXEL_NOTICE_CAP 3
#define PIXEL_NOTICE_TTL_S 8.0
#define PIXEL_MODAL_ITEM_CAP 24

typedef struct {
    char command[48];
    char description[192];
} pixel_command_t;

typedef enum {
    PIXEL_OP_RUNNING = 0,
    PIXEL_OP_DONE,
    PIXEL_OP_ERROR,
} pixel_op_status_t;

typedef struct {
    bool used;
    uint64_t sequence;
    char name[64];
    char preview[128];
    pixel_op_status_t status;
    double started_s;
    double elapsed_ms;
} pixel_tool_visual_t;

typedef struct {
    bool used;
    int child_id;
    char status[20];
    char task[96];
    char model[64];
    size_t output_bytes;
    double cost_usd;
    double updated_s;
} pixel_swarm_visual_t;

typedef struct {
    int turn;
    unsigned phase_mask;
    int tool_count;
    int swarm_count;
    double started_s;
    double ended_s;
} pixel_turn_visual_t;

typedef struct {
    char label[128];
    char detail[192];
    bool disabled;
} pixel_composer_item_t;

typedef struct {
    pixel_tui_menu_kind_t kind;
    pixel_composer_item_t items[PIXEL_COMPOSER_MENU_CAP];
    int count;
    int selected;
} pixel_composer_menu_t;

typedef struct {
    bool used;
    pixel_tui_notice_level_t level;
    char text[200];
    double created_s;
    uint64_t sequence;
} pixel_notice_t;

typedef struct {
    bool active;
    pixel_tui_modal_kind_t kind;
    char title[128];
    char subtitle[256];
    char footer[256];
    pixel_composer_item_t items[PIXEL_MODAL_ITEM_CAP];
    int count;
    int selected;
} pixel_modal_t;

typedef struct {
    bool active;
    uint32_t image_ids[4];
    uint32_t generation;
    int image_widths[4];
    int image_heights[4];
    int cols;
    int rows;
    int width;
    int height;
    double backing_scale;
    int surface_width;
    int surface_height;
    pixel_tui_state_t state;
    char model[128];
    pixel_message_t messages[PIXEL_MESSAGE_CAP];
    int message_start;
    int message_count;
    int current_message;
    char input[4096];
    size_t input_cursor;
    bool input_active;
    double caret_epoch_s;
    pixel_composer_menu_t composer_menu;
    pixel_notice_t notices[PIXEL_NOTICE_CAP];
    uint64_t next_notice_sequence;
    pixel_modal_t modal;
    int transcript_scroll;
    /* Background/transcript and composer patches have independent latency
     * budgets. A fast editor patch must not postpone the next semantic frame. */
    double last_paint_s;
    double last_composer_paint_s;
    /* Cached underlay of the bounded live-tool deck, never a second scene. */
    uint8_t *activity_underlay;
    size_t activity_underlay_cap;
    native_ui_rect_t activity_rect;
    int activity_avail_h, activity_total, activity_shown;
    bool activity_compact, activity_valid;
    double last_activity_paint_s;
    double background_frame_cost_ema_ms;
    double stream_repaint_pending_since_s;
    double composer_repaint_pending_since_s;
    double started_s;
    double state_started_s;
    double turn_started_s;
    double cost_usd;
    double reported_cost_usd;
    double estimated_cost_usd;
    int reported_cost_samples;
    int estimated_cost_samples;
    int unpriced_responses;
    double context_percent;
    int input_tokens;
    int output_tokens;
    int tools_used;
    double budget_limit_usd;
    double budget_burn_rate;
    double budget_percent;
    double budget_runway_s;
    bool show_clock;
    char slot_name[64];
    int queue_depth;
    int queue_capacity;
    int turn;
    uint64_t next_sequence;
    size_t thinking_bytes;
    pixel_command_t commands[PIXEL_COMMAND_CAP];
    int command_count;
    bool command_help_active;
    pixel_tool_visual_t tool_visuals[PIXEL_TOOL_VIS_CAP];
    uint64_t next_tool_sequence;
    pixel_swarm_visual_t swarm_visuals[PIXEL_SWARM_VIS_CAP];
    pixel_turn_visual_t turn_visuals[PIXEL_TURN_VIS_CAP];
    int turn_visual_count;
    /* ui_render owns one retained scene; incidental overlays never own it. */
    char *scene_json;
    uint32_t scene_image_id;
    uint32_t scene_generation;
    int scene_col, scene_row, scene_cols, scene_rows;
    int scene_width, scene_height;
    bool scene_placed;
    bool scene_dirty;
    uint32_t overlay_image_id;
    int saved_stdout_fd;
    int saved_stderr_fd;
    int devnull_fd;
    int capture_read_fd;
    int capture_write_fd;
    pthread_t capture_thread;
    bool capture_thread_started;
    volatile bool capture_stop;
    bool capture_muted;
    int capture_message;
    FILE *tty_out;
    bool stdio_suppressed;
    bool terminal_suspended;
    bool animation_enabled;
    bool animation_stop;
    bool animation_thread_started;
    int animation_interval_ms;
    uint64_t animation_frame;
    pthread_t animation_thread;
    pixel_tui_state_t previous_state;
    double transition_started_s;
    /* Retained animation timeline: producers set targets, the compositor
     * thread samples values. Guarded by g_session_mutex like the rest. */
    ui_motion_t motion;
    /* Last frame actually uploaded for the placed image, for damage diffing.
     * When only small regions change, the compositor patches the resident
     * image via Kitty frame edits instead of re-encoding the whole screen. */
    uint8_t *prev_frame;
    size_t prev_frame_cap;
    uint8_t *patch_buffer;
    size_t patch_buffer_cap;
    int prev_frame_width;
    int prev_frame_height;
    uint32_t prev_frame_image;
    uint32_t patch_streak;
    bool patch_enabled;
    bool stream_repaint_pending;
    bool structural_repaint_pending;
    bool composer_repaint_pending;
    bool composer_fast_eligible;
    double reveal_last_s;
    pixel_tui_tool_view_t tool_view;
} pixel_session_t;

static pixel_session_t g_session;
static pthread_mutex_t g_session_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_animation_cond = PTHREAD_COND_INITIALIZER;

typedef struct {
    bool pending;
    char input[4096];
    size_t cursor;
    bool active;
    pixel_composer_menu_t menu;
} composer_mailbox_t;

typedef struct {
    bool pending;
    char text[PIXEL_STREAM_MAILBOX_CAP + 1U];
    size_t len;
    double pending_since_s;
} stream_mailbox_t;

/* Input publication is deliberately independent of g_session_mutex.  The
 * compositor may hold that lock for raster + terminal upload; a keystroke must
 * still be accepted immediately and collapse into the latest retained draft. */
static composer_mailbox_t g_composer_mailbox;
static pthread_mutex_t g_composer_mailbox_mutex = PTHREAD_MUTEX_INITIALIZER;
static stream_mailbox_t g_stream_mailbox;
static pthread_mutex_t g_stream_mailbox_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool g_session_active_fast = false;
static _Atomic bool g_session_suspended_fast = false;
static _Atomic bool g_animation_thread_fast = false;
static _Atomic bool g_composer_mailbox_pending = false;
static _Atomic bool g_stream_mailbox_pending = false;
static _Atomic bool g_composer_input_active = false;
static _Atomic bool g_composer_accepting_input = false;

static void *session_capture_thread_main(void *arg);
static void *session_animation_thread_main(void *arg);
static double monotonic_s(void);
static pixel_message_t *session_new_message(const char *role, const char *detail);
static px_color_t session_animated_accent(pixel_tui_state_t state);
static void draw_meter(px_canvas_t *c, int x, int y, int w, double percent, px_color_t color);
static px_canvas_t *render_scene_frame(const char *scene_json, int width, int requested_height);
static bool session_refresh_scene(FILE *out);
static void session_place_scene(FILE *out, bool reanchor);
static void free_canvas(px_canvas_t *c);
static void session_release_scene(FILE *out);

/* Preserve literal "\\u0000", but reject JSON escapes that decode to a C
 * string terminator before action/spec helpers could silently truncate them. */
static bool scene_json_has_nul(const char *p) {
    while (*p) {
        if (*p++ != '\\') continue;
        if (*p == 'u' && !strncmp(p + 1, "0000", 4)) return true;
        if (*p) p++;
    }
    return false;
}

static void session_lock(void) {
    /*
     * Retained session state stays single-owner; latency-sensitive producer
     * paths publish through their independently measured bounded mailboxes.
     */
    (void)pthread_mutex_lock(&g_session_mutex);
}

static void session_unlock(void) {
    (void)pthread_mutex_unlock(&g_session_mutex);
}

static FILE *session_output(FILE *fallback) {
    return g_session.active && g_session.tty_out ? g_session.tty_out : fallback;
}

static bool session_suppress_stdio(void) {
    if (!g_session.active || g_session.stdio_suppressed)
        return true;
    fflush(stdout);
    fflush(stderr);
    if (g_session.devnull_fd < 0)
        g_session.devnull_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (g_session.devnull_fd < 0)
        return false;
    int target = (!g_session.capture_muted && g_session.capture_write_fd >= 0)
                     ? g_session.capture_write_fd
                     : g_session.devnull_fd;
    if (dup2(target, STDOUT_FILENO) < 0 || dup2(target, STDERR_FILENO) < 0)
        return false;
    g_session.stdio_suppressed = true;
    return true;
}

static void session_capture_set_muted(bool muted) {
    if (!g_session.active || g_session.capture_muted == muted)
        return;
    fflush(stdout);
    fflush(stderr);
    g_session.capture_muted = muted;
    if (!g_session.stdio_suppressed)
        return;
    int target = (!muted && g_session.capture_write_fd >= 0) ? g_session.capture_write_fd
                                                             : g_session.devnull_fd;
    if (target >= 0) {
        (void)dup2(target, STDOUT_FILENO);
        (void)dup2(target, STDERR_FILENO);
    }
}

static bool session_capture_start(void) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return false;
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    g_session.capture_read_fd = fds[0];
    g_session.capture_write_fd = fds[1];
    g_session.capture_stop = false;
    if (pthread_create(&g_session.capture_thread, NULL, session_capture_thread_main, NULL) != 0) {
        close(fds[0]);
        close(fds[1]);
        g_session.capture_read_fd = -1;
        g_session.capture_write_fd = -1;
        return false;
    }
    g_session.capture_thread_started = true;
    return true;
}

static void session_restore_stdio(void) {
    if (!g_session.active || !g_session.stdio_suppressed)
        return;
    fflush(stdout);
    fflush(stderr);
    if (g_session.saved_stdout_fd >= 0)
        (void)dup2(g_session.saved_stdout_fd, STDOUT_FILENO);
    if (g_session.saved_stderr_fd >= 0)
        (void)dup2(g_session.saved_stderr_fd, STDERR_FILENO);
    g_session.stdio_suppressed = false;
}

static double clamp01(double x) {
    if (x < 0.0)
        return 0.0;
    if (x > 1.0)
        return 1.0;
    return x;
}

static px_color_t color_mix(px_color_t a, px_color_t b, double t) {
    t = clamp01(t);
    return (px_color_t){(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                        (uint8_t)(a.b + (b.b - a.b) * t)};
}

/* Map boundaries, not lengths: fractional-density neighbors share exactly
 * one raster edge even when a logical pixel occupies less than one sample. */
static int device_px(const px_canvas_t *c, double logical) {
    double scale = c && c->backing_scale > 0 ? c->backing_scale : 1.0;
    double pixel = floor(logical * scale);
    if (pixel <= INT32_MIN)
        return INT32_MIN;
    if (pixel >= INT32_MAX)
        return INT32_MAX;
    return (int)pixel;
}

static int device_span(const px_canvas_t *c, int origin, int extent) {
    int64_t span = (int64_t)device_px(c, (double)origin + extent) - device_px(c, origin);
    return span < 0 ? 0 : span > INT32_MAX ? INT32_MAX : (int)span;
}

static int logical_advance(const px_canvas_t *c, int pixels) {
    double scale = c && c->backing_scale > 0 ? c->backing_scale : 1.0;
    double advance = ceil((double)pixels / scale);
    return advance >= INT32_MAX ? INT32_MAX : (int)advance;
}

static void put_pixel(px_canvas_t *c, int x, int y, px_color_t color, double alpha) {
    if (!c || !c->pixels || (unsigned)x >= (unsigned)c->width || (unsigned)y >= (unsigned)c->height)
        return;
    int px = device_px(c, x), py = device_px(c, y);
    int end_x = device_px(c, (double)x + 1), end_y = device_px(c, (double)y + 1);
    if (end_x > c->pixel_width) end_x = c->pixel_width;
    if (end_y > c->pixel_height) end_y = c->pixel_height;
    if (px >= end_x || py >= end_y)
        return;
    for (int yy = py; yy < end_y; yy++) {
        px_color_t *row = c->pixels + (size_t)yy * (size_t)c->pixel_width + (size_t)px;
        for (int xx = px; xx < end_x; xx++, row++)
            *row = color_mix(*row, color, alpha);
    }
}

static void fill_rect(px_canvas_t *c, int x, int y, int w, int h, px_color_t color, double alpha) {
    if (!c || !c->pixels || w <= 0 || h <= 0)
        return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int64_t right = (int64_t)x + w, bottom = (int64_t)y + h;
    int x1 = right > c->width ? c->width : right < 0 ? 0 : (int)right;
    int y1 = bottom > c->height ? c->height : bottom < 0 ? 0 : (int)bottom;
    if (x1 <= x0 || y1 <= y0)
        return;
    int px0 = device_px(c, x0), py0 = device_px(c, y0);
    int px1 = device_px(c, x1), py1 = device_px(c, y1);
    if (px1 > c->pixel_width)
        px1 = c->pixel_width;
    if (py1 > c->pixel_height)
        py1 = c->pixel_height;
    if (px1 <= px0 || py1 <= py0)
        return;
    for (int yy = py0; yy < py1; yy++) {
        px_color_t *row = c->pixels + (size_t)yy * (size_t)c->pixel_width + (size_t)px0;
        for (int xx = px0; xx < px1; xx++, row++)
            *row = color_mix(*row, color, alpha);
    }
}

/* Bridge the local canvas/color types onto the shared effects core. The
 * layouts are identical (packed 24-bit RGB); pixel_fx adds AA rounded
 * geometry, shadows, gradients, and blur on the same memory. */
static pixel_fx_surface_t fx_surface(px_canvas_t *c) {
    pixel_fx_surface_t s;
    pixel_fx_surface_init(&s, (uint8_t *)c->pixels, c->pixel_width, c->pixel_height);
    return s;
}

static pixel_fx_rgb_t fx_color(px_color_t color) {
    return (pixel_fx_rgb_t){color.r, color.g, color.b};
}

static void fill_rounded(px_canvas_t *c, int x, int y, int w, int h, int radius, px_color_t color,
                         double alpha) {
    pixel_fx_surface_t s = fx_surface(c);
    pixel_fx_fill_rounded(&s, device_px(c, x), device_px(c, y), device_span(c, x, w),
                          device_span(c, y, h), device_px(c, radius),
                          fx_color(color), alpha);
}

static void stroke_rounded(px_canvas_t *c, int x, int y, int w, int h, int radius, px_color_t color,
                           double alpha) {
    pixel_fx_surface_t s = fx_surface(c);
    pixel_fx_stroke_rounded(&s, device_px(c, x), device_px(c, y), device_span(c, x, w),
                            device_span(c, y, h), device_px(c, radius), device_px(c, 1),
                            fx_color(color), alpha);
}

/* Elevated panel: soft shadow under an AA rounded surface with a hairline
 * border. This is the standard ground for every session card. */
static void draw_panel(px_canvas_t *c, int x, int y, int w, int h, int radius, px_color_t fill,
                       double fill_alpha) {
    pixel_fx_surface_t s = fx_surface(c);
    int px = device_px(c, x), py = device_px(c, y);
    int pw = device_span(c, x, w), ph = device_span(c, y, h), pr = device_px(c, radius);
    pixel_fx_shadow(&s, px, py, pw, ph, pr, device_px(c, 14),
                    device_px(c, 3), (pixel_fx_rgb_t){0, 0, 0}, 0.42);
    pixel_fx_fill_rounded(&s, px, py, pw, ph, pr,
                          fx_color(fill), fill_alpha);
    pixel_fx_stroke_rounded(&s, px, py, pw, ph, pr, device_px(c, 1),
                            fx_color(C_DIM), 0.26);
}

/* ── timeline identities ─────────────────────────────────────────────────
 * Stable (key, prop) pairs for the retained motion timeline. Message and
 * tool tracks derive keys from their sequence numbers. */
enum {
    MOTION_PROP_VALUE = 0,    /* eased scalar (meters) */
    MOTION_PROP_ENTRANCE = 1, /* 0 → 1 arrival of a new object */
    MOTION_PROP_FLASH = 2,    /* 1 → 0 attention decay */
};
#define MOTION_KEY_CONTEXT UINT64_C(0x110)
#define MOTION_KEY_QUEUE UINT64_C(0x111)
#define MOTION_KEY_MESSAGE(sequence) (UINT64_C(0x4d000000) + (sequence))
#define MOTION_KEY_TOOL(sequence) (UINT64_C(0x54000000) + (sequence))

static double session_motion_value(uint64_t key, uint16_t prop, double fallback) {
    return ui_motion_value(&g_session.motion, key, prop, monotonic_s(), fallback);
}

static double smoothstep(double t) {
    t = clamp01(t);
    return t * t * (3.0 - 2.0 * t);
}

static double motion_phase(double period_s, double offset_s) {
    if (!g_session.active || !g_session.animation_enabled || period_s <= 0.0)
        return 0.0;
    double phase = fmod(monotonic_s() + offset_s, period_s) / period_s;
    return phase < 0.0 ? phase + 1.0 : phase;
}

static double session_caret_phase(double now_s) {
    double elapsed = now_s - g_session.caret_epoch_s;
    return fmod(elapsed > 0.0 ? elapsed : 0.0, 1.0);
}

static bool session_caret_visible(double now_s) {
    return !g_session.animation_enabled || session_caret_phase(now_s) < 0.62;
}

static double motion_pulse(double period_s, double offset_s) {
    double phase = motion_phase(period_s, offset_s);
    return 0.5 - 0.5 * cos(phase * 6.28318530717958647692);
}

static void draw_motion_sweep(px_canvas_t *c, int x, int y, int w, int h, px_color_t color,
                              double period_s, double alpha) {
    if (!c || w < 12 || h < 1 || !g_session.animation_enabled)
        return;
    int radius = w / 7;
    if (radius < 12)
        radius = 12;
    if (radius > 90)
        radius = 90;
    double phase = motion_phase(period_s, 0.0);
    int center = x - radius + (int)((double)(w + radius * 2) * phase);
    for (int xx = center - radius; xx <= center + radius; xx += 2) {
        double distance = fabs((double)(xx - center)) / (double)radius;
        double strength = 1.0 - clamp01(distance);
        if (xx >= x && xx < x + w)
            fill_rect(c, xx, y, xx + 2 <= x + w ? 2 : 1, h, color,
                      alpha * smoothstep(strength));
    }
}

static void draw_line(px_canvas_t *c, int x0, int y0, int x1, int y1, px_color_t color,
                      double alpha) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_pixel(c, x0, y0, color, alpha);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void fill_circle(px_canvas_t *c, int cx, int cy, int radius, px_color_t color,
                        double alpha) {
    if (!c || radius < 1)
        return;
    int rr = radius * radius;
    for (int y = -radius; y <= radius; y++) {
        int span = (int)sqrt((double)(rr - y * y));
        fill_rect(c, cx - span, cy + y, span * 2 + 1, 1, color, alpha);
    }
}

static void draw_circle_ring(px_canvas_t *c, int cx, int cy, int radius, int thickness,
                             px_color_t color, double alpha) {
    if (!c || radius < 2 || thickness < 1)
        return;
    int outer = radius * radius;
    int inner_r = radius - thickness;
    int inner = inner_r * inner_r;
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            int d = x * x + y * y;
            if (d <= outer && d >= inner)
                put_pixel(c, cx + x, cy + y, color, alpha);
        }
    }
}

/* One arc of a ring, angles in radians, antialiased by subpixel sampling. */
static void draw_ring_arc(px_canvas_t *c, int cx, int cy, int radius, double a0, double a1,
                          px_color_t color, double alpha) {
    if (!c || radius < 2 || a1 <= a0)
        return;
    double step = 0.9 / (double)radius;
    for (double a = a0; a <= a1; a += step) {
        double x = (double)cx + cos(a) * (double)radius;
        double y = (double)cy + sin(a) * (double)radius;
        put_pixel(c, (int)(x + 0.5), (int)(y + 0.5), color, alpha);
        put_pixel(c, (int)x, (int)y, color, alpha * 0.5);
    }
}

/* Overmind Soul mark: the core is the active agent, the paired upper vanes are
 * Wings, the three lower points are Talons, and the segmented outer ring is the
 * Immune System. State changes alter the core topology; idle remains static. */
static void draw_dsco_soul(px_canvas_t *c, int cx, int cy, int radius, pixel_tui_state_t state) {
    if (!c || radius < 10)
        return;
    px_color_t accent = session_animated_accent(state);
    double live = state == PIXEL_TUI_IDLE ? 0.0 : motion_pulse(0.92, 0.0);

    /* Immune System: the outer ring is real telemetry, one segment per tool
     * slot. Running work pulses its arc, completions cool to green and fade
     * over a few seconds, failures hold red. With no tool activity the ring
     * closes back into an unbroken faint circle. */
    bool segmented = false;
    if (g_session.active) {
        double now = monotonic_s();
        const double slot_span = 6.28318530717958647692 / PIXEL_TOOL_VIS_CAP;
        const double gap = slot_span * 0.16;
        for (int i = 0; i < PIXEL_TOOL_VIS_CAP; i++) {
            const pixel_tool_visual_t *tool = &g_session.tool_visuals[i];
            if (!tool->used)
                continue;
            double a0 = -1.5707963267948966 + slot_span * i + gap * 0.5;
            double a1 = a0 + slot_span - gap;
            px_color_t arc = C_AMBER;
            double arc_alpha = 0.0;
            if (tool->status == PIXEL_OP_RUNNING) {
                arc_alpha = 0.55 + motion_pulse(1.1, i * 0.13) * 0.35;
            } else {
                double age = now - (tool->started_s + tool->elapsed_ms / 1000.0);
                arc = tool->status == PIXEL_OP_DONE ? C_GREEN : C_RED;
                arc_alpha =
                    (tool->status == PIXEL_OP_ERROR ? 0.85 : 0.70) - (age > 0.0 ? age * 0.12 : 0.0);
            }
            if (arc_alpha <= 0.04)
                continue;
            draw_ring_arc(c, cx, cy, radius, a0, a1, arc, arc_alpha);
            segmented = true;
        }
    }
    if (!segmented)
        draw_circle_ring(c, cx, cy, radius, 1, C_DIM, 0.36);
    else
        draw_circle_ring(c, cx, cy, radius, 1, C_DIM, 0.14);

    /* Wings: two quiet vanes carrying the core into the upper field. */
    draw_line(c, cx - 3, cy - 2, cx - radius + 3, cy - radius / 2, C_CYAN, 0.54);
    draw_line(c, cx - 2, cy - 5, cx - radius / 2, cy - radius + 3, C_CYAN, 0.32);
    draw_line(c, cx + 3, cy - 2, cx + radius - 3, cy - radius / 2, C_CYAN, 0.54);
    draw_line(c, cx + 2, cy - 5, cx + radius / 2, cy - radius + 3, C_CYAN, 0.32);

    /* Talons: three execution points below the decision core. */
    int talon_y = cy + radius - 4;
    draw_line(c, cx, cy + 3, cx, talon_y, C_AMBER, 0.54);
    draw_line(c, cx - 2, cy + 3, cx - radius / 2, talon_y - 2, C_AMBER, 0.40);
    draw_line(c, cx + 2, cy + 3, cx + radius / 2, talon_y - 2, C_AMBER, 0.40);
    fill_circle(c, cx - radius / 2, talon_y - 2, 1, C_AMBER, 0.82);
    fill_circle(c, cx, talon_y, 1, C_AMBER, 0.82);
    fill_circle(c, cx + radius / 2, talon_y - 2, 1, C_AMBER, 0.82);

    int core = state == PIXEL_TUI_EXECUTING ? 4 : 3;
    fill_circle(c, cx, cy, core + (int)(live * 1.4), accent, 0.82);
    draw_circle_ring(c, cx, cy, core + 3, 1, accent, 0.46 + live * 0.28);
    if (state == PIXEL_TUI_REASONING) {
        fill_circle(c, cx - 6, cy, 1, accent, 0.74);
        fill_circle(c, cx + 6, cy, 1, accent, 0.74);
    } else if (state == PIXEL_TUI_EXECUTING) {
        fill_rect(c, cx - 1, cy - radius + 2, 3, 3, accent, 0.86);
    } else if (state == PIXEL_TUI_RESPONDING) {
        draw_line(c, cx + core + 3, cy, cx + radius - 2, cy, accent, 0.78);
    }
}

static void canvas_background(px_canvas_t *c, uint32_t seed) {
    (void)seed;
    for (int y = 0; y < c->pixel_height; y++) {
        double v = (double)y / (double)(c->pixel_height > 1 ? c->pixel_height - 1 : 1);
        px_color_t row = color_mix(C_BG_TOP, C_BG_BOTTOM, v);
        for (int x = 0; x < c->pixel_width; x++)
            c->pixels[(size_t)y * (size_t)c->pixel_width + (size_t)x] = row;
    }
}

/* Paint a vertical slice of the session background into a small device-scale
 * canvas.  This is byte-identical to canvas_background() at the corresponding
 * full-surface rows and lets a retained region start from a clean backing
 * instead of alpha-blending new text over the previous composer. */
static void canvas_background_slice(px_canvas_t *c, int physical_y, int full_pixel_height) {
    if (!c || !c->pixels)
        return;
    int denominator = full_pixel_height > 1 ? full_pixel_height - 1 : 1;
    for (int y = 0; y < c->pixel_height; y++) {
        int source_y = physical_y + y;
        if (source_y < 0)
            source_y = 0;
        if (source_y >= full_pixel_height)
            source_y = full_pixel_height - 1;
        double v = (double)source_y / (double)denominator;
        px_color_t row = color_mix(C_BG_TOP, C_BG_BOTTOM, v);
        for (int x = 0; x < c->pixel_width; x++)
            c->pixels[(size_t)y * (size_t)c->pixel_width + (size_t)x] = row;
    }
}

#define GLYPH(a, b, c, d, e, f, g)                                                                 \
    (((uint64_t)(a) << 30) | ((uint64_t)(b) << 25) | ((uint64_t)(c) << 20) |                       \
     ((uint64_t)(d) << 15) | ((uint64_t)(e) << 10) | ((uint64_t)(f) << 5) | (g))
static uint64_t glyph_bits(char ch) {
    switch ((unsigned char)toupper((unsigned char)ch)) {
        case 'A':
            return GLYPH(14, 17, 17, 31, 17, 17, 17);
        case 'B':
            return GLYPH(30, 17, 17, 30, 17, 17, 30);
        case 'C':
            return GLYPH(14, 17, 16, 16, 16, 17, 14);
        case 'D':
            return GLYPH(30, 17, 17, 17, 17, 17, 30);
        case 'E':
            return GLYPH(31, 16, 16, 30, 16, 16, 31);
        case 'F':
            return GLYPH(31, 16, 16, 30, 16, 16, 16);
        case 'G':
            return GLYPH(14, 17, 16, 23, 17, 17, 15);
        case 'H':
            return GLYPH(17, 17, 17, 31, 17, 17, 17);
        case 'I':
            return GLYPH(14, 4, 4, 4, 4, 4, 14);
        case 'J':
            return GLYPH(1, 1, 1, 1, 17, 17, 14);
        case 'K':
            return GLYPH(17, 18, 20, 24, 20, 18, 17);
        case 'L':
            return GLYPH(16, 16, 16, 16, 16, 16, 31);
        case 'M':
            return GLYPH(17, 27, 21, 21, 17, 17, 17);
        case 'N':
            return GLYPH(17, 25, 21, 19, 17, 17, 17);
        case 'O':
            return GLYPH(14, 17, 17, 17, 17, 17, 14);
        case 'P':
            return GLYPH(30, 17, 17, 30, 16, 16, 16);
        case 'Q':
            return GLYPH(14, 17, 17, 17, 21, 18, 13);
        case 'R':
            return GLYPH(30, 17, 17, 30, 20, 18, 17);
        case 'S':
            return GLYPH(15, 16, 16, 14, 1, 1, 30);
        case 'T':
            return GLYPH(31, 4, 4, 4, 4, 4, 4);
        case 'U':
            return GLYPH(17, 17, 17, 17, 17, 17, 14);
        case 'V':
            return GLYPH(17, 17, 17, 17, 17, 10, 4);
        case 'W':
            return GLYPH(17, 17, 17, 21, 21, 21, 10);
        case 'X':
            return GLYPH(17, 17, 10, 4, 10, 17, 17);
        case 'Y':
            return GLYPH(17, 17, 10, 4, 4, 4, 4);
        case 'Z':
            return GLYPH(31, 1, 2, 4, 8, 16, 31);
        case '0':
            return GLYPH(14, 17, 19, 21, 25, 17, 14);
        case '1':
            return GLYPH(4, 12, 4, 4, 4, 4, 14);
        case '2':
            return GLYPH(14, 17, 1, 2, 4, 8, 31);
        case '3':
            return GLYPH(30, 1, 1, 14, 1, 1, 30);
        case '4':
            return GLYPH(2, 6, 10, 18, 31, 2, 2);
        case '5':
            return GLYPH(31, 16, 16, 30, 1, 1, 30);
        case '6':
            return GLYPH(14, 16, 16, 30, 17, 17, 14);
        case '7':
            return GLYPH(31, 1, 2, 4, 8, 8, 8);
        case '8':
            return GLYPH(14, 17, 17, 14, 17, 17, 14);
        case '9':
            return GLYPH(14, 17, 17, 15, 1, 1, 14);
        case ':':
            return GLYPH(0, 4, 4, 0, 4, 4, 0);
        case '.':
            return GLYPH(0, 0, 0, 0, 0, 4, 4);
        case '/':
            return GLYPH(1, 2, 2, 4, 8, 8, 16);
        case '-':
            return GLYPH(0, 0, 0, 31, 0, 0, 0);
        case '+':
            return GLYPH(0, 4, 4, 31, 4, 4, 0);
        case '#':
            return GLYPH(10, 31, 10, 10, 31, 10, 0);
        case '[':
            return GLYPH(14, 8, 8, 8, 8, 8, 14);
        case ']':
            return GLYPH(14, 2, 2, 2, 2, 2, 14);
        case '(':
            return GLYPH(2, 4, 8, 8, 8, 4, 2);
        case ')':
            return GLYPH(8, 4, 2, 2, 2, 4, 8);
        case '_':
            return GLYPH(0, 0, 0, 0, 0, 0, 31);
        case '=':
            return GLYPH(0, 31, 0, 31, 0, 0, 0);
        case '?':
            return GLYPH(14, 17, 1, 2, 4, 0, 4);
        case '!':
            return GLYPH(4, 4, 4, 4, 4, 0, 4);
        case ',':
            return GLYPH(0, 0, 0, 0, 0, 4, 8);
        case ';':
            return GLYPH(0, 4, 4, 0, 4, 4, 8);
        case '|':
            return GLYPH(4, 4, 4, 4, 4, 4, 4);
        case '<':
            return GLYPH(2, 4, 8, 16, 8, 4, 2);
        case '>':
            return GLYPH(8, 4, 2, 1, 2, 4, 8);
        case '%':
            return GLYPH(17, 2, 4, 8, 16, 17, 0);
        case '$':
            return GLYPH(4, 15, 20, 14, 5, 30, 4);
        case '@':
            return GLYPH(14, 17, 23, 21, 23, 16, 14);
        case '*':
            return GLYPH(0, 21, 14, 31, 14, 21, 0);
        case '\\':
            return GLYPH(16, 8, 8, 4, 2, 2, 1);
        default:
            return 0;
    }
}

static float text_point_size(int scale) {
    return scale >= 3 ? 22.0f : (scale == 2 ? 15.0f : 10.5f);
}

static size_t utf8_char_bytes(const char *s) {
    if (!s || !*s)
        return 0;
    unsigned char c = (unsigned char)*s;
    if (c < 0x80)
        return 1;
    if ((c & 0xe0) == 0xc0 && ((unsigned char)s[1] & 0xc0) == 0x80)
        return 2;
    if ((c & 0xf0) == 0xe0 && ((unsigned char)s[1] & 0xc0) == 0x80 &&
        ((unsigned char)s[2] & 0xc0) == 0x80)
        return 3;
    if ((c & 0xf8) == 0xf0 && ((unsigned char)s[1] & 0xc0) == 0x80 &&
        ((unsigned char)s[2] & 0xc0) == 0x80 && ((unsigned char)s[3] & 0xc0) == 0x80)
        return 4;
    return 1;
}

static int draw_text(px_canvas_t *c, int x, int y, int scale, const char *text, px_color_t color,
                     double alpha, int max_width) {
    int origin = x;
    if (!text || scale < 1)
        return 0;
    double backing = c->backing_scale > 0 ? c->backing_scale : 1.0;
    int native_advance =
        font_compat_draw_rgb((uint8_t *)c->pixels, c->pixel_width, c->pixel_height,
                             c->pixel_width * (int)sizeof(px_color_t), device_px(c, x), device_px(c, y),
                             device_span(c, x, max_width > 0 ? max_width : c->width - x), text,
                             text_point_size(scale) * (float)backing, false, color.r, color.g,
                             color.b, (float)clamp01(alpha));
    if (native_advance >= 0)
        return logical_advance(c, native_advance);
    for (; *text; text++) {
        if (max_width > 0 && x - origin + 5 * scale > max_width)
            break;
        uint64_t bits = glyph_bits(*text);
        for (int gy = 0; gy < 7; gy++) {
            for (int gx = 0; gx < 5; gx++) {
                if ((bits >> ((6 - gy) * 5 + (4 - gx))) & 1U)
                    fill_rect(c, x + gx * scale, y + gy * scale, scale, scale, color, alpha);
            }
        }
        x += 6 * scale;
    }
    return x - origin;
}

static void draw_text_ellipsis(px_canvas_t *c, int x, int y, int scale, const char *text,
                               px_color_t color, double alpha, int max_width) {
    if (!text || max_width < 6 * scale)
        return;
    if (font_compat_available()) {
        /* CoreText clips at a glyph boundary and preserves shaped Unicode.
         * Byte-count truncation would split UTF-8 and destroy fallback runs. */
        draw_text(c, x, y, scale, text, color, alpha, max_width);
        return;
    }
    int max_chars = max_width / (6 * scale);
    size_t len = strlen(text);
    if ((int)len <= max_chars) {
        draw_text(c, x, y, scale, text, color, alpha, max_width);
        return;
    }
    char buf[256];
    int keep = max_chars - 3;
    if (keep < 1)
        keep = 1;
    if (keep > (int)sizeof(buf) - 4)
        keep = (int)sizeof(buf) - 4;
    memcpy(buf, text, (size_t)keep);
    memcpy(buf + keep, "...", 4);
    draw_text(c, x, y, scale, buf, color, alpha, max_width);
}

/* Native controls use the same proportional family as conversation. Keep
 * monospace in the editor, code and numeric measurements. */
static void draw_ui_label(px_canvas_t *c, int x, int y, float size, bool bold,
                           const char *text, px_color_t color, double alpha, int max_width) {
    if (!text || !*text || max_width < 4) return;
    char clipped[768];
    size_t n = strlen(text);
    if (n > sizeof(clipped) - 4) n = sizeof(clipped) - 4;
    while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    memcpy(clipped, text, n);
    clipped[n] = '\0';
    if (font_compat_measure_prose_utf8(clipped, size, bold, false) > max_width - 3 || text[n]) {
        do {
            if (!n) break;
            n--;
            while (n && ((unsigned char)clipped[n] & 0xc0) == 0x80) n--;
            memcpy(clipped + n, "…", 4);
        } while (font_compat_measure_prose_utf8(clipped, size, bold, false) > max_width - 3);
    }
    double backing = c->backing_scale > 0 ? c->backing_scale : 1;
    int drawn = font_compat_draw_prose_rgb((uint8_t *)c->pixels, c->pixel_width, c->pixel_height,
        c->pixel_width * (int)sizeof(px_color_t), device_px(c, x), device_px(c, y),
        device_span(c, x, max_width), clipped, size * (float)backing, bold, false,
        color.r, color.g, color.b, (float)alpha);
    if (drawn < 0) draw_text_ellipsis(c, x, y, 1, text, color, alpha, max_width);
}

static px_color_t status_color(plan_status_t status) {
    switch (status) {
        case PLAN_DONE:
            return C_GREEN;
        case PLAN_IN_PROGRESS:
            return C_CYAN;
        case PLAN_BLOCKED:
            return C_AMBER;
        case PLAN_FAILED:
        case PLAN_CANCELLED:
            return C_RED;
        case PLAN_SKIPPED:
            return C_DIM;
        default:
            return C_VIOLET;
    }
}

static void collect_step(plan_layout_t *layout, int step_id, int depth, int parent) {
    if (!layout || depth > 12)
        return;
    step_t *step = step_get(step_id);
    if (!step)
        return;
    if (layout->count >= (int)(sizeof(layout->nodes) / sizeof(layout->nodes[0]))) {
        layout->hidden++;
        return;
    }
    int at = layout->count++;
    layout->nodes[at] = (plan_node_t){step, depth, parent, 0, 0};
    for (int i = 0; i < step->child_step_count; i++)
        collect_step(layout, step->child_step_ids[i], depth + 1, at);
}

static void build_layout(plan_t *plan, plan_layout_t *layout) {
    memset(layout, 0, sizeof(*layout));
    for (int i = 0; i < plan->root_step_count; i++)
        collect_step(layout, plan->root_step_ids[i], 0, -1);
}

static int node_height(const step_t *step) {
    int atoms = step ? step->atom_count : 0;
    if (atoms > 3)
        atoms = 3;
    return 58 + atoms * 18;
}

static void draw_chip(px_canvas_t *c, int x, int y, const char *label, px_color_t color) {
    int width = (int)strlen(label) * 6 + 12;
    fill_rounded(c, x, y, width, 16, 7, color, 0.12);
    stroke_rounded(c, x, y, width, 16, 7, color, 0.30);
    fill_rounded(c, x + 2, y + 4, 2, 8, 1, color, 0.80);
    draw_text(c, x + 8, y + 4, 1, label, color, 0.95, width - 10);
}

static void draw_node(px_canvas_t *c, const plan_node_t *node, int ordinal) {
    const step_t *step = node->step;
    int x = 48 + node->depth * 48;
    int y = node->y;
    int w = c->width - x - 34;
    px_color_t accent = status_color(step->status);
    fill_rect(c, x, y, w, node->height, C_PANEL, 0.93);
    fill_rect(c, x, y, 2, node->height, accent, 0.72);
    fill_rect(c, x, y, w, 1, C_DIM, 0.22);
    fill_rect(c, x, y + node->height - 1, w, 1, C_DIM, 0.14);

    char num[16];
    snprintf(num, sizeof(num), "%02d", ordinal + 1);
    draw_text(c, x + 15, y + 14, 2, num, accent, 0.95, 28);
    draw_text_ellipsis(c, x + 62, y + 12, 2, step->title ? step->title : "UNTITLED", C_TEXT, 0.96,
                       w - 140);

    const char *type = step_type_name(step->type);
    const char *status = plan_status_name(step->status);
    int status_w = (int)strlen(status) * 6 + 12;
    draw_chip(c, x + w - status_w - 12, y + 10, status, accent);
    draw_chip(c, x + 62, y + 37, type, C_DIM);

    int atom_y = y + 58;
    int shown = step->atom_count < 3 ? step->atom_count : 3;
    for (int i = 0; i < shown; i++) {
        atom_t *atom = atom_get(step->atom_ids[i]);
        if (!atom)
            continue;
        px_color_t atom_color = status_color(atom->status);
        fill_rect(c, x + 62, atom_y + i * 18, w - 78, 14, C_PANEL_ALT, 0.82);
        fill_rect(c, x + 67, atom_y + 4 + i * 18, 5, 5, atom_color, 0.9);
        draw_text_ellipsis(c, x + 80, atom_y + 3 + i * 18, 1,
                           atom->title ? atom->title : atom_type_name(atom->type), C_DIM, 0.90,
                           w - 102);
    }
    if (step->atom_count > shown) {
        char extra[32];
        snprintf(extra, sizeof(extra), "+%d ATOMS", step->atom_count - shown);
        draw_text(c, x + w - 70, y + node->height - 12, 1, extra, C_VIOLET, 0.9, 66);
    }
}

/* ── Canvas pool ──────────────────────────────────────────────────────────
 * Frames previously heap-allocated a ~2.9 MB pixel slab per render
 * (struct calloc + pixel malloc, freed in free_canvas). Render paths always
 * begin with canvas_background (full repaint), so slabs are safely reusable.
 * Two slots cover the concurrent renderers (session animation thread + plan
 * path); exhaustion falls back to plain heap so behavior degrades to the
 * old cost model instead of failing. */
#define CANVAS_POOL_SLOTS 2
typedef struct {
    px_canvas_t canvas;
    size_t cap; /* allocated bytes in canvas.pixels */
    bool in_use;
} canvas_slot_t;
static canvas_slot_t g_canvas_pool[CANVAS_POOL_SLOTS];
static pthread_mutex_t g_canvas_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

static px_canvas_t *canvas_acquire_device(int width, int height, int pixel_width, int pixel_height,
                                          double backing_scale) {
    if (width <= 0 || height <= 0 || pixel_width <= 0 || pixel_height <= 0 ||
        !isfinite(backing_scale) || backing_scale <= 0.0)
        return NULL;
    double raster_width = floor((double)width * backing_scale);
    double raster_height = floor((double)height * backing_scale);
    if (raster_width > INT32_MAX || raster_height > INT32_MAX)
        return NULL;
    if (pixel_width < raster_width || pixel_height < raster_height)
        return NULL;
    if ((size_t)pixel_width > SIZE_MAX / sizeof(px_color_t) / (size_t)pixel_height)
        return NULL;
    size_t need = (size_t)pixel_width * (size_t)pixel_height * sizeof(px_color_t);
    (void)pthread_mutex_lock(&g_canvas_pool_mutex);
    for (int i = 0; i < CANVAS_POOL_SLOTS; i++) {
        canvas_slot_t *s = &g_canvas_pool[i];
        if (s->in_use)
            continue;
        if (s->cap < need) {
            px_color_t *grown = realloc(s->canvas.pixels, need);
            if (!grown)
                continue; /* try next slot or fall through */
            s->canvas.pixels = grown;
            s->cap = need;
        }
        s->in_use = true;
        s->canvas.width = width;
        s->canvas.height = height;
        s->canvas.pixel_width = pixel_width;
        s->canvas.pixel_height = pixel_height;
        s->canvas.backing_scale = backing_scale;
        (void)pthread_mutex_unlock(&g_canvas_pool_mutex);
        return &s->canvas;
    }
    (void)pthread_mutex_unlock(&g_canvas_pool_mutex);
    /* Pool exhausted: legacy per-frame heap path (released in free_canvas). */
    px_canvas_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->width = width;
    c->height = height;
    c->pixel_width = pixel_width;
    c->pixel_height = pixel_height;
    c->backing_scale = backing_scale;
    c->pixels = malloc(need);
    if (!c->pixels) {
        free(c);
        return NULL;
    }
    return c;
}

static px_canvas_t *canvas_acquire_scaled(int width, int height, double backing_scale) {
    if (width <= 0 || height <= 0 || !isfinite(backing_scale) || backing_scale <= 0.0)
        return NULL;
    double pixel_width = fmax(1.0, ceil((double)width * backing_scale));
    double pixel_height = fmax(1.0, ceil((double)height * backing_scale));
    if (pixel_width > INT32_MAX || pixel_height > INT32_MAX)
        return NULL;
    return canvas_acquire_device(width, height, (int)pixel_width, (int)pixel_height,
                                 backing_scale);
}

static px_canvas_t *canvas_acquire(int width, int height) {
    return canvas_acquire_scaled(width, height, 1);
}

static px_canvas_t *render_plan_frame(int plan_id, int requested_width, plan_layout_t *out_layout) {
    plan_t *plan = plan_get(plan_id);
    if (!plan)
        return NULL;
    plan_layout_t layout;
    build_layout(plan, &layout);
    int width = requested_width;
    if (width < 640)
        width = 640;
    if (width > 1280)
        width = 1280;
    int y = 134;
    for (int i = 0; i < layout.count; i++) {
        layout.nodes[i].height = node_height(layout.nodes[i].step);
        layout.nodes[i].y = y;
        y += layout.nodes[i].height + 12;
    }
    int height = y + (layout.hidden ? 42 : 24);
    if (height < 300)
        height = 300;
    px_canvas_t *c = canvas_acquire(width, height);
    if (!c)
        return NULL;
    canvas_background(c, (uint32_t)plan_id * 2654435761U);

    /* Header: goal, mode, state, and a live topology rail. */
    fill_rect(c, 24, 20, width - 48, 94, C_PANEL, 0.94);
    fill_rect(c, 24, 20, width - 48, 1, C_DIM, 0.34);
    fill_rect(c, 24, 20, 2, 94, status_color(plan->status), 0.68);
    char plan_label[48];
    snprintf(plan_label, sizeof(plan_label), "PLAN #%d // %s", plan->id,
             plan_mode_name(plan->mode));
    draw_text(c, 44, 36, 1, plan_label, C_CYAN, 0.95, width - 280);
    draw_text_ellipsis(c, 44, 56, 3, plan->title ? plan->title : "EXECUTION PLAN", C_TEXT, 0.98,
                       width - 300);
    draw_text_ellipsis(c, 44, 88, 1, plan->goal ? plan->goal : "NO GOAL SET", C_DIM, 0.96,
                       width - 280);
    draw_chip(c, width - 180, 36, plan_status_name(plan->status), status_color(plan->status));
    draw_text(c, width - 180, 88, 1, "PLAN TREE", C_DIM, 0.82, 150);

    /* Connector topology goes behind cards. */
    for (int i = 0; i < layout.count; i++) {
        plan_node_t *node = &layout.nodes[i];
        if (node->parent < 0)
            continue;
        plan_node_t *parent = &layout.nodes[node->parent];
        int px = 48 + parent->depth * 48 + 22;
        int py = parent->y + parent->height;
        int nx = 48 + node->depth * 48 - 12;
        int ny = node->y + 18;
        px_color_t wire = status_color(node->step->status);
        draw_line(c, px, py, px, ny, wire, 0.28);
        draw_line(c, px, ny, nx, ny, wire, 0.48);
        fill_rect(c, nx - 1, ny - 1, 3, 3, wire, 0.62);
    }
    for (int i = 0; i < layout.count; i++)
        draw_node(c, &layout.nodes[i], i);
    if (layout.hidden) {
        char more[48];
        snprintf(more, sizeof(more), "+%d HIDDEN NODES", layout.hidden);
        draw_text(c, 48, height - 28, 1, more, C_AMBER, 0.95, width - 96);
    }
    draw_text(c, width - 166, height - 18, 1, "DSCO / PLAN", C_DIM, 0.72, 146);
    if (out_layout)
        *out_layout = layout;
    return c;
}

static void collect_plan_actions(plan_action_layout_t *layout, int step_id, int depth) {
    if (!layout || depth > 12)
        return;
    step_t *step = step_get(step_id);
    if (!step)
        return;
    for (int i = 0; i < step->atom_count; i++) {
        if (layout->count >= (int)(sizeof(layout->nodes) / sizeof(layout->nodes[0]))) {
            layout->hidden++;
            continue;
        }
        atom_t *atom = atom_get(step->atom_ids[i]);
        if (!atom)
            continue;
        layout->nodes[layout->count++] = (plan_action_node_t){
            .atom = atom,
            .step = step,
            .x = 0,
            .y = 0,
            .w = 0,
            .h = 0,
        };
        layout->wire_count += atom->input_from_count;
    }
    for (int i = 0; i < step->child_step_count; i++)
        collect_plan_actions(layout, step->child_step_ids[i], depth + 1);
}

static void build_action_layout(plan_t *plan, plan_action_layout_t *layout) {
    if (!plan || !layout)
        return;
    memset(layout, 0, sizeof(*layout));
    for (int i = 0; i < plan->root_step_count; i++)
        collect_plan_actions(layout, plan->root_step_ids[i], 0);
    int ready[64];
    int count = plan_atom_ready_wave(plan->id, ready, (int)(sizeof(ready) / sizeof(ready[0])));
    if (count > 0) {
        if (count > (int)(sizeof(layout->ready_ids) / sizeof(layout->ready_ids[0])))
            count = (int)(sizeof(layout->ready_ids) / sizeof(layout->ready_ids[0]));
        memcpy(layout->ready_ids, ready, (size_t)count * sizeof(ready[0]));
        layout->ready_count = count;
    }
}

static bool action_id_ready(const plan_action_layout_t *layout, int atom_id) {
    if (!layout)
        return false;
    for (int i = 0; i < layout->ready_count; i++)
        if (layout->ready_ids[i] == atom_id)
            return true;
    return false;
}

static int action_index_for_id(const plan_action_layout_t *layout, int atom_id) {
    if (!layout)
        return -1;
    int visible = layout->count > 12 ? 12 : layout->count;
    for (int i = 0; i < visible; i++)
        if (layout->nodes[i].atom && layout->nodes[i].atom->id == atom_id)
            return i;
    return -1;
}

static const char *plan_action_label(const atom_t *atom) {
    if (!atom)
        return "ACTION";
    if (atom->title && *atom->title)
        return atom->title;
    if (atom->tool_name && *atom->tool_name)
        return atom->tool_name;
    return atom_type_name(atom->type);
}

static void draw_plan_action_card(px_canvas_t *c, const plan_action_node_t *node,
                                  const plan_action_layout_t *layout) {
    if (!c || !node || !node->atom)
        return;
    const atom_t *atom = node->atom;
    px_color_t accent = status_color(atom->status);
    bool ready = action_id_ready(layout, atom->id);
    fill_rect(c, node->x, node->y, node->w, node->h, C_PANEL_ALT, ready ? 0.93 : 0.78);
    fill_rect(c, node->x, node->y, 3, node->h, accent, ready ? 0.92 : 0.60);
    fill_rect(c, node->x, node->y, node->w, 1, C_DIM, 0.22);
    fill_rect(c, node->x, node->y + node->h - 1, node->w, 1, C_DIM, 0.13);
    char id[24];
    snprintf(id, sizeof(id), "A-%02d", atom->id);
    draw_text(c, node->x + 10, node->y + 8, 1, id, accent, 0.92, 42);
    if (ready)
        fill_circle(c, node->x + node->w - 9, node->y + 10, 3, C_GREEN, 0.92);
    draw_text_ellipsis(c, node->x + 10, node->y + 25, 1, plan_action_label(atom), C_TEXT, 0.84,
                       node->w - 20);
    draw_chip(c, node->x + 10, node->y + node->h - 24, plan_status_name(atom->status), accent);
}

static int plan_action_lane_index(const plan_action_layout_t *layout, int step_id, int *step_ids,
                                  int *count, int max_count) {
    if (!layout || !step_ids || !count || max_count < 1)
        return -1;
    for (int i = 0; i < *count; i++)
        if (step_ids[i] == step_id)
            return i;
    if (*count >= max_count)
        return -1;
    step_ids[*count] = step_id;
    return (*count)++;
}

static px_canvas_t *render_plan_actions_frame(int plan_id, int requested_width) {
    plan_t *plan = plan_get(plan_id);
    if (!plan)
        return NULL;
    int width = requested_width;
    if (width < 640)
        width = 640;
    if (width > 1280)
        width = 1280;
    int height = 620;
    px_canvas_t *c = canvas_acquire(width, height);
    if (!c)
        return NULL;
    canvas_background(c, (uint32_t)plan_id * 2246822519U);

    plan_action_layout_t layout;
    build_action_layout(plan, &layout);
    int visible = layout.count > 12 ? 12 : layout.count;
    int margin = 24, gap = 18, top_y = 20;
    fill_rect(c, margin, top_y, width - margin * 2, 94, C_PANEL, 0.94);
    fill_rect(c, margin, top_y, width - margin * 2, 1, C_DIM, 0.34);
    fill_rect(c, margin, top_y, 2, 94, status_color(plan->status), 0.68);
    draw_text(c, margin + 20, top_y + 16, 1, "ACTION PLAN / DEPENDENCY MAP", C_CYAN, 0.94,
              width - 300);
    draw_text_ellipsis(c, margin + 20, top_y + 36, 2, plan->title ? plan->title : "ACTION PLAN",
                       C_TEXT, 0.96, width - 300);
    draw_text_ellipsis(c, margin + 20, top_y + 67, 1, plan->goal ? plan->goal : "NO GOAL SET",
                       C_DIM, 0.86, width - 300);
    draw_chip(c, width - 190, top_y + 18, plan_status_name(plan->status),
              status_color(plan->status));
    char count_label[64];
    snprintf(count_label, sizeof(count_label), "%d ACTIONS  /  %d READY", layout.count,
             layout.ready_count);
    draw_text(c, width - 190, top_y + 69, 1, count_label, C_DIM, 0.74, 170);

    int body_y = 132, bottom_h = 142;
    int body_h = height - body_y - bottom_h - gap - margin;
    int left_w = (width * 64) / 100;
    int right_w = width - margin * 2 - left_w - gap;
    int left_x = margin, right_x = left_x + left_w + gap;
    fill_rect(c, left_x, body_y, left_w, body_h, C_PANEL, 0.90);
    fill_rect(c, right_x, body_y, right_w, body_h, C_PANEL, 0.90);
    draw_text(c, left_x + 16, body_y + 12, 1, "ACTION DAG / WIRED INPUTS", C_TEXT, 0.76,
              left_w - 32);
    draw_text(c, right_x + 16, body_y + 12, 1, "READY FRONTIER / GATES", C_TEXT, 0.76,
              right_w - 32);
    draw_line(c, left_x + 16, body_y + 31, left_x + left_w - 16, body_y + 31, C_DIM, 0.20);
    draw_line(c, right_x + 16, body_y + 31, right_x + right_w - 16, body_y + 31, C_DIM, 0.20);

    int cols = width >= 900 ? 4 : 3;
    if (visible > 0 && visible < cols)
        cols = visible;
    if (cols < 1)
        cols = 1;
    int node_gap = 14;
    int node_w = (left_w - 32 - (cols - 1) * node_gap) / cols;
    if (node_w < 84)
        node_w = 84;
    int node_h = 62;
    for (int i = 0; i < visible; i++) {
        int col = i % cols, row = i / cols;
        layout.nodes[i].w = node_w;
        layout.nodes[i].h = node_h;
        layout.nodes[i].x = left_x + 16 + col * (node_w + node_gap);
        layout.nodes[i].y = body_y + 48 + row * (node_h + 18);
    }
    /* Dependency wires are drawn behind cards; atom input edges are the
     * strongest signal, while adjacent actions provide a faint fallback flow
     * for plans that have not declared explicit wiring yet. */
    for (int i = 0; i < visible; i++) {
        plan_action_node_t *dst = &layout.nodes[i];
        int edge_count = 0;
        for (int j = 0; j < dst->atom->input_from_count; j++) {
            int src_i = action_index_for_id(&layout, dst->atom->input_from_ids[j]);
            if (src_i < 0)
                continue;
            plan_action_node_t *src = &layout.nodes[src_i];
            draw_line(c, src->x + src->w / 2, src->y + src->h / 2, dst->x + dst->w / 2,
                      dst->y + dst->h / 2, status_color(dst->atom->status), 0.54);
            edge_count++;
        }
        if (edge_count == 0 && i > 0) {
            plan_action_node_t *src = &layout.nodes[i - 1];
            draw_line(c, src->x + src->w, src->y + src->h / 2, dst->x, dst->y + dst->h / 2, C_DIM,
                      0.20);
        }
    }
    for (int i = 0; i < visible; i++)
        draw_plan_action_card(c, &layout.nodes[i], &layout);
    if (layout.hidden || layout.count > visible) {
        char hidden[48];
        snprintf(hidden, sizeof(hidden), "+%d ACTIONS HIDDEN",
                 layout.hidden + layout.count - visible);
        draw_text(c, left_x + 16, body_y + body_h - 16, 1, hidden, C_AMBER, 0.82, left_w - 32);
    }

    int ry = body_y + 48, rw = right_w - 32;
    draw_text(c, right_x + 16, ry, 1, "READY NOW", C_GREEN, 0.86, rw);
    ry += 20;
    int shown_ready = layout.ready_count < 5 ? layout.ready_count : 5;
    for (int i = 0; i < shown_ready; i++) {
        atom_t *atom = atom_get(layout.ready_ids[i]);
        if (!atom)
            continue;
        char label[96];
        snprintf(label, sizeof(label), "A-%02d  %s", atom->id, plan_action_label(atom));
        draw_text_ellipsis(c, right_x + 16, ry, 1, label, C_GREEN, 0.84, rw);
        ry += 18;
    }
    if (shown_ready == 0) {
        draw_text(c, right_x + 16, ry, 1, "NO READY ACTIONS", C_DIM, 0.74, rw);
        ry += 18;
    }
    ry += 9;
    draw_text(c, right_x + 16, ry, 1, "GATED / WAITING", C_AMBER, 0.86, rw);
    ry += 20;
    int shown_waiting = 0;
    for (int i = 0; i < visible && shown_waiting < 4; i++) {
        atom_t *atom = layout.nodes[i].atom;
        if (!atom || action_id_ready(&layout, atom->id) || atom->status == PLAN_DONE)
            continue;
        char label[96];
        snprintf(label, sizeof(label), "A-%02d  %s", atom->id, plan_status_name(atom->status));
        draw_text_ellipsis(c, right_x + 16, ry, 1, label, C_AMBER, 0.78, rw);
        ry += 18;
        shown_waiting++;
    }
    if (shown_waiting == 0) {
        draw_text(c, right_x + 16, ry, 1, "NONE", C_DIM, 0.72, rw);
        ry += 18;
    }
    plan_graph_diagnostic_t diag = {0};
    bool graph_ok = plan_graph_validate(plan->id, &diag);
    draw_text(c, right_x + 16, body_y + body_h - 55, 1,
              graph_ok ? "GRAPH VALID" : plan_graph_error_name(diag.code),
              graph_ok ? C_GREEN : C_RED, 0.78, rw);
    char wires[48];
    snprintf(wires, sizeof(wires), "WIRES %d  /  PARALLEL %dX", layout.wire_count,
             layout.ready_count > 3 ? 3 : layout.ready_count);
    draw_text(c, right_x + 16, body_y + body_h - 36, 1, wires, C_CYAN, 0.74, rw);
    draw_meter(c, right_x + 16, body_y + body_h - 17, rw,
               layout.count > 0
                   ? (100.0 * (double)(layout.count - layout.ready_count) / (double)layout.count)
                   : 0.0,
               C_CYAN);

    int timeline_y = body_y + body_h + gap;
    fill_rect(c, margin, timeline_y, width - margin * 2, bottom_h, C_PANEL, 0.90);
    draw_text(c, margin + 16, timeline_y + 12, 1, "EXECUTION LANES / NEXT ACTION", C_TEXT, 0.76,
              width - margin * 2 - 32);
    draw_line(c, margin + 16, timeline_y + 31, width - margin - 16, timeline_y + 31, C_DIM, 0.20);
    int step_ids[8], lane_count = 0;
    for (int i = 0; i < visible; i++)
        (void)plan_action_lane_index(&layout, layout.nodes[i].step->id, step_ids, &lane_count, 8);
    int lane_x = margin + 112;
    int lane_w = width - margin * 2 - 132;
    int lane_h = lane_count > 0 ? (bottom_h - 54) / lane_count : 18;
    if (lane_h < 15)
        lane_h = 15;
    for (int lane = 0; lane < lane_count; lane++) {
        int yy = timeline_y + 42 + lane * lane_h;
        step_t *step = step_get(step_ids[lane]);
        draw_text_ellipsis(c, margin + 16, yy, 1, step && step->title ? step->title : "STEP", C_DIM,
                           0.74, 88);
        fill_rect(c, lane_x, yy + 4, lane_w, 7, C_BG_BOTTOM, 0.92);
        for (int i = 0; i < visible; i++) {
            if (!layout.nodes[i].step || layout.nodes[i].step->id != step_ids[lane])
                continue;
            int bx = lane_x + (i * lane_w) / (visible > 0 ? visible : 1);
            int bw = lane_w / (visible > 0 ? visible : 1) - 3;
            if (bw < 6)
                bw = 6;
            fill_rect(c, bx, yy + 4, bw, 7, status_color(layout.nodes[i].atom->status), 0.76);
        }
    }
    int marker = lane_x + (int)(lane_w * motion_phase(1.8, 0.0));
    fill_rect(c, marker, timeline_y + 34, 2, bottom_h - 42, C_TEXT, 0.56);
    char footer[128];
    snprintf(footer, sizeof(footer), "READY %d  ·  WIRED %d  ·  %s", layout.ready_count,
             layout.wire_count,
             layout.ready_count > 0 ? "NEXT ACTION HIGHLIGHTED" : "WAITING ON DEPENDENCIES");
    draw_text(c, margin + 16, timeline_y + bottom_h - 16, 1, footer, C_CYAN, 0.76,
              width - margin * 2 - 32);
    return c;
}

static const char *session_state_name(pixel_tui_state_t state) {
    switch (state) {
        case PIXEL_TUI_REASONING:
            return "REASONING";
        case PIXEL_TUI_EXECUTING:
            return "EXECUTING";
        case PIXEL_TUI_RESPONDING:
            return "RESPONDING";
        default:
            return "IDLE";
    }
}

static px_color_t session_state_color(pixel_tui_state_t state) {
    switch (state) {
        case PIXEL_TUI_REASONING:
            return C_CYAN;
        case PIXEL_TUI_EXECUTING:
            return C_AMBER;
        case PIXEL_TUI_RESPONDING:
            return C_GREEN;
        default:
            return C_VIOLET;
    }
}

static double session_transition_progress(void) {
    if (!g_session.active || !g_session.animation_enabled ||
        g_session.previous_state == g_session.state || g_session.transition_started_s <= 0.0)
        return 1.0;
    return smoothstep((monotonic_s() - g_session.transition_started_s) / 0.24);
}

static px_color_t session_animated_accent(pixel_tui_state_t state) {
    if (!g_session.active || state != g_session.state)
        return session_state_color(state);
    double progress = session_transition_progress();
    return color_mix(session_state_color(g_session.previous_state), session_state_color(state),
                     progress);
}

static px_color_t message_role_color(const char *role) {
    if (!role)
        return C_DIM;
    if (!strcasecmp(role, "USER"))
        return C_CYAN;
    if (!strcasecmp(role, "ASSISTANT"))
        return C_DIM;
    if (!strncasecmp(role, "TOOL", 4))
        return C_AMBER;
    if (!strcasecmp(role, "ERROR"))
        return C_RED;
    if (!strcasecmp(role, "THINKING"))
        return C_CYAN;
    return C_GREEN;
}

typedef struct {
    int total;
    int users;
    int assistants;
    int tools;
    int thinking;
    int errors;
} session_summary_t;

static session_summary_t session_summarize(void) {
    session_summary_t summary = {0};
    for (int i = 0; i < g_session.message_count; i++) {
        int at = (g_session.message_start + i) % PIXEL_MESSAGE_CAP;
        const char *role = g_session.messages[at].role;
        summary.total++;
        if (!strcasecmp(role, "USER"))
            summary.users++;
        else if (!strcasecmp(role, "ASSISTANT"))
            summary.assistants++;
        else if (!strncasecmp(role, "TOOL", 4))
            summary.tools++;
        else if (!strcasecmp(role, "THINKING"))
            summary.thinking++;
        else if (!strcasecmp(role, "ERROR"))
            summary.errors++;
    }
    return summary;
}

static pixel_turn_visual_t *session_current_turn_visual(void) {
    if (g_session.turn_visual_count < 1)
        return NULL;
    return &g_session.turn_visuals[g_session.turn_visual_count - 1];
}

static int session_running_tool_count(void) {
    int count = 0;
    for (int i = 0; i < PIXEL_TOOL_VIS_CAP; i++)
        if (g_session.tool_visuals[i].used && g_session.tool_visuals[i].status == PIXEL_OP_RUNNING)
            count++;
    return count;
}

static int session_swarm_counts(int *active, int *done, int *errors) {
    int total = 0;
    if (active)
        *active = 0;
    if (done)
        *done = 0;
    if (errors)
        *errors = 0;
    for (int i = 0; i < PIXEL_SWARM_VIS_CAP; i++) {
        const pixel_swarm_visual_t *agent = &g_session.swarm_visuals[i];
        if (!agent->used)
            continue;
        total++;
        if (!strcasecmp(agent->status, "done")) {
            if (done)
                (*done)++;
        } else if (!strcasecmp(agent->status, "error") || !strcasecmp(agent->status, "killed")) {
            if (errors)
                (*errors)++;
        } else if (active) {
            (*active)++;
        }
    }
    return total;
}

static const pixel_tool_visual_t *session_latest_tool(void) {
    const pixel_tool_visual_t *latest = NULL;
    for (int i = 0; i < PIXEL_TOOL_VIS_CAP; i++) {
        const pixel_tool_visual_t *tool = &g_session.tool_visuals[i];
        if (tool->used && (!latest || tool->sequence > latest->sequence))
            latest = tool;
    }
    return latest;
}

static const pixel_tool_visual_t *session_latest_running_tool(void) {
    const pixel_tool_visual_t *latest = NULL;
    for (int i = 0; i < PIXEL_TOOL_VIS_CAP; i++) {
        const pixel_tool_visual_t *tool = &g_session.tool_visuals[i];
        if (tool->used && tool->status == PIXEL_OP_RUNNING &&
            (!latest || tool->sequence > latest->sequence))
            latest = tool;
    }
    return latest;
}

static bool swarm_visual_terminal(const char *status) {
    return status && (!strcasecmp(status, "done") || !strcasecmp(status, "error") ||
                      !strcasecmp(status, "killed"));
}

static void format_elapsed(char *dst, size_t cap, double seconds) {
    if (!dst || cap == 0)
        return;
    if (seconds < 0.0)
        seconds = 0.0;
    int elapsed = (int)seconds;
    if (elapsed < 60)
        snprintf(dst, cap, "%02ds", elapsed);
    else if (elapsed < 3600)
        snprintf(dst, cap, "%02dm %02ds", elapsed / 60, elapsed % 60);
    else
        snprintf(dst, cap, "%02dh %02dm", elapsed / 3600, (elapsed / 60) % 60);
}

static void format_runway(char *dst, size_t cap, double seconds) {
    if (!dst || cap == 0)
        return;
    if (seconds < 0.0)
        snprintf(dst, cap, "unlimited");
    else
        format_elapsed(dst, cap, seconds);
}

static void __attribute__((unused)) draw_state_track(px_canvas_t *c, int x, int y, int w,
                                                     pixel_tui_state_t state) {
    static const char *labels[] = {"READY", "THINK", "TOOLS", "REPLY"};
    int active = (int)state;
    int gap = 5;
    int segment_w = (w - gap * 3) / 4;
    if (!c || segment_w < 28)
        return;
    double transition = session_transition_progress();
    double pulse = state == PIXEL_TUI_IDLE ? 0.0 : motion_pulse(0.90, 0.0);
    for (int i = 0; i < 4; i++) {
        int xx = x + i * (segment_w + gap);
        bool was_active = i == (int)g_session.previous_state && transition < 1.0;
        px_color_t color = i == active ? session_animated_accent(state) : C_DIM;
        double panel_alpha = i == active ? 0.13 + pulse * 0.07 : 0.055;
        if (was_active)
            panel_alpha += (1.0 - transition) * 0.08;
        fill_rect(c, xx, y, segment_w, 16, color, panel_alpha);
        fill_rect(c, xx, y + 15, segment_w, 1, color, i == active ? 0.58 + pulse * 0.25 : 0.14);
        if (i == active && state != PIXEL_TUI_IDLE)
            draw_motion_sweep(c, xx, y + 14, segment_w, 2, color, 1.20, 0.84);
        if (segment_w >= 58)
            draw_text(c, xx + 6, y + 4, 1, labels[i], color, i == active ? 0.92 : 0.48,
                      segment_w - 12);
    }
}

static void draw_key_value(px_canvas_t *c, int x, int y, int w, const char *key, const char *value,
                           px_color_t value_color) {
    char label[48];
    snprintf(label, sizeof(label), "%s", key);
    for (size_t i = 1; label[i]; i++) label[i] = (char)tolower((unsigned char)label[i]);
    draw_ui_label(c, x, y, 11.0f, false, label, C_DIM, 0.85, w / 2);
    int value_w = font_compat_measure_utf8(value, text_point_size(1), false);
    if (value_w < 0)
        value_w = (int)strlen(value) * 6;
    int value_x = x + w - value_w;
    if (value_x < x + w / 2)
        value_x = x + w / 2;
    draw_text_ellipsis(c, value_x, y, 1, value, value_color, 0.90, x + w - value_x);
}

static void draw_meter(px_canvas_t *c, int x, int y, int w, double percent, px_color_t color) {
    if (percent < 0.0)
        percent = 0.0;
    if (percent > 100.0)
        percent = 100.0;
    fill_rounded(c, x, y, w, 4, 2, C_DIM, 0.14);
    int fill_w = (int)((double)w * percent / 100.0);
    if (fill_w > 0) {
        fill_rounded(c, x, y, fill_w, 4, 2, color, 0.68);
        /* Value edge: a slightly brighter tip makes motion readable even at
         * one-percent deltas without adding any chrome. */
        if (fill_w > 4)
            fill_rounded(c, x + fill_w - 3, y, 3, 4, 2, color, 0.95);
    }
}

static px_color_t tool_visual_color(pixel_op_status_t status) {
    if (status == PIXEL_OP_DONE)
        return C_GREEN;
    if (status == PIXEL_OP_ERROR)
        return C_RED;
    return C_AMBER;
}

static px_color_t __attribute__((unused)) swarm_visual_color(const char *status) {
    if (status && !strcasecmp(status, "done"))
        return C_GREEN;
    if (status && (!strcasecmp(status, "error") || !strcasecmp(status, "killed")))
        return C_RED;
    if (status && !strcasecmp(status, "streaming"))
        return C_CYAN;
    return C_VIOLET;
}

static void draw_turn_trace(px_canvas_t *c, int x, int y, int w, int h) {
    int count = g_session.turn_visual_count;
    if (!c || count < 1 || w < 30 || h < 8)
        return;
    int shown = count > 8 ? 8 : count;
    int gap = 4;
    int cell_w = (w - gap * (shown - 1)) / shown;
    if (cell_w < 5)
        cell_w = 5;
    int start = count - shown;
    for (int i = 0; i < shown; i++) {
        const pixel_turn_visual_t *turn = &g_session.turn_visuals[start + i];
        int load = turn->tool_count + turn->swarm_count * 2;
        int bar_h = 4 + (load > 6 ? 6 : load) * 2;
        if (bar_h > h)
            bar_h = h;
        bool current = i == shown - 1;
        px_color_t color = current ? session_animated_accent(g_session.state) : C_DIM;
        fill_rect(c, x + i * (cell_w + gap), y + h - bar_h, cell_w, bar_h, color,
                  current ? 0.72 : 0.24);
        if (current && g_session.animation_enabled)
            draw_motion_sweep(c, x + i * (cell_w + gap), y + h - 2, cell_w, 2, color, 0.90, 0.82);
    }
}

static void __attribute__((unused)) draw_phase_field(px_canvas_t *c, int x, int y, int w, int h,
                                                     pixel_tui_state_t state) {
    if (!c || w < 30 || h < 12)
        return;
    px_color_t accent = session_animated_accent(state);
    double phase = motion_phase(state == PIXEL_TUI_EXECUTING ? 0.90 : 1.35, 0.0);
    if (state == PIXEL_TUI_REASONING) {
        int last_y = y + h / 2;
        for (int xx = 0; xx < w; xx += 3) {
            double wave = sin(((double)xx / 18.0) + phase * 6.28318530717958647692);
            double carrier = sin(((double)xx / 47.0) - phase * 12.56637061435917295384);
            int next_y = y + h / 2 + (int)(wave * (h * 0.24) + carrier * (h * 0.10));
            if (xx > 0)
                draw_line(c, x + xx - 3, last_y, x + xx, next_y, accent, 0.56);
            last_y = next_y;
        }
        for (int i = 0; i < 4; i++) {
            int nx = x + (i + 1) * w / 5;
            double pulse = 0.36 + motion_pulse(0.82, i * 0.11) * 0.62;
            fill_circle(c, nx, y + h / 2, 2 + (int)(pulse * 2.0), accent, pulse);
        }
    } else if (state == PIXEL_TUI_EXECUTING) {
        int slot_w = w / 7;
        if (slot_w < 12)
            slot_w = 12;
        int offset = (int)(phase * (slot_w + 6));
        for (int xx = x - slot_w + offset; xx < x + w; xx += slot_w + 6) {
            fill_rect(c, xx, y + h / 2 - 4, slot_w, 8, C_PANEL_ALT, 0.92);
            fill_rect(c, xx, y + h / 2 + 3, slot_w, 1, accent, 0.72);
        }
        draw_motion_sweep(c, x, y + h / 2 - 1, w, 2, accent, 0.82, 0.90);
    } else if (state == PIXEL_TUI_RESPONDING) {
        for (int i = 0; i < 7; i++) {
            double energy = motion_pulse(1.05, i * 0.09);
            int bar_h = 3 + (int)(energy * (h - 4));
            int xx = x + i * w / 7;
            fill_rect(c, xx, y + (h - bar_h) / 2, w / 10, bar_h, accent, 0.28 + energy * 0.52);
        }
    } else {
        draw_turn_trace(c, x, y, w, h);
    }
}

static void draw_live_operations(px_canvas_t *c, int x, int y, int w, int h,
                                 pixel_tui_state_t state) {
    if (!c || w < 120 || h < 38)
        return;
    (void)state;
    draw_text(c, x, y, 1, "ACTIVE OPERATION", C_TEXT, 0.70, w);
    int yy = y + 22;
    const pixel_tool_visual_t *tool = session_latest_tool();
    if (tool) {
        px_color_t color = tool_visual_color(tool->status);
        /* Completion answer: the finished row exhales once — a wash that
         * decays over ~a second — instead of silently flipping color. */
        double flash =
            session_motion_value(MOTION_KEY_TOOL(tool->sequence), MOTION_PROP_FLASH, 0.0);
        if (flash > 0.01)
            fill_rounded(c, x - 4, yy - 3, w + 4, 17, 5, color, 0.16 * flash);
        fill_circle(c, x + 3, yy + 5, 3, color, 0.78);
        draw_text_ellipsis(c, x + 13, yy, 1, tool->name, color, 0.90, w - 13);
        yy += 18;
        if (tool->preview[0] && yy + 14 < y + h) {
            draw_text_ellipsis(c, x + 13, yy, 1, tool->preview, C_DIM, 0.72, w - 13);
            yy += 18;
        }
    }
    int active = 0, done = 0, errors = 0;
    int swarm_total = session_swarm_counts(&active, &done, &errors);
    if (swarm_total > 0 && yy + 14 < y + h) {
        char label[80];
        snprintf(label, sizeof(label), "SWARM  %d LIVE  /  %d DONE", active, done);
        draw_text_ellipsis(c, x, yy, 1, label, errors ? C_RED : C_CYAN, 0.76, w);
    }
}

static void draw_session_rail(px_canvas_t *c, int x, int y, int w, int h, const char *model,
                              pixel_tui_state_t state, const session_summary_t *summary) {
    if (!c || !summary || w < 150 || h < 120)
        return;
    px_color_t accent = session_animated_accent(state);
    int inner_x = x + 14;
    int inner_w = w - 28;
    fill_rect(c, x, y, w, h, C_PANEL, 0.58);
    fill_rect(c, x, y, 1, h, C_DIM, 0.22);
    fill_rect(c, x + w - 1, y, 1, h, C_DIM, 0.12);
    (void)model; /* Model identity is already visible in the persistent header. */
    draw_ui_label(c, inner_x, y + 14, 14.0f, true, "Session", C_TEXT, 0.92, inner_w);
    draw_line(c, inner_x, y + 40, x + w - 14, y + 40, C_DIM, 0.14);

    char value[64];
    int yy = y + 54;
    draw_key_value(c, inner_x, yy, inner_w, "STATE", session_state_name(state), accent);
    yy += 19;
    snprintf(value, sizeof(value), "%d", g_session.turn);
    draw_key_value(c, inner_x, yy, inner_w, "TURN", value, C_TEXT);
    yy += 19;
    format_elapsed(value, sizeof(value), monotonic_s() - g_session.started_s);
    draw_key_value(c, inner_x, yy, inner_w, "ELAPSED", value, C_TEXT);
    yy += 19;
    snprintf(value, sizeof(value), "%d", summary->total);
    draw_key_value(c, inner_x, yy, inner_w, "EVENTS", value, C_TEXT);
    yy += 19;
    snprintf(value, sizeof(value), "%d / %d", g_session.queue_depth,
             g_session.queue_capacity > 0 ? g_session.queue_capacity : 8);
    draw_key_value(c, inner_x, yy, inner_w, "QUEUE", value,
                   g_session.queue_depth > 0 ? C_AMBER : C_GREEN);

    int swarm_total = session_swarm_counts(NULL, NULL, NULL);
    if (state != PIXEL_TUI_IDLE || session_running_tool_count() > 0 || swarm_total > 0) {
        if (session_running_tool_count() > 0 || swarm_total > 0) {
            yy += 28;
            draw_live_operations(c, inner_x, yy, inner_w, y + h - yy - 12, state);
        }
        return;
    }

    if (h < 255)
        return;
    yy += 27;
    draw_ui_label(c, inner_x, yy, 11.0f, true, "Resources", C_TEXT, 0.85, inner_w);
    yy += 20;
    snprintf(value, sizeof(value), "%.0f%%", g_session.context_percent);
    draw_key_value(c, inner_x, yy, inner_w, "CONTEXT", value,
                   g_session.context_percent >= 85.0
                       ? C_RED
                       : (g_session.context_percent >= 60.0 ? C_AMBER : C_GREEN));
    yy += 15;
    draw_meter(
        c, inner_x, yy, inner_w,
        session_motion_value(MOTION_KEY_CONTEXT, MOTION_PROP_VALUE, g_session.context_percent),
        g_session.context_percent >= 85.0
            ? C_RED
            : (g_session.context_percent >= 60.0 ? C_AMBER : C_GREEN));
    yy += 17;
    snprintf(value, sizeof(value), "%d / %d", g_session.input_tokens, g_session.output_tokens);
    draw_key_value(c, inner_x, yy, inner_w, "IN / OUT", value, C_TEXT);
    yy += 19;
    if (g_session.budget_limit_usd > 0.0)
        snprintf(value, sizeof(value), "$%.3f%s / $%.2f", g_session.cost_usd,
                 g_session.unpriced_responses > 0 ? "+?" : "",
                 g_session.budget_limit_usd);
    else
        snprintf(value, sizeof(value), "$%.4f%s", g_session.cost_usd,
                 g_session.unpriced_responses > 0 ? "+?" : "");
    draw_key_value(c, inner_x, yy, inner_w, "VALUE", value,
                   g_session.budget_percent >= 80.0 ? C_AMBER : C_TEXT);
    int cost_detail_height = 0;
    if (h >= 330) {
        yy += 19;
        if (g_session.reported_cost_samples > 0)
            snprintf(value, sizeof(value), "$%.4f", g_session.reported_cost_usd);
        else snprintf(value, sizeof(value), "unknown");
        draw_key_value(c, inner_x, yy, inner_w, "REPORTED", value, C_TEXT);
        yy += 19;
        if (g_session.estimated_cost_samples > 0)
            snprintf(value, sizeof(value), "$%.4f", g_session.estimated_cost_usd);
        else snprintf(value, sizeof(value), "unknown");
        draw_key_value(c, inner_x, yy, inner_w, "REF EST.", value, C_DIM);
        yy += 19;
        snprintf(value, sizeof(value), "%d", g_session.unpriced_responses);
        draw_key_value(c, inner_x, yy, inner_w, "UNPRICED", value,
                       g_session.unpriced_responses > 0 ? C_AMBER : C_DIM);
        cost_detail_height = 57;
    }
    yy += 19;
    if (g_session.budget_limit_usd > 0.0) {
        char runway[24];
        format_runway(runway, sizeof(runway), g_session.budget_runway_s);
        snprintf(value, sizeof(value), "%.0f%% / %s", g_session.budget_percent, runway);
        draw_key_value(c, inner_x, yy, inner_w, "BUDGET", value,
                       g_session.budget_percent >= 95.0
                           ? C_RED
                           : (g_session.budget_percent >= 80.0 ? C_AMBER : C_GREEN));
    } else {
        snprintf(value, sizeof(value), "%d", g_session.tools_used);
        draw_key_value(c, inner_x, yy, inner_w, "TOOLS", value, C_TEXT);
    }

    if (h < 385 + cost_detail_height)
        return;
    yy += 19;
    snprintf(value, sizeof(value), "$%.2f/h", g_session.budget_burn_rate);
    draw_key_value(c, inner_x, yy, inner_w, "BURN", value,
                   g_session.budget_percent >= 80.0 ? C_AMBER : C_TEXT);
    yy += 27;
    draw_ui_label(c, inner_x, yy, 11.0f, true, "Turn history", C_TEXT, 0.85, inner_w);
    draw_turn_trace(c, inner_x, yy + 17, inner_w, 28);
    yy += 54;
    draw_ui_label(c, inner_x, yy, 11.0f, true, "Activity", C_TEXT, 0.85, inner_w);
    yy += 20;
    snprintf(value, sizeof(value), "%d", summary->users);
    draw_key_value(c, inner_x, yy, inner_w, "USER", value, C_CYAN);
    yy += 18;
    snprintf(value, sizeof(value), "%d", summary->assistants);
    draw_key_value(c, inner_x, yy, inner_w, "ASSIST", value, C_VIOLET);
    yy += 18;
    snprintf(value, sizeof(value), "%d", summary->tools);
    draw_key_value(c, inner_x, yy, inner_w, "TOOLS", value, C_AMBER);
    yy += 18;
    snprintf(value, sizeof(value), "%d", summary->errors);
    draw_key_value(c, inner_x, yy, inner_w, "ERRORS", value, summary->errors ? C_RED : C_DIM);
}

/* Convert arbitrary provider/tool text into the compact bitmap font's safe
 * subset. ANSI controls and Markdown punctuation must never leak back into
 * Kitty as terminal commands because the image is the primary UI surface. */
static size_t plain_text_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0)
        return 0;
    size_t n = 0;
    bool space = false;
    for (size_t i = 0; src && src[i] && n + 1 < cap; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == 0x1b) {
            i++;
            if (src[i] == '[') {
                i++;
                while (src[i] && !((src[i] >= '@' && src[i] <= '~')))
                    i++;
            } else if (src[i] == ']') {
                while (src[i] && src[i] != '\a' && !(src[i] == 0x1b && src[i + 1] == '\\'))
                    i++;
                if (src[i] == 0x1b && src[i + 1] == '\\')
                    i++;
            }
            continue;
        }
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            while (n > 0 && dst[n - 1] == ' ')
                n--;
            if (n > 0 && dst[n - 1] != '\n')
                dst[n++] = '\n';
            space = false;
            continue;
        }
        if (ch == '\t' || ch < 0x20) {
            if (!space && n > 0)
                dst[n++] = ' ';
            space = true;
            continue;
        }
        if (ch >= 0x80) {
            size_t seq = (ch & 0xe0) == 0xc0   ? 2
                         : (ch & 0xf0) == 0xe0 ? 3
                         : (ch & 0xf8) == 0xf0 ? 4
                                               : 1;
            size_t valid = 1;
            while (valid < seq && src[i + valid] && ((unsigned char)src[i + valid] & 0xc0) == 0x80)
                valid++;
            if (valid != seq || n + seq >= cap)
                continue;
            memcpy(dst + n, src + i, seq);
            n += seq;
            i += seq - 1;
            space = false;
            continue;
        }
        dst[n++] = (char)ch;
        space = ch == ' ';
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\n'))
        n--;
    dst[n] = '\0';
    return n;
}

/* ── Inline live-operation cards ─────────────────────────────────────────
 * While tools run, the transcript tail carries animated native cards: a
 * category-tinted rail, a spinner arc, the salient argument preview, and a
 * ticking elapsed readout. Every card derives from tool_visuals plus the
 * retained motion timeline — presence eases in on tool_begin and out on
 * tool_end while the durable TOOL row arrives, so liveness costs no new
 * session state and no coordination code. */
#define LIVE_OP_CARD_H 36
#define LIVE_OP_CARD_COMPACT_H 20
#define LIVE_OP_CARD_GAP 4
#define LIVE_OP_CARD_MAX 3
/* CoreText's mask includes descent + padding. Reserving only 14 px made
 * the overflow label clip/reposition when rendered into a bounded patch. */
static int live_op_chip_height(void) {
    int line_h = font_compat_line_height(text_point_size(1), false) + 4;
    return line_h > 18 ? line_h : 18;
}

/* Purely cosmetic category classifier for the card rail; keep it dumb. */
static px_color_t tool_accent_color(const char *name) {
    char low[64];
    size_t n = 0;
    for (; name && name[n] && n + 1 < sizeof(low); n++)
        low[n] = (char)tolower((unsigned char)name[n]);
    low[n] = '\0';
    if (strstr(low, "read") || strstr(low, "grep") || strstr(low, "glob") || strstr(low, "ls") ||
        strstr(low, "search"))
        return C_CYAN;
    if (strstr(low, "write") || strstr(low, "edit") || strstr(low, "apply") || strstr(low, "patch"))
        return C_VIOLET;
    if (strstr(low, "bash") || strstr(low, "shell") || strstr(low, "sandbox") ||
        strstr(low, "exec"))
        return C_AMBER;
    if (strstr(low, "web") || strstr(low, "http") || strstr(low, "fetch") || strstr(low, "url"))
        return C_GREEN;
    return C_CYAN;
}

/* Pull the most salient argument out of a tool's input JSON — the command,
 * path, pattern, or query a person actually wants to see — falling back to
 * the raw JSON when nothing matches. Naive by design: display-only text that
 * is never parsed back. */
static void tool_preview_extract(const char *name, const char *input_json, char *dst, size_t cap) {
    (void)name;
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!input_json || !*input_json)
        return;
    static const char *const keys[] = {
        "command", "file_path", "path", "pattern", "query", "url", "prompt",
    };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        char needle[24];
        snprintf(needle, sizeof(needle), "\"%s\"", keys[k]);
        const char *p = strstr(input_json, needle);
        if (!p)
            continue;
        p += strlen(needle);
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != ':')
            continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p != '"')
            continue;
        p++;
        char value[192];
        size_t n = 0;
        while (*p && *p != '"' && n + 1 < sizeof(value)) {
            if (*p == '\\' && p[1]) {
                char esc = p[1];
                value[n++] = (esc == 'n' || esc == 't' || esc == 'r') ? ' ' : esc;
                p += 2;
                continue;
            }
            value[n++] = *p++;
        }
        value[n] = '\0';
        if (n > 0) {
            plain_text_copy(dst, cap, value);
            if (dst[0])
                return;
        }
    }
    plain_text_copy(dst, cap, input_json);
}

/* Headless seam for the preview classifier (regression-tested directly). */
void pixel_tui_tool_preview_extract(const char *name, const char *input_json, char *dst,
                                    size_t cap) {
    tool_preview_extract(name, input_json, dst, cap);
}

/* ── Heap-backed message text ─────────────────────────────────────────
 * Transcript rows grow on demand up to PIXEL_MESSAGE_TEXT_MAX; overflow
 * drops the oldest bytes so the newest streamed content always renders. */
static const char *message_text(const pixel_message_t *message) {
    return message && message->text ? message->text : "";
}

static void message_text_clear(pixel_message_t *message) {
    if (!message)
        return;
    free(message->text);
    message->text = NULL;
    message->text_len = 0;
    message->text_cap = 0;
}

static bool message_text_reserve(pixel_message_t *message, size_t need) {
    if (!message || need == 0 || need > PIXEL_MESSAGE_TEXT_MAX + 1U)
        return false;
    if (message->text_cap >= need)
        return true;
    size_t capacity = message->text_cap ? message->text_cap : 256U;
    while (capacity < need && capacity < PIXEL_MESSAGE_TEXT_MAX + 1U) {
        size_t grown = capacity <= (PIXEL_MESSAGE_TEXT_MAX + 1U) / 2U ? capacity * 2U
                                                                      : PIXEL_MESSAGE_TEXT_MAX + 1U;
        if (grown <= capacity)
            break;
        capacity = grown;
    }
    if (capacity < need)
        capacity = PIXEL_MESSAGE_TEXT_MAX + 1U;
    char *text = realloc(message->text, capacity);
    if (!text)
        return false;
    message->text = text;
    message->text_cap = capacity;
    if (message->text_len == 0)
        message->text[0] = '\0';
    return true;
}

static bool message_text_append(pixel_message_t *message, const char *text, size_t bytes) {
    if (!message || (!text && bytes > 0))
        return false;
    if (bytes == 0)
        return true;
    if (bytes > PIXEL_MESSAGE_TEXT_MAX) {
        text += bytes - PIXEL_MESSAGE_TEXT_MAX;
        bytes = PIXEL_MESSAGE_TEXT_MAX;
        while (bytes > 0 && ((unsigned char)*text & 0xc0) == 0x80) {
            text++;
            bytes--;
        }
    }
    size_t drop = message->text_len + bytes > PIXEL_MESSAGE_TEXT_MAX
                      ? message->text_len + bytes - PIXEL_MESSAGE_TEXT_MAX
                      : 0;
    while (drop < message->text_len && ((unsigned char)message->text[drop] & 0xc0) == 0x80)
        drop++;
    size_t kept = message->text_len - drop;
    if (!message_text_reserve(message, kept + bytes + 1U))
        return false;
    if (drop > 0) {
        memmove(message->text, message->text + drop, kept);
        message->reveal_len = message->reveal_len > drop ? message->reveal_len - drop : 0;
    }
    memcpy(message->text + kept, text, bytes);
    message->text_len = kept + bytes;
    message->text[message->text_len] = '\0';
    if (message->reveal_len > message->text_len)
        message->reveal_len = message->text_len;
    return true;
}

static void message_text_trim_front(pixel_message_t *message, size_t max_len) {
    if (!message || !message->text || message->text_len <= max_len)
        return;
    size_t drop = message->text_len - max_len;
    while (drop < message->text_len && ((unsigned char)message->text[drop] & 0xc0) == 0x80)
        drop++;
    size_t kept = message->text_len - drop;
    memmove(message->text, message->text + drop, kept);
    message->text_len = kept;
    message->text[kept] = '\0';
    message->reveal_len = message->reveal_len > drop ? message->reveal_len - drop : 0;
}

static bool message_text_set_plain(pixel_message_t *message, const char *text) {
    if (!message)
        return false;
    if (!text || !*text) {
        if (message->text)
            message->text[0] = '\0';
        message->text_len = 0;
        message->reveal_len = 0;
        return true;
    }
    size_t bytes = strlen(text);
    if (bytes > PIXEL_MESSAGE_TEXT_MAX) {
        text += bytes - PIXEL_MESSAGE_TEXT_MAX;
        bytes = PIXEL_MESSAGE_TEXT_MAX;
        while (bytes > 0 && ((unsigned char)*text & 0xc0) == 0x80) {
            text++;
            bytes--;
        }
    }
    if (!message_text_reserve(message, bytes + 1U))
        return false;
    message->text_len = plain_text_copy(message->text, message->text_cap, text);
    message->reveal_len = message->text_len;
    return true;
}

static void session_messages_free(void) {
    for (int i = 0; i < PIXEL_MESSAGE_CAP; i++)
        message_text_clear(&g_session.messages[i]);
}

/* ── Tool result projection ───────────────────────────────────────────
 * DSCO_PIXEL_TUI_TOOLS selects how much of each governed tool result the
 * transcript retains: results (default, one line + tail count), calls
 * (status only), full (bounded multiline preview). */
pixel_tui_tool_view_t pixel_tui_tool_view_parse(const char *value) {
    if (!value || !*value || !strcasecmp(value, "results") || !strcasecmp(value, "result") ||
        !strcasecmp(value, "compact"))
        return PIXEL_TUI_TOOL_VIEW_RESULTS;
    if (!strcasecmp(value, "calls") || !strcasecmp(value, "call") || !strcasecmp(value, "status"))
        return PIXEL_TUI_TOOL_VIEW_CALLS;
    if (!strcasecmp(value, "full") || !strcasecmp(value, "verbose") || !strcasecmp(value, "all"))
        return PIXEL_TUI_TOOL_VIEW_FULL;
    return PIXEL_TUI_TOOL_VIEW_RESULTS;
}

const char *pixel_tui_tool_view_name(pixel_tui_tool_view_t view) {
    switch (view) {
        case PIXEL_TUI_TOOL_VIEW_CALLS:
            return "calls";
        case PIXEL_TUI_TOOL_VIEW_FULL:
            return "full";
        case PIXEL_TUI_TOOL_VIEW_RESULTS:
        default:
            return "results";
    }
}

#define PIXEL_TOOL_RESULT_COMPACT_BYTES 240U
#define PIXEL_TOOL_RESULT_FULL_BYTES 2048U
#define PIXEL_TOOL_RESULT_FULL_LINES 10U

void pixel_tui_tool_result_preview(const char *result, pixel_tui_tool_view_t view, char *dst,
                                   size_t cap, uint32_t *tail_lines, uint32_t *total_bytes) {
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (tail_lines)
        *tail_lines = 0;
    if (total_bytes)
        *total_bytes = 0;
    if (!result || !*result)
        return;

    size_t bytes = strlen(result);
    if (total_bytes)
        *total_bytes = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
    if (view == PIXEL_TUI_TOOL_VIEW_CALLS)
        return;
    size_t max_bytes = view == PIXEL_TUI_TOOL_VIEW_FULL ? PIXEL_TOOL_RESULT_FULL_BYTES
                                                        : PIXEL_TOOL_RESULT_COMPACT_BYTES;
    size_t max_lines = view == PIXEL_TUI_TOOL_VIEW_FULL ? PIXEL_TOOL_RESULT_FULL_LINES : 1U;
    size_t consumed = 0, emitted_lines = 0, total_lines = 1;
    bool truncated_line = false;
    for (const char *scan = result; *scan; scan++)
        if (*scan == '\n')
            total_lines++;

    const char *p = result;
    while (*p && emitted_lines < max_lines && consumed < max_bytes) {
        const char *nl = strchr(p, '\n');
        size_t source = nl ? (size_t)(nl - p) : strlen(p);
        size_t room = max_bytes - consumed;
        size_t take = source < room ? source : room;
        while (take > 0 && ((unsigned char)p[take] & 0xc0) == 0x80)
            take--;
        if (take > 0) {
            if (emitted_lines > 0 && consumed + 1U < cap)
                dst[consumed++] = '\n';
            size_t dst_room = cap - consumed - 1U;
            if (take > dst_room)
                take = dst_room;
            while (take > 0 && ((unsigned char)p[take] & 0xc0) == 0x80)
                take--;
            memcpy(dst + consumed, p, take);
            consumed += take;
            dst[consumed] = '\0';
        }
        truncated_line |= take < source;
        emitted_lines++;
        if (!nl || consumed >= max_bytes || consumed + 1U >= cap)
            break;
        p = nl + 1;
    }
    if (truncated_line) {
        /* Byte limits can cut a single JSON line without any hidden-line
         * count. Mark that loss explicitly, inside the existing preview cap. */
        size_t bound = max_bytes < cap - 1U ? max_bytes : cap - 1U;
        if (bound >= 3U) {
            size_t keep = consumed < bound - 3U ? consumed : bound - 3U;
            while (keep > 0 && ((unsigned char)dst[keep] & 0xc0) == 0x80)
                keep--;
            memcpy(dst + keep, "…", 4U);
        }
    }
    if (tail_lines && total_lines > emitted_lines) {
        size_t hidden = total_lines - emitted_lines;
        *tail_lines = hidden > UINT32_MAX ? UINT32_MAX : (uint32_t)hidden;
    }
}

static pixel_message_t *session_find_tool_message(uint64_t operation_id, const char *name) {
    pixel_message_t *latest = NULL;
    for (int i = 0; i < g_session.message_count; i++) {
        int at = (g_session.message_start + i) % PIXEL_MESSAGE_CAP;
        pixel_message_t *message = &g_session.messages[at];
        if (!message->tool_row || message->tool_status >= 0)
            continue;
        if (operation_id && message->tool_operation_id == operation_id)
            return message;
        /* An explicit ID is authoritative, including when its row has
         * already completed or left the ring. Never finish another call. */
        if (!operation_id && name && *name && !strcmp(message->tool_name, name))
            latest = message;
    }
    return latest;
}

/* A card is live while its presence exceeds a whisker: running cards enter
 * over 0.18s (ENTRANCE), finished cards decay their presence (VALUE) to zero
 * over 0.45s so the durable TOOL row can crossfade in underneath. */
static double tool_card_presence(const pixel_tool_visual_t *t) {
    if (!t || !t->used)
        return 0.0;
    uint64_t key = MOTION_KEY_TOOL(t->sequence);
    if (t->status == PIXEL_OP_RUNNING)
        return clamp01(session_motion_value(key, MOTION_PROP_ENTRANCE, 1.0));
    return clamp01(session_motion_value(key, MOTION_PROP_VALUE, 0.0));
}

/* Collect live cards sorted by sequence ascending (oldest first). Returns
 * the total live count; out/presences carry up to cap entries. */
static int session_live_op_cards(const pixel_tool_visual_t **out, double *presences, int cap) {
    int count = 0;
    for (int i = 0; i < PIXEL_TOOL_VIS_CAP && count < cap; i++) {
        const pixel_tool_visual_t *t = &g_session.tool_visuals[i];
        double presence = tool_card_presence(t);
        if (presence <= 0.02)
            continue;
        int at = count;
        while (at > 0 && out[at - 1]->sequence > t->sequence) {
            out[at] = out[at - 1];
            presences[at] = presences[at - 1];
            at--;
        }
        out[at] = t;
        presences[at] = presence;
        count++;
    }
    return count;
}

static int live_op_card_height(double presence, bool compact) {
    /* Fade in place. Height animation reflowed the entire transcript on
     * every tick, making a tool batch look like jumping, disappearing text. */
    return presence > 0.02 ? (compact ? LIVE_OP_CARD_COMPACT_H : LIVE_OP_CARD_H) : 0;
}

/* Plan the deck pinned at the transcript tail: newest LIVE_OP_CARD_MAX cards,
 * stable card heights, clamped to a third of the transcript. On a tight
 * clamp cards drop to compact single-line form before shedding entries, so
 * live status survives even in short viewports. Returns the deck height. */
static int live_op_deck_plan(const double *presences, int total, int avail_h, int *shown_out,
                             bool *compact_out) {
    int limit = avail_h / 3;
    for (int shown = total > LIVE_OP_CARD_MAX ? LIVE_OP_CARD_MAX : total; shown > 0; shown--) {
        for (int pass = 0; pass < 2; pass++) {
            bool compact = pass == 1;
            int deck = total > shown ? live_op_chip_height() : 0;
            for (int i = total - shown; i < total; i++) {
                int card_h = live_op_card_height(presences[i], compact);
                if (card_h > 0)
                    deck += card_h + LIVE_OP_CARD_GAP;
            }
            if (deck <= limit) {
                *shown_out = shown;
                *compact_out = compact;
                return deck;
            }
        }
    }
    *shown_out = 0;
    *compact_out = false;
    return 0;
}

/* One live-op card. Returns the stable pixel height consumed. */
static int draw_live_op_card(px_canvas_t *c, int x, int y, int w, const pixel_tool_visual_t *t,
                             double presence, bool compact) {
    int card_h = live_op_card_height(presence, compact);
    if (!c || !t || card_h < 2 || w < 90)
        return card_h;
    px_color_t accent = tool_accent_color(t->name);
    px_color_t status = tool_visual_color(t->status);
    /* Flat, bounded cards can be redrawn without disturbing transcript
     * pixels. Opacity supplies arrival feedback without layout motion. */
    fill_rounded(c, x, y, w, card_h, 6, C_PANEL_ALT, 0.55 * presence);
    stroke_rounded(c, x, y, w, card_h, 6, C_DIM, 0.26);
    fill_rect(c, x, y, 3, card_h, accent, 0.80 * presence);
    if (card_h >= (compact ? 15 : 18)) {
        int row_y = compact ? y + (card_h - 12) / 2 : y + 4;
        int mark_y = compact ? y + card_h / 2 : y + 11;
        if (t->status == PIXEL_OP_RUNNING) {
            /* Spinner arc; motion_phase returns 0 under reduced motion, so
             * the arc parks as a static open ring — informational either way. */
            double phase = motion_phase(1.1, (double)(t->sequence & 7u) * 0.13);
            double a0 = phase * 6.28318530717958647692;
            draw_ring_arc(c, x + 14, mark_y, 5, a0, a0 + 4.2, accent, 0.90 * presence);
        } else {
            fill_circle(c, x + 14, mark_y, 3, status, 0.90 * presence);
        }
        char elapsed[32];
        double seconds =
            t->status == PIXEL_OP_RUNNING ? monotonic_s() - t->started_s : t->elapsed_ms / 1000.0;
        if (seconds < 0.0)
            seconds = 0.0;
        if (seconds < 60.0)
            snprintf(elapsed, sizeof(elapsed), "%.1fs", seconds);
        else
            format_elapsed(elapsed, sizeof(elapsed), seconds);
        int elapsed_w = font_compat_measure_utf8(elapsed, text_point_size(1), false);
        if (elapsed_w < 0)
            elapsed_w = (int)strlen(elapsed) * 6;
        int elapsed_x = x + w - 10 - elapsed_w;
        if (elapsed_x < x + 24)
            elapsed_x = x + 24;
        draw_text(c, elapsed_x, row_y, 1, elapsed, t->status == PIXEL_OP_RUNNING ? C_TEXT : status,
                  0.84 * presence, x + w - 6 - elapsed_x);
        int name_w = elapsed_x - 8 - (x + 24);
        if (name_w > 12)
            draw_text_ellipsis(c, x + 24, row_y, 1, t->name, accent, 0.92 * presence, name_w);
        if (!compact && card_h >= LIVE_OP_CARD_H - 4 && t->preview[0])
            draw_text_ellipsis(c, x + 24, y + 20, 1, t->preview, C_DIM, 0.66 * presence, w - 34);
    }
    /* Completion wash: the same one-breath exhale the rail summary uses. */
    double flash = session_motion_value(MOTION_KEY_TOOL(t->sequence), MOTION_PROP_FLASH, 0.0);
    if (flash > 0.01)
        fill_rounded(c, x, y, w, card_h, 6, status, 0.16 * flash);
    if (t->status == PIXEL_OP_RUNNING)
        draw_motion_sweep(c, x + 3, y + card_h - 2, w - 6, 2, accent, 1.40, 0.50 * presence);
    return card_h;
}

static void draw_live_op_deck(px_canvas_t *c, int x, int y, int w,
                              const pixel_tool_visual_t *const *cards, const double *presences,
                              int total, int shown, bool compact) {
    int yy = y;
    for (int i = total - shown; i < total; i++) {
        int card_h = draw_live_op_card(c, x, yy, w, cards[i], presences[i], compact);
        if (card_h > 0)
            yy += card_h + LIVE_OP_CARD_GAP;
    }
    if (total > shown) {
        char chip[40];
        snprintf(chip, sizeof(chip), "+%d MORE RUNNING", total - shown);
        draw_text(c, x + 10, yy, 1, chip, C_DIM, 0.62, w - 20);
    }
}

#define PIXEL_VISUAL_RUN_CAP 12
#define PIXEL_VISUAL_RUN_TEXT 160

typedef struct {
    char text[PIXEL_VISUAL_RUN_TEXT];
    rich_style_t style;
    uint8_t level;
} pixel_visual_run_t;

typedef struct {
    pixel_visual_run_t runs[PIXEL_VISUAL_RUN_CAP];
    int run_count;
    int char_count;
    char role[20];
    bool first;
    bool streaming;
    bool rule;
    bool tool_row;
    int8_t tool_status;
    int turn;
    int indent;
    uint64_t sequence;
    rich_style_t block_style;
} pixel_visual_line_t;

/* Wrapping is serialized by g_session_mutex. Reuse the large rich-line slab
 * across stream frames instead of calloc/free churn at up to 30 Hz. */
static pixel_visual_line_t *s_visual_lines;
static int s_visual_line_cap;
/* Bumped whenever a message OTHER than the newest mutates in place (a tool
 * row completing out of order, an older reveal advancing). The newest message
 * is always rebuilt, so it never needs the epoch. */
static uint64_t s_transcript_epoch;
static uint64_t s_visual_cached_epoch;
static int s_visual_cached_chars;
static int s_visual_cached_width;
static int s_visual_cached_message_start;
static int s_visual_cached_message_count;
static int s_visual_cached_line_count;
static uint64_t s_visual_cached_last_sequence;
static pixel_message_t s_visual_cached_last_message;
static char s_visual_cached_last_text[PIXEL_MESSAGE_TEXT_MAX + 1];
static bool s_visual_cached_last_valid;

/* Rich tokens are nearly 400 bytes each. A fixed 512-token stack array both
 * consumed ~200 KiB per repaint and truncated hosted responses once their
 * Markdown crossed that boundary. Keep one renderer-owned slab and grow it
 * geometrically only when the current response needs more semantic tokens. */
#define PIXEL_RICH_TOKEN_MIN 512U
#define PIXEL_RICH_TOKEN_MAX 32768U
static rich_token_t *s_rich_tokens;
static size_t s_rich_token_cap;

static bool rich_tokens_acquire(size_t requested) {
    if (requested < PIXEL_RICH_TOKEN_MIN)
        requested = PIXEL_RICH_TOKEN_MIN;
    if (requested > PIXEL_RICH_TOKEN_MAX)
        requested = PIXEL_RICH_TOKEN_MAX;
    if (s_rich_token_cap >= requested)
        return true;
    rich_token_t *grown = realloc(s_rich_tokens, requested * sizeof(*s_rich_tokens));
    if (!grown)
        return false;
    s_rich_tokens = grown;
    s_rich_token_cap = requested;
    return true;
}

static size_t rich_tokens_parse(const char *text, size_t text_len,
                                const rich_token_t **out_tokens) {
    if (!text || !out_tokens)
        return 0;
    size_t requested = PIXEL_RICH_TOKEN_MIN;
    size_t estimate = text_len / 24U + 128U;
    while (requested < estimate && requested < PIXEL_RICH_TOKEN_MAX)
        requested *= 2U;
    if (requested > PIXEL_RICH_TOKEN_MAX)
        requested = PIXEL_RICH_TOKEN_MAX;
    if (!rich_tokens_acquire(requested) && !s_rich_tokens) {
        *out_tokens = NULL;
        return 0;
    }

    size_t count = 0;
    for (;;) {
        count = rich_text_parse(text, s_rich_tokens, s_rich_token_cap);
        if (count < s_rich_token_cap || s_rich_token_cap >= PIXEL_RICH_TOKEN_MAX)
            break;
        size_t next = s_rich_token_cap <= PIXEL_RICH_TOKEN_MAX / 2U ? s_rich_token_cap * 2U
                                                                    : PIXEL_RICH_TOKEN_MAX;
        if (!rich_tokens_acquire(next))
            break;
    }
    *out_tokens = s_rich_tokens;
    return count;
}

/* Estimate wrapped-line demand from actual text volume so long responses
 * never hit an arbitrary line ceiling; newline-dense output (code, logs)
 * contributes its hard breaks explicitly. */
static int session_visual_line_capacity(int chars) {
    size_t columns = chars >= 8 ? (size_t)chars : 8U;
    size_t estimate = 32U + (size_t)g_session.message_count * 4U;
    for (int i = 0; i < g_session.message_count; i++) {
        int at = (g_session.message_start + i) % PIXEL_MESSAGE_CAP;
        const pixel_message_t *message = &g_session.messages[at];
        estimate += message->text_len / columns + 2U;
        for (const char *p = message_text(message); *p; p++)
            if (*p == '\n')
                estimate++;
    }
    if (estimate < 256U)
        estimate = 256U;
    if (estimate > 65536U)
        estimate = 65536U;
    return (int)estimate;
}

static void visual_cache_invalidate(void) {
    s_visual_cached_last_valid = false;
    s_visual_cached_chars = 0;
    s_visual_cached_message_start = 0;
    s_visual_cached_message_count = 0;
    s_visual_cached_line_count = 0;
    s_visual_cached_last_sequence = 0;
}

static pixel_visual_line_t *visual_lines_acquire(int cap) {
    if (cap <= 0)
        return NULL;
    if (s_visual_line_cap < cap) {
        pixel_visual_line_t *grown = realloc(s_visual_lines, (size_t)cap * sizeof(*s_visual_lines));
        if (!grown)
            return NULL;
        s_visual_lines = grown;
        s_visual_line_cap = cap;
    }
    return s_visual_lines;
}

static int utf8_glyph_count(const char *text, size_t bytes) {
    int count = 0;
    for (size_t i = 0; text && i < bytes && text[i]; count++) {
        size_t step = utf8_char_bytes(text + i);
        if (step < 1 || i + step > bytes)
            step = 1;
        i += step;
    }
    return count;
}

static void visual_line_init(pixel_visual_line_t *line, const pixel_message_t *message,
                             bool first) {
    if (!line || !message)
        return;
    memset(line, 0, sizeof(*line));
    snprintf(line->role, sizeof(line->role), "%s", message->role);
    line->first = first;
    line->turn = message->turn;
    line->sequence = message->sequence;
    line->block_style = RICH_STYLE_BODY;
}

static bool visual_line_empty(const pixel_visual_line_t *line) {
    return !line || (line->run_count == 0 && !line->rule);
}

static void visual_run_append(pixel_visual_line_t *line, rich_style_t style, int level,
                              const char *text, size_t bytes) {
    if (!line || !text || bytes == 0)
        return;
    while (bytes > 0) {
        pixel_visual_run_t *run = NULL;
        if (line->run_count > 0) {
            pixel_visual_run_t *last = &line->runs[line->run_count - 1];
            size_t room = sizeof(last->text) - strlen(last->text) - 1;
            if (last->style == style && last->level == level && room >= utf8_char_bytes(text))
                run = last;
        }
        if (!run) {
            if (line->run_count >= PIXEL_VISUAL_RUN_CAP)
                return;
            run = &line->runs[line->run_count++];
            memset(run, 0, sizeof(*run));
            run->style = style;
            run->level = (uint8_t)level;
        }
        size_t used = strlen(run->text);
        size_t room = sizeof(run->text) - used - 1;
        size_t take = bytes < room ? bytes : room;
        while (take > 0 && ((unsigned char)text[take] & 0xc0) == 0x80)
            take--;
        if (take == 0) {
            /* The previous run can have one or two bytes left while the next
             * glyph needs three or four. Never retry the same full run: that
             * was a renderer hot-loop under Unicode tool output. */
            return;
        }
        memcpy(run->text + used, text, take);
        run->text[used + take] = '\0';
        text += take;
        bytes -= take;
    }
}

static pixel_visual_line_t *visual_next_line(pixel_visual_line_t *lines, int *count, int cap,
                                             const pixel_message_t *message, bool *first) {
    if (!lines || !count || *count >= cap)
        return NULL;
    pixel_visual_line_t *line = &lines[(*count)++];
    visual_line_init(line, message, first ? *first : false);
    if (first)
        *first = false;
    return line;
}

static void tool_meta_format(const pixel_message_t *message, char *dst, size_t cap) {
    dst[0] = '\0';
    size_t at = 0;
    if (message->tool_elapsed_ms > 0.0f) {
        if (message->tool_elapsed_ms < 1000.0f)
            at +=
                (size_t)snprintf(dst + at, cap - at, " · %.0fms", (double)message->tool_elapsed_ms);
        else
            at += (size_t)snprintf(dst + at, cap - at, " · %.1fs",
                                   (double)message->tool_elapsed_ms / 1000.0);
    }
    if (at < cap && message->tool_total_bytes > 1024U)
        at += (size_t)snprintf(dst + at, cap - at, " · %.1fKB",
                               (double)message->tool_total_bytes / 1024.0);
    else if (at < cap && message->tool_total_bytes > 0U && message->tool_status >= 0)
        at += (size_t)snprintf(dst + at, cap - at, " · %uB", message->tool_total_bytes);
}

/* One compact row per governed tool call: status glyph, name, argument
 * preview, timing, then a bounded dim result excerpt. Raw markdown parsing
 * is skipped — tool output renders literally. */
static int wrap_tool_message(const pixel_message_t *message, int chars, pixel_visual_line_t *lines,
                             int count, int cap) {
    bool first = true;
    pixel_visual_line_t *line = visual_next_line(lines, &count, cap, message, &first);
    if (!line)
        return count;
    line->tool_row = true;
    line->tool_status = message->tool_status;
    const char *glyph = message->tool_status < 0 ? "◈ " : message->tool_status > 0 ? "✓ " : "✗ ";
    visual_run_append(line, RICH_STYLE_LIST_MARKER, 0, glyph, strlen(glyph));
    line->char_count += 2;
    visual_run_append(line, RICH_STYLE_CODE, 0, message->tool_name, strlen(message->tool_name));
    line->char_count += utf8_glyph_count(message->tool_name, strlen(message->tool_name));
    if (message->detail[0]) {
        visual_run_append(line, RICH_STYLE_MUTED, 0, "  ", 2);
        line->char_count += 2;
        int room = chars - line->char_count - 14;
        if (room > 8) {
            char preview[sizeof(message->detail)];
            plain_text_copy(preview, sizeof(preview), message->detail);
            size_t take = strlen(preview);
            int glyphs = utf8_glyph_count(preview, take);
            while (glyphs > room && take > 0) {
                do
                    take--;
                while (take > 0 && ((unsigned char)preview[take] & 0xc0) == 0x80);
                glyphs--;
            }
            visual_run_append(line, RICH_STYLE_MUTED, 0, preview, take);
            line->char_count += glyphs;
        }
    }
    char meta[48];
    tool_meta_format(message, meta, sizeof(meta));
    if (meta[0]) {
        visual_run_append(line, RICH_STYLE_MUTED, 0, meta, strlen(meta));
        line->char_count += utf8_glyph_count(meta, strlen(meta));
    }

    const char *text = message_text(message);
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        /* The preview is already bounded at capture time. Soft-wrap its
         * retained bytes instead of dropping the rest of a long JSON/log
         * line, which could hide the archive key or the actual outcome. */
        size_t offset = 0;
        do {
            line = visual_next_line(lines, &count, cap, message, &first);
            if (!line)
                return count;
            line->tool_row = true;
            line->tool_status = message->tool_status;
            line->indent = 1;
            line->block_style = RICH_STYLE_MUTED;
            size_t take = 0;
            int glyphs = 0;
            int columns = chars > 2 ? chars - 2 : 1;
            while (offset + take < len && glyphs < columns) {
                size_t step = utf8_char_bytes(p + offset + take);
                if (step > len - offset - take)
                    step = 1;
                take += step;
                glyphs++;
            }
            visual_run_append(line, RICH_STYLE_MUTED, 0, p + offset, take);
            line->char_count = glyphs;
            offset += take;
        } while (offset < len);
        if (!nl)
            break;
        p = nl + 1;
    }
    if (message->tool_tail_lines > 0) {
        line = visual_next_line(lines, &count, cap, message, &first);
        if (!line)
            return count;
        line->tool_row = true;
        line->tool_status = message->tool_status;
        line->indent = 1;
        line->block_style = RICH_STYLE_MUTED;
        char tail[64];
        snprintf(tail, sizeof(tail), "… +%u lines", message->tool_tail_lines);
        visual_run_append(line, RICH_STYLE_MUTED, 0, tail, strlen(tail));
        line->char_count += utf8_glyph_count(tail, strlen(tail));
    }
    if (count > 0)
        lines[count - 1].streaming = message->tool_status < 0;
    return count;
}

static float rich_style_size(rich_style_t style, int level);
static void rich_style_traits(rich_style_t style, bool *bold, bool *italic);

/* Measure styled spans before breaking a word. Character counts alone cannot
 * lay out proportional prose, especially headings, CJK, or mixed code. */
static int rich_span_width(rich_style_t style, int level, const char *text, size_t bytes) {
    bool bold = false, italic = false;
    rich_style_traits(style, &bold, &italic);
    int width = 0;
    while (bytes) {
        char span[PIXEL_VISUAL_RUN_TEXT];
        size_t take = bytes < sizeof(span) - 1 ? bytes : sizeof(span) - 1;
        while (take && ((unsigned char)text[take] & 0xc0) == 0x80) take--;
        if (!take) break;
        memcpy(span, text, take);
        span[take] = '\0';
        int measured;
        if (style == RICH_STYLE_MATH || style == RICH_STYLE_MATH_DISPLAY)
            measured = font_compat_measure_math_utf8(span, rich_style_size(style, level), bold);
        else if (style == RICH_STYLE_CODE)
            measured = font_compat_measure_utf8_styled(span, rich_style_size(style, level), bold, italic);
        else
            measured = font_compat_measure_prose_utf8(span, rich_style_size(style, level), bold, italic);
        width += measured > 0 ? measured : utf8_glyph_count(span, take) * 8;
        text += take;
        bytes -= take;
    }
    return width;
}

static int wrap_rich_message_width(const pixel_message_t *message, int chars, int max_px, pixel_visual_line_t *lines,
                             int count, int cap) {
    if (!message || !lines || count >= cap || chars < 8)
        return count;
    if (message->tool_row)
        return wrap_tool_message(message, chars, lines, count, cap);
    const char *text = message_text(message);
    if (!text[0] && !message->detail[0] && !strcasecmp(message->role, "THINKING"))
        return count;
    /* Gradual reveal: while text is still surfacing, parse only the revealed
     * prefix. The buffer is owned by the session and mutated under the same
     * lock the renderer holds, so a transient NUL is safe. */
    size_t limit = message->reveal_pending && message->reveal_len < message->text_len
                       ? message->reveal_len
                       : message->text_len;
    char saved = 0;
    char *mutable_text = (char *)text;
    bool truncated = text[0] && limit < message->text_len;
    if (truncated) {
        saved = mutable_text[limit];
        mutable_text[limit] = '\0';
    }
    const rich_token_t *tokens = NULL;
    size_t token_count = rich_tokens_parse(text, limit, &tokens);
    if (truncated)
        mutable_text[limit] = saved;
    bool first = true;
    int used_px = 0;
    int message_start = count;
    pixel_visual_line_t *line = (used_px = 0, visual_next_line(lines, &count, cap, message, &first));
    if (!line)
        return count;

    if (message->detail[0]) {
        char detail[sizeof(message->detail)];
        plain_text_copy(detail, sizeof(detail), message->detail);
        visual_run_append(line, RICH_STYLE_CODE, 0, detail, strlen(detail));
        line->char_count += utf8_glyph_count(detail, strlen(detail));
        used_px += rich_span_width(RICH_STYLE_CODE, 0, detail, strlen(detail));
        if (text[0]) {
            visual_run_append(line, RICH_STYLE_MUTED, 0, "  /  ", 5);
            line->char_count += 5;
            used_px += rich_span_width(RICH_STYLE_MUTED, 0, "  /  ", 5);
        }
    }

    /* Whitespace belongs to the source, not to a style run. Keep it across
     * token boundaries so **cloudy**, and pre**fix**ed stay intact. */
    int pending_spaces = 0;
    for (size_t ti = 0; ti < token_count && line; ti++) {
        const rich_token_t *token = &tokens[ti];
        if (token->type == RICH_TOKEN_BREAK) {
            pending_spaces = 0;
            if (!visual_line_empty(line) ||
                (ti + 1 < token_count && tokens[ti + 1].type == RICH_TOKEN_BREAK))
                line = (used_px = 0, visual_next_line(lines, &count, cap, message, &first));
            continue;
        }
        if (token->type == RICH_TOKEN_RULE) {
            pending_spaces = 0;
            if (!visual_line_empty(line))
                line = (used_px = 0, visual_next_line(lines, &count, cap, message, &first));
            if (!line)
                break;
            line->rule = true;
            line->block_style = RICH_STYLE_MUTED;
            continue;
        }
        if (token->block_start) {
            pending_spaces = 0;
            if (!visual_line_empty(line))
                line = (used_px = 0, visual_next_line(lines, &count, cap, message, &first));
        }
        if (!line)
            break;
        line->indent = token->indent;
        if ((token->style == RICH_STYLE_CODE && token->block_start) || token->style == RICH_STYLE_MATH_DISPLAY ||
            token->style == RICH_STYLE_HEADING || token->style == RICH_STYLE_QUOTE ||
            token->style == RICH_STYLE_LIST_MARKER)
            line->block_style = token->style;

        rich_style_t continuation_style = line->block_style;
        const char *p = token->text;
        while (*p && line) {
            while (*p == ' ' || *p == '\t') {
                pending_spaces += *p == '\t' ? 4 : 1;
                p++;
            }
            if (!*p)
                break;
            const char *word = p;
            while (*p && *p != ' ' && *p != '\t')
                p += utf8_char_bytes(p);
            size_t bytes = (size_t)(p - word);
            int glyphs = utf8_glyph_count(word, bytes);
            /* A Markdown boundary can split a single word or detach its
             * punctuation. Reserve its contiguous suffix before wrapping. */
            int suffix_glyphs = 0;
            int suffix_px = 0;
            if (!*p) {
                for (size_t next = ti + 1; next < token_count; next++) {
                    const rich_token_t *tail = &tokens[next];
                    if (tail->type != RICH_TOKEN_TEXT || tail->block_start)
                        break;
                    const char *q = tail->text;
                    while (*q && *q != ' ' && *q != '\t') {
                        suffix_glyphs++;
                        q += utf8_char_bytes(q);
                    }
                    if (max_px > 0) suffix_px += rich_span_width(tail->style, tail->level, tail->text, (size_t)(q - tail->text));
                    if (*q || suffix_glyphs >= chars)
                        break;
                }
            }
            int spaces = line->char_count > 0 ? pending_spaces : 0;
            int needed = glyphs + suffix_glyphs + spaces;
            int word_px = max_px > 0 ? rich_span_width(token->style, token->level, word, bytes) : 0;
            int space_px = max_px > 0 ? rich_span_width(token->style, token->level, " ", 1) : 0;
            int available_px = max_px - token->indent * 10 - 4;
            bool overflow = max_px > 0
                ? used_px + word_px + suffix_px + spaces * space_px > available_px
                : line->char_count + needed > chars;
            if (line->char_count > 0 && overflow) {
                line = (used_px = 0, visual_next_line(lines, &count, cap, message, &first));
                if (!line)
                    break;
                line->indent = token->indent;
                line->block_style = continuation_style;
                spaces = 0;
            }
            for (int si = 0; si < spaces; si++) {
                visual_run_append(line, token->style, token->level, " ", 1);
                line->char_count++;
                used_px += space_px;
            }
            pending_spaces = 0;
            while (bytes > 0 && line) {
                int remaining = max_px > 0 ? 1800 - line->char_count : chars - line->char_count;
                if (remaining < 1) {
                    line = (used_px = 0, visual_next_line(lines, &count, cap, message, &first));
                    if (!line)
                        break;
                    line->indent = token->indent;
                    line->block_style = continuation_style;
                    remaining = max_px > 0 ? 1800 : chars;
                }
                const char *q = word;
                size_t take = 0;
                int taken = 0;
                int taken_px = 0;
                if (max_px > 0 && rich_span_width(token->style, token->level, q, bytes) <= available_px - used_px) {
                    take = bytes;
                    taken = utf8_glyph_count(q, bytes);
                    taken_px = rich_span_width(token->style, token->level, q, take);
                } else while (take < bytes && taken < remaining) {
                    size_t step = utf8_char_bytes(q + take);
                    int glyph_px = max_px > 0 ? rich_span_width(token->style, token->level, q + take, step) : 0;
                    if (max_px > 0 && used_px + taken_px + glyph_px > available_px && (take || used_px)) break;
                    take += step;
                    taken++;
                    taken_px += glyph_px;
                }
                if (!take) {
                    line = (used_px = 0, visual_next_line(lines, &count, cap, message, &first));
                    if (line) { line->indent = token->indent; line->block_style = continuation_style; }
                    continue;
                }
                used_px += taken_px;
                visual_run_append(line, token->style, token->level, q, take);
                line->char_count += taken;
                word += take;
                bytes -= take;
                if (bytes > 0) {
                    line = (used_px = 0, visual_next_line(lines, &count, cap, message, &first));
                    if (line) { line->indent = token->indent; line->block_style = continuation_style; }
                }
            }
        }
    }
    while (count > message_start + 1 && visual_line_empty(&lines[count - 1]))
        count--;
    if (count > message_start)
        lines[count - 1].streaming =
            message->streaming ||
            (message->reveal_pending && message->reveal_len < message->text_len);
    return count;
}

static int __attribute__((unused)) wrap_rich_message(const pixel_message_t *message, int chars, pixel_visual_line_t *lines,
                             int count, int cap) {
    return wrap_rich_message_width(message, chars, 0, lines, count, cap);
}

#define SESSION_TRANSCRIPT_BODY_SIZE 13.0f

static int session_transcript_line_height(void) {
    int mono = font_compat_line_height(SESSION_TRANSCRIPT_BODY_SIZE, false);
    int prose = font_compat_prose_line_height(14.0f, false);
    int measured = mono > prose ? mono : prose;
    return (measured > 0 ? measured : 17) + 1;
}

static float rich_style_size(rich_style_t style, int level) {
    if (style == RICH_STYLE_HEADING)
        return level <= 2 ? 17.0f : 15.0f;
    if (style == RICH_STYLE_MATH_DISPLAY)
        return 15.0f;
    return style == RICH_STYLE_CODE ? SESSION_TRANSCRIPT_BODY_SIZE : 14.0f;
}

static void rich_style_traits(rich_style_t style, bool *bold, bool *italic) {
    if (bold)
        *bold = style == RICH_STYLE_STRONG || style == RICH_STYLE_HEADING ||
                style == RICH_STYLE_MATH_DISPLAY;
    if (italic)
        *italic = style == RICH_STYLE_EMPHASIS || style == RICH_STYLE_QUOTE;
}

static int measure_rich_run(const pixel_visual_run_t *run, bool monospace) {
    if (!run || !run->text[0])
        return 0;
    bool bold = false, italic = false;
    rich_style_traits(run->style, &bold, &italic);
    int width = (run->style == RICH_STYLE_MATH || run->style == RICH_STYLE_MATH_DISPLAY)
                    ? font_compat_measure_math_utf8(run->text,
                                                    (monospace ? SESSION_TRANSCRIPT_BODY_SIZE : rich_style_size(run->style, run->level)), bold)
                    : (monospace || run->style == RICH_STYLE_CODE
                          ? font_compat_measure_utf8_styled(run->text, (monospace ? SESSION_TRANSCRIPT_BODY_SIZE : rich_style_size(run->style, run->level)), bold, italic)
                          : font_compat_measure_prose_utf8(run->text, (monospace ? SESSION_TRANSCRIPT_BODY_SIZE : rich_style_size(run->style, run->level)), bold, italic));
    return width > 0 ? width : (int)strlen(run->text) * 7;
}

static int draw_rich_run(px_canvas_t *c, int x, int y, int max_width, const pixel_visual_run_t *run,
                         int line_h, bool monospace) {
    if (!run || !run->text[0] || max_width < 1)
        return 0;
    bool bold = false, italic = false;
    rich_style_traits(run->style, &bold, &italic);
    px_color_t color = C_TEXT;
    double alpha = 0.91;
    if (run->style == RICH_STYLE_CODE) {
        color = C_TEXT;
        alpha = 0.92;
    } else if (run->style == RICH_STYLE_LINK) {
        color = C_CYAN;
        alpha = 0.96;
    } else if (run->style == RICH_STYLE_MATH || run->style == RICH_STYLE_MATH_DISPLAY) {
        color = C_CYAN;
        alpha = 0.95;
    } else if (run->style == RICH_STYLE_QUOTE || run->style == RICH_STYLE_MUTED ||
               run->style == RICH_STYLE_STRIKE) {
        color = C_DIM;
        alpha = 0.80;
    } else if (run->style == RICH_STYLE_LIST_MARKER) {
        color = C_VIOLET;
        alpha = 0.94;
    }
    double backing = c->backing_scale > 0 ? c->backing_scale : 1.0;
    int advance =
        (run->style == RICH_STYLE_MATH || run->style == RICH_STYLE_MATH_DISPLAY)
            ? font_compat_draw_math_rgb((uint8_t *)c->pixels, c->pixel_width, c->pixel_height,
                                        c->pixel_width * (int)sizeof(px_color_t), device_px(c, x),
                                        device_px(c, y), device_span(c, x, max_width), run->text,
                                        (monospace ? SESSION_TRANSCRIPT_BODY_SIZE : rich_style_size(run->style, run->level)) * (float)backing,
                                        bold, color.r, color.g, color.b, (float)alpha)
            : (monospace || run->style == RICH_STYLE_CODE ? font_compat_draw_rgb_styled : font_compat_draw_prose_rgb)((uint8_t *)c->pixels, c->pixel_width, c->pixel_height,
                                          c->pixel_width * (int)sizeof(px_color_t), device_px(c, x),
                                          device_px(c, y), device_span(c, x, max_width), run->text,
                                          (monospace ? SESSION_TRANSCRIPT_BODY_SIZE : rich_style_size(run->style, run->level)) * (float)backing,
                                          bold, italic, color.r, color.g, color.b, (float)alpha);
    if (advance >= 0)
        advance = logical_advance(c, advance);
    if (advance < 0)
        advance = draw_text(c, x, y, 1, run->text, color, alpha, max_width);
    if (run->style == RICH_STYLE_LINK)
        draw_line(c, x, y + line_h - 3, x + advance, y + line_h - 3, C_CYAN, 0.38);
    else if (run->style == RICH_STYLE_STRIKE)
        draw_line(c, x, y + line_h / 2, x + advance, y + line_h / 2, C_DIM, 0.58);
    return advance;
}

static void draw_session_transcript_lines(px_canvas_t *c, int x, int y, int w, int h, int deck_h) {
    if (!c || w < 80 || h < 20 || g_session.message_count <= 0)
        return;
    if (w > 1200) w = 1200;
    /* Readable at physical 1x on an external 1080p display without doubling
     * the whole layout. Measure leading and wrapping from the rendered font. */
    const float body_size = SESSION_TRANSCRIPT_BODY_SIZE;
    int line_h = session_transcript_line_height();
    int role_w = 44;
    int full_role_w = font_compat_measure_utf8("THINK", 11.0f, true);
    if (full_role_w > 0 && full_role_w + 8 > role_w)
        role_w = full_role_w + 8;
    if (role_w > 54)
        role_w = 54;
    if (role_w > w / 5)
        role_w = w / 5;
    int alphabet_w = font_compat_measure_utf8("abcdefghijklmnopqrstuvwxyz", body_size, false);
    /* Round up: integer truncation used to overestimate capacity by ~20%, so
     * long paragraphs were clipped at the right edge instead of wrapping. */
    int avg_advance = alphabet_w > 0 ? (alphabet_w + 25) / 26 : 6;
    int chars = (w - role_w - 16) / (avg_advance > 0 ? avg_advance : 1);
    if (chars < 12)
        chars = 12;
    /* Wide proportional glyphs can occupy twice the monospace estimate.
     * Reserve enough rows for them so a long unbroken response stays intact. */
    int line_cap = session_visual_line_capacity(chars / 2);
    pixel_visual_line_t *lines = visual_lines_acquire(line_cap);
    if (!lines)
        return;
    int line_count = 0;
    int first_message = 0;
    int last_at = (g_session.message_start + g_session.message_count - 1) % PIXEL_MESSAGE_CAP;
    const pixel_message_t *last_message = &g_session.messages[last_at];
    if (w == s_visual_cached_width && chars == s_visual_cached_chars && s_transcript_epoch == s_visual_cached_epoch &&
        g_session.message_start == s_visual_cached_message_start &&
        g_session.message_count == s_visual_cached_message_count &&
        last_message->sequence == s_visual_cached_last_sequence) {
        /* Animation, scrolling and caret frames do not mutate transcript
         * content. Reuse the newest layout too when both bytes and metadata
         * match; same-length edits and tool/stream status changes still miss. */
        if (s_visual_cached_last_valid &&
            !memcmp(last_message, &s_visual_cached_last_message, sizeof(*last_message)) &&
            !memcmp(message_text(last_message), s_visual_cached_last_text, last_message->text_len)) {
            line_count = s_visual_cached_line_count;
            first_message = g_session.message_count;
        } else {
            while (line_count < s_visual_cached_line_count &&
                   lines[line_count].sequence != last_message->sequence)
                line_count++;
            first_message = g_session.message_count - 1;
        }
    } else if (w == s_visual_cached_width && chars == s_visual_cached_chars && s_transcript_epoch == s_visual_cached_epoch &&
               g_session.message_start == s_visual_cached_message_start &&
               g_session.message_count == s_visual_cached_message_count + 1) {
        line_count = s_visual_cached_line_count;
        first_message = g_session.message_count - 1;
    }
    for (int i = first_message; i < g_session.message_count; i++) {
        int at = (g_session.message_start + i) % PIXEL_MESSAGE_CAP;
        line_count = wrap_rich_message_width(&g_session.messages[at], chars, w - role_w - 16, lines, line_count, line_cap);
    }
    s_visual_cached_chars = chars;
    s_visual_cached_width = w;
    s_visual_cached_epoch = s_transcript_epoch;
    s_visual_cached_message_start = g_session.message_start;
    s_visual_cached_message_count = g_session.message_count;
    s_visual_cached_line_count = line_count;
    s_visual_cached_last_sequence = last_message->sequence;
    if (first_message < g_session.message_count) {
        s_visual_cached_last_valid = last_message->text_len <= PIXEL_MESSAGE_TEXT_MAX;
        if (s_visual_cached_last_valid) {
            memcpy(&s_visual_cached_last_message, last_message, sizeof(*last_message));
            memcpy(s_visual_cached_last_text, message_text(last_message), last_message->text_len + 1);
        }
    }
    /* The live-op deck reserves the transcript tail; visible/first/max_scroll
     * shift accordingly while the wrapped-line cache stays untouched. */
    int visible = (h - 8 - deck_h) / line_h;
    if (visible < 1)
        return;
    int max_scroll = line_count > visible ? line_count - visible : 0;
    if (g_session.transcript_scroll > max_scroll)
        g_session.transcript_scroll = max_scroll;
    if (g_session.transcript_scroll < 0)
        g_session.transcript_scroll = 0;
    int first = line_count > visible ? line_count - visible - g_session.transcript_scroll : 0;
    if (first < 0)
        first = 0;
    int last = first + visible;
    if (last > line_count)
        last = line_count;
    int yy = y + 4;
    for (int i = first; i < last; i++, yy += line_h) {
        px_color_t role_color = message_role_color(lines[i].role);
        /* Arrival: new messages ease in from the left under a wash that
         * decays as the entrance track settles. Reduced-motion timelines
         * resolve instantly, so this branch costs nothing there. */
        double entrance = lines[i].sequence
                              ? session_motion_value(MOTION_KEY_MESSAGE(lines[i].sequence),
                                                     MOTION_PROP_ENTRANCE, 1.0)
                              : 1.0;
        int slide = (int)((1.0 - entrance) * 14.0);
        if (entrance < 1.0)
            fill_rounded(c, x + 4, yy, w - 8, line_h - 2, 4, role_color, 0.10 * (1.0 - entrance));
        if (lines[i].tool_row) {
            /* Tool rows thread under the turn: a continuous dim rail in the
             * gutter instead of a repeated role label, with one status dot
             * per call. The rail links consecutive tool lines so a burst of
             * calls reads as one grouped activity block. */
            px_color_t status_color = lines[i].tool_status < 0   ? C_VIOLET
                                      : lines[i].tool_status > 0 ? C_GREEN
                                                                 : C_RED;
            int rail_x = x + role_w - 14;
            bool prev_tool = i > 0 && lines[i - 1].tool_row;
            bool next_tool = i + 1 < line_count && lines[i + 1].tool_row;
            int rail_top = prev_tool ? yy - 1 : yy + 3;
            int rail_bottom = next_tool ? yy + line_h + 1 : yy + line_h - 4;
            fill_rect(c, rail_x, rail_top, 1, rail_bottom - rail_top, C_DIM, 0.42);
            if (lines[i].first) {
                double dot = lines[i].tool_status < 0 ? 0.55 + motion_pulse(0.9, 0.0) * 0.40 : 0.85;
                fill_rounded(c, rail_x - 2, yy + line_h / 2 - 3, 5, 5, 2, status_color, dot);
            }
        } else if (lines[i].first) {
            fill_rect(c, x + slide, yy + 2, 2, line_h - 5, role_color, 0.74);
            char role_label[64];
            const char *short_role = lines[i].role;
            if (!strcasecmp(short_role, "USER"))
                short_role = "YOU";
            else if (!strcasecmp(short_role, "ASSISTANT"))
                short_role = "DSCO";
            else if (!strcasecmp(short_role, "THINKING"))
                short_role = "THINK";
            else if (!strncasecmp(short_role, "TOOL", 4))
                short_role = "TOOL";
            snprintf(role_label, sizeof(role_label), "%s", short_role);
            double backing = c->backing_scale > 0 ? c->backing_scale : 1.0;
            font_compat_draw_rgb_styled(
                (uint8_t *)c->pixels, c->pixel_width, c->pixel_height,
                c->pixel_width * (int)sizeof(px_color_t), device_px(c, x + 7 + slide), device_px(c, yy),
                device_span(c, x + 7 + slide, role_w - 9), role_label, 11.0f * (float)backing, true, false,
                role_color.r, role_color.g, role_color.b, 0.88f);
        }
        int text_base = x + role_w + lines[i].indent * 10 + slide;
        int text_avail = w - (text_base - x);
        if (text_avail < 8)
            text_avail = 8;
        int line_width = 0;
        for (int ri = 0; ri < lines[i].run_count; ri++)
            line_width += measure_rich_run(&lines[i].runs[ri], lines[i].tool_row);
        int card_w = line_width + 22;
        if (card_w < 160)
            card_w = 160;
        if (lines[i].block_style == RICH_STYLE_MATH_DISPLAY && card_w < 360)
            card_w = 360;
        if (card_w > text_avail)
            card_w = text_avail;
        if (lines[i].block_style == RICH_STYLE_CODE) {
            /* A code block is one continuous surface, not a stack of
             * variable-width pills competing with each line of text. */
            int block_x = x + role_w - 5;
            fill_rect(c, block_x, yy - 2, w - role_w + 5, line_h, C_PANEL_ALT, 0.65);
            fill_rect(c, block_x, yy - 2, 1, line_h, C_DIM, 0.22);
        } else if (lines[i].block_style == RICH_STYLE_MATH_DISPLAY) {
            fill_rounded(c, text_base - 4, yy - 2, card_w, line_h, 4, C_PANEL_ALT, 0.58);
            fill_rect(c, text_base - 3, yy - 1, 1, line_h - 2, C_CYAN, 0.60);
        } else if (lines[i].block_style == RICH_STYLE_QUOTE) {
            fill_rounded(c, text_base - 5, yy - 1, 1, line_h - 2, 1, C_VIOLET, 0.48);
        }
        if (lines[i].rule) {
            draw_line(c, text_base, yy + line_h / 2, text_base + text_avail, yy + line_h / 2, C_DIM,
                      0.30);
        }
        int text_x = text_base;
        if (lines[i].block_style == RICH_STYLE_MATH_DISPLAY && line_width > 0 &&
            line_width + 20 < card_w)
            text_x += (card_w - line_width) / 2 - 4;
        int advance = 0;
        for (int ri = 0; ri < lines[i].run_count; ri++) {
            if (lines[i].tool_row && lines[i].first && ri == 0) {
                /* Status glyph tracks the call outcome, not the marker hue. */
                px_color_t status_color = lines[i].tool_status < 0   ? C_VIOLET
                                          : lines[i].tool_status > 0 ? C_GREEN
                                                                     : C_RED;
                int drawn = draw_text(c, text_x + advance, yy, 1, lines[i].runs[ri].text,
                                      status_color, 0.95, text_avail - advance);
                if (drawn > 0)
                    advance += drawn;
                continue;
            }
            int drawn = draw_rich_run(c, text_x + advance, yy, text_avail - advance,
                                      &lines[i].runs[ri], line_h, lines[i].tool_row);
            if (drawn > 0)
                advance += drawn;
            if (advance >= text_avail)
                break;
        }
        if (lines[i].streaming) {
            double pulse = 0.48 + motion_pulse(0.72, 0.0) * 0.46;
            fill_rect(c, text_x + advance + 3, yy + 2, 2, line_h - 5, role_color, pulse);
        }
    }
}

static void draw_session_transcript(px_canvas_t *c, int x, int y, int w, int h) {
    if (!c || w < 80 || h < 20)
        return;
    /* Live-operation deck: running (and just-finished, still-easing) tools
     * stay pinned at the transcript tail — even before any message exists,
     * and even while the user is scrolled back in history. */
    const pixel_tool_visual_t *cards[PIXEL_TOOL_VIS_CAP];
    double presences[PIXEL_TOOL_VIS_CAP];
    int live_total = session_live_op_cards(cards, presences, PIXEL_TOOL_VIS_CAP);
    int deck_shown = 0;
    bool deck_compact = false;
    int deck_h = live_total > 0
                     ? live_op_deck_plan(presences, live_total, h, &deck_shown, &deck_compact)
                     : 0;
    if (g_session.message_count <= 0 && deck_h <= 0)
        return;
    draw_session_transcript_lines(c, x, y, w, h, deck_h);
    if (deck_h > 0) {
        /* Preserve the real underlay before alpha drawing. Reusing already
         * composited pixels would darken the deck on every animation tick. */
        native_ui_rect_t r = {x + 2, y + h - deck_h, w - 4, deck_h};
        int px = device_px(c, r.x), py = device_px(c, r.y);
        int pw = device_span(c, r.x, r.width), ph = device_span(c, r.y, r.height);
        size_t stride = (size_t)pw * 3U;
        size_t bytes = stride * (size_t)ph;
        if (g_session.active && r.x >= 0 && r.y >= 0 &&
            pw > 0 && ph > 0 && px <= c->pixel_width - pw && py <= c->pixel_height - ph) {
            if (g_session.activity_underlay_cap < bytes) {
                uint8_t *grown = realloc(g_session.activity_underlay, bytes);
                if (grown) {
                    g_session.activity_underlay = grown;
                    g_session.activity_underlay_cap = bytes;
                }
            }
            if (g_session.activity_underlay_cap >= bytes) {
                for (int row = 0; row < ph; row++)
                    memcpy(g_session.activity_underlay + (size_t)row * stride,
                           (uint8_t *)c->pixels +
                           ((size_t)(py + row) * c->pixel_width + px) * 3U,
                           stride);
                g_session.activity_rect = r;
                g_session.activity_avail_h = h;
                g_session.activity_total = live_total;
                g_session.activity_shown = deck_shown;
                g_session.activity_compact = deck_compact;
                g_session.activity_valid = true;
            }
        }
        draw_live_op_deck(c, r.x, r.y, r.width, cards, presences, live_total, deck_shown,
                          deck_compact);
    }
}

static void draw_session_command_help(px_canvas_t *c, int x, int y, int w, int h) {
    if (!c || w < 80 || h < 40 || g_session.command_count < 1)
        return;
    int line_h = font_compat_line_height(text_point_size(1), false);
    if (line_h < 11)
        line_h = 11;
    line_h += 2;
    int heading_h = line_h + 8;
    int footer_h = line_h + 4;
    int rows = (h - heading_h - footer_h) / line_h;
    if (rows < 1)
        return;
    int max_columns = w / 145;
    if (max_columns < 1)
        max_columns = 1;
    if (max_columns > 5)
        max_columns = 5;
    int columns = (g_session.command_count + rows - 1) / rows;
    if (columns > max_columns)
        columns = max_columns;
    if (columns < 1)
        columns = 1;
    int gap = 12;
    int cell_w = (w - gap * (columns - 1)) / columns;
    int visible_count = rows * columns;
    if (visible_count > g_session.command_count)
        visible_count = g_session.command_count;

    char heading[96];
    snprintf(heading, sizeof(heading), "%d COMMANDS / LIVE REGISTRY / TYPE TO FILTER",
             g_session.command_count);
    draw_text(c, x, y, 1, heading, C_TEXT, 0.88, w);

    for (int i = 0; i < visible_count; i++) {
        int column = i / rows;
        int row = i % rows;
        int xx = x + column * (cell_w + gap);
        int yy = y + heading_h + row * line_h;
        if (xx >= x + w)
            break;
        int available = x + w - xx;
        if (available > cell_w)
            available = cell_w;
        if (available < 20)
            continue;
        int max_command_w = available - 4;
        if (available >= 250)
            max_command_w = available * 2 / 5;
        draw_text_ellipsis(c, xx, yy, 1, g_session.commands[i].command, C_CYAN, 0.94,
                           max_command_w);
        if (available >= 250) {
            int desc_x = xx + max_command_w + 8;
            draw_text_ellipsis(c, desc_x, yy, 1, g_session.commands[i].description, C_DIM, 0.76,
                               available - max_command_w - 8);
        }
    }
    if (visible_count < g_session.command_count) {
        char more[96];
        snprintf(more, sizeof(more), "+%d MORE / START TYPING TO CLOSE REGISTRY",
                 g_session.command_count - visible_count);
        draw_text(c, x, y + heading_h + rows * line_h + 2, 1, more, C_AMBER, 0.72, w);
    }
}

#define SESSION_INPUT_TEXT_SCALE 2

static int session_input_line_height(void) {
    int measured = font_compat_line_height(text_point_size(SESSION_INPUT_TEXT_SCALE), false);
    return (measured > 0 ? measured : 15) + 5;
}

static int session_input_columns(int width) {
    int alphabet_w =
        font_compat_measure_utf8("abcdefghijklmnopqrstuvwxyz",
                                text_point_size(SESSION_INPUT_TEXT_SCALE), false);
    int avg_advance = alphabet_w > 0 ? (alphabet_w + 25) / 26 : 12;
    int columns = width / (avg_advance > 0 ? avg_advance : 1);
    if (columns < 1)
        columns = 1;
    return columns;
}

static native_ui_composer_layout_t session_input_layout(int width, int max_rows) {
    int columns = session_input_columns(width);
    return native_ui_composer_layout(g_session.input, g_session.input_cursor, columns, max_rows);
}

static int session_input_visual_rows(int width) {
    return session_input_layout(width, NATIVE_UI_COMPOSER_MAX_ROWS).row_count;
}

typedef struct {
    int deck_y;
    int deck_height;
    int deck_extra;
    native_ui_rect_t composer;
} session_deck_geometry_t;

/* Keep the command deck's geometry in one place.  The immediate full-session
 * renderer and the retained composer fast path must agree exactly or a local
 * patch could leave stale pixels when multiline input changes the deck size. */
static session_deck_geometry_t session_deck_geometry(int width, int height,
                                                     pixel_tui_state_t state) {
    native_ui_agent_shell_layout_t shell = native_ui_agent_shell_layout(width, height);
    bool transcript_focus = state == PIXEL_TUI_REASONING || state == PIXEL_TUI_RESPONDING;
    int outer = shell.outer_margin;
    int inner = transcript_focus ? 8 : shell.inner_padding;
    int header_h = shell.header.height + shell.header.y;
    int deck_h = shell.composer.height + 6;
    int input_rows = session_input_visual_rows(width - (outer + inner) * 2 - 26);
    if (input_rows < 1)
        input_rows = 1;
    int deck_extra = (input_rows - 1) * session_input_line_height();
    int max_deck_h = height - header_h - 54;
    if (max_deck_h < deck_h)
        max_deck_h = deck_h;
    if (deck_h + deck_extra > max_deck_h)
        deck_extra = max_deck_h - deck_h;
    deck_h += deck_extra;
    int deck_y = height - deck_h;
    return (session_deck_geometry_t){
        .deck_y = deck_y,
        .deck_height = deck_h,
        .deck_extra = deck_extra,
        .composer = {outer, deck_y, width - outer * 2, deck_h - 6},
    };
}

static void draw_session_input(px_canvas_t *c, int x, int y, int w, int h) {
    if (w < 40 || h < 8)
        return;
    const int scale = SESSION_INPUT_TEXT_SCALE;
    int line_h = session_input_line_height();
    int max_rows = h / line_h;
    if (max_rows < 1)
        max_rows = 1;
    if (max_rows > NATIVE_UI_COMPOSER_MAX_ROWS)
        max_rows = NATIVE_UI_COMPOSER_MAX_ROWS;
    native_ui_composer_layout_t layout = session_input_layout(w - 8, max_rows);
    int content_h = layout.row_count * line_h;
    int baseline = y + (h - content_h) / 2;
    if (baseline < y)
        baseline = y;
    char row_text[sizeof(g_session.input)];
    for (int i = 0; i < layout.row_count; i++) {
        size_t start = layout.rows[i].byte_start;
        size_t end = layout.rows[i].byte_end;
        size_t n = end > start ? end - start : 0;
        if (n >= sizeof(row_text))
            n = sizeof(row_text) - 1;
        memcpy(row_text, g_session.input + start, n);
        row_text[n] = '\0';
        draw_text(c, x, baseline + i * line_h, scale, row_text, C_TEXT, 0.96, w - 8);
    }
    if (!g_session.input[0])
        draw_ui_label(c, x + 6, baseline, 14.0f, false, "Ask anything, or / for commands",
                       C_DIM, 0.58, w - 14);
    int cursor_visible_row = layout.cursor_row - layout.first_row;
    if (g_session.input_active && cursor_visible_row >= 0 &&
        cursor_visible_row < layout.row_count &&
        session_caret_visible(monotonic_s())) {
        native_ui_composer_row_t row = layout.rows[cursor_visible_row];
        size_t cursor = g_session.input_cursor;
        if (cursor < row.byte_start)
            cursor = row.byte_start;
        if (cursor > row.byte_end)
            cursor = row.byte_end;
        size_t prefix_len = cursor - row.byte_start;
        if (prefix_len >= sizeof(row_text))
            prefix_len = sizeof(row_text) - 1;
        memcpy(row_text, g_session.input + row.byte_start, prefix_len);
        row_text[prefix_len] = '\0';
        int advance = font_compat_measure_utf8(row_text, text_point_size(scale), false);
        if (advance < 0)
            advance = layout.cursor_column * 6;
        int cursor_x = x + advance;
        if (cursor_x > x + w - 3)
            cursor_x = x + w - 3;
        fill_rect(c, cursor_x, baseline + cursor_visible_row * line_h, 2, line_h,
                  session_animated_accent(g_session.state), 0.88);
    }
    if (layout.first_row > 0)
        draw_text(c, x + w - 20, y, 1, "▲", C_DIM, 0.72, 16);
    if (layout.first_row + layout.row_count < layout.total_rows)
        draw_text(c, x + w - 20, y + h - line_h, 1, "▼", C_DIM, 0.72, 16);
}

static px_color_t notice_color(pixel_tui_notice_level_t level) {
    switch (level) {
        case PIXEL_TUI_NOTICE_SUCCESS:
            return C_GREEN;
        case PIXEL_TUI_NOTICE_WARNING:
            return C_AMBER;
        case PIXEL_TUI_NOTICE_ERROR:
            return C_RED;
        case PIXEL_TUI_NOTICE_ACTIVITY:
            return C_VIOLET;
        default:
            return C_CYAN;
    }
}

static bool session_notice_alive(const pixel_notice_t *notice, double now) {
    return notice && notice->used && now - notice->created_s < PIXEL_NOTICE_TTL_S;
}

static bool session_has_live_notice(void) {
    double now = monotonic_s();
    for (int i = 0; i < PIXEL_NOTICE_CAP; i++)
        if (session_notice_alive(&g_session.notices[i], now))
            return true;
    return false;
}

static void draw_session_notices(px_canvas_t *c, int x, int bottom, int w) {
    double now = monotonic_s();
    int toast_w = w < 430 ? w : 430;
    int yy = bottom;
    for (int i = 0; i < PIXEL_NOTICE_CAP; i++) {
        const pixel_notice_t *notice = &g_session.notices[i];
        if (!session_notice_alive(notice, now))
            continue;
        yy -= 28;
        px_color_t color = notice_color(notice->level);
        double age = now - notice->created_s;
        double alpha = age > PIXEL_NOTICE_TTL_S - 1.0 ? PIXEL_NOTICE_TTL_S - age : 1.0;
        if (alpha < 0.0)
            alpha = 0.0;
        fill_rounded(c, x + w - toast_w, yy, toast_w, 23, 5, C_PANEL_ALT, 0.94 * alpha);
        fill_rounded(c, x + w - toast_w + 2, yy + 3, 2, 17, 1, color, 0.86 * alpha);
        draw_text_ellipsis(c, x + w - toast_w + 10, yy + 5, 1, notice->text, color, 0.90 * alpha,
                           toast_w - 18);
    }
}

static void draw_session_composer_menu(px_canvas_t *c, int x, int bottom, int w, int min_y) {
    const pixel_composer_menu_t *menu = &g_session.composer_menu;
    if (menu->kind == PIXEL_TUI_MENU_NONE || menu->count < 1 || w < 120)
        return;
    int line_h = 20;
    int panel_h = 27 + menu->count * line_h;
    int panel_w = w < 760 ? w : 760;
    int y = bottom - panel_h - 6;
    if (y < min_y)
        y = min_y;
    fill_rounded(c, x, y, panel_w, panel_h, 7, C_PANEL_ALT, 0.98);
    stroke_rounded(c, x, y, panel_w, panel_h, 7, C_DIM, 0.32);
    const char *title = menu->kind == PIXEL_TUI_MENU_IMAGES ? "IMAGE PICKER / TAB ATTACHES"
                                                            : "COMMANDS / TAB COMPLETES";
    draw_text(c, x + 10, y + 7, 1, title, C_DIM, 0.75, panel_w - 20);
    for (int i = 0; i < menu->count; i++) {
        int yy = y + 25 + i * line_h;
        const pixel_composer_item_t *item = &menu->items[i];
        bool selected = i == menu->selected;
        if (selected)
            fill_rounded(c, x + 5, yy - 2, panel_w - 10, line_h - 1, 4, C_CYAN, 0.13);
        px_color_t label_color = item->disabled ? C_DIM : (selected ? C_CYAN : C_TEXT);
        draw_text(c, x + 11, yy + 2, 1, selected ? "›" : " ", label_color, 0.90, 10);
        int label_w = panel_w >= 470 ? panel_w * 2 / 5 : panel_w - 34;
        draw_text_ellipsis(c, x + 24, yy + 2, 1, item->label, label_color, selected ? 0.96 : 0.80,
                           label_w);
        if (panel_w >= 470 && item->detail[0])
            draw_text_ellipsis(c, x + 34 + label_w, yy + 2, 1, item->detail, C_DIM, 0.68,
                               panel_w - label_w - 46);
    }
}

static px_color_t modal_color(pixel_tui_modal_kind_t kind) {
    switch (kind) {
        case PIXEL_TUI_MODAL_PERMISSION:
            return C_AMBER;
        case PIXEL_TUI_MODAL_QUESTION:
            return C_CYAN;
        case PIXEL_TUI_MODAL_MENU:
            return C_VIOLET;
        default:
            return C_TEXT;
    }
}

static void draw_session_modal(px_canvas_t *c) {
    const pixel_modal_t *modal = &g_session.modal;
    if (!c || !modal->active)
        return;
    px_color_t accent = modal_color(modal->kind);
    fill_rect(c, 0, 0, c->width, c->height, C_BG_BOTTOM, 0.78);

    int panel_w = c->width - 48;
    if (panel_w > 780)
        panel_w = 780;
    if (panel_w < 280)
        panel_w = c->width - 16;
    int line_h = 30;
    int max_visible = (c->height - 150) / line_h;
    if (max_visible < 2)
        max_visible = 2;
    if (max_visible > modal->count)
        max_visible = modal->count;
    int top = 0;
    if (modal->count > max_visible) {
        top = modal->selected - max_visible / 2;
        if (top < 0)
            top = 0;
        if (top > modal->count - max_visible)
            top = modal->count - max_visible;
    }
    int panel_h = 94 + max_visible * line_h;
    if (modal->footer[0])
        panel_h += 24;
    if (panel_h > c->height - 24)
        panel_h = c->height - 24;
    int x = (c->width - panel_w) / 2;
    int y = (c->height - panel_h) / 2;
    draw_panel(c, x, y, panel_w, panel_h, 10, C_PANEL, 0.995);
    fill_rounded(c, x + 4, y + 6, 3, panel_h - 12, 1, accent, 0.86);
    draw_text_ellipsis(c, x + 18, y + 15, 2, modal->title[0] ? modal->title : "DSCO", C_TEXT, 0.96,
                       panel_w - 36);
    if (modal->subtitle[0])
        draw_text_ellipsis(c, x + 18, y + 39, 1, modal->subtitle, C_DIM, 0.78, panel_w - 36);
    draw_line(c, x + 18, y + 61, x + panel_w - 18, y + 61, C_DIM, 0.24);

    int list_y = y + 70;
    for (int i = 0; i < max_visible; i++) {
        int at = top + i;
        const pixel_composer_item_t *item = &modal->items[at];
        int yy = list_y + i * line_h;
        bool selected = at == modal->selected;
        if (selected)
            fill_rounded(c, x + 12, yy - 3, panel_w - 24, line_h - 2, 5, accent, 0.15);
        px_color_t color = item->disabled ? C_DIM : (selected ? accent : C_TEXT);
        draw_text(c, x + 21, yy + 4, 1, selected ? "›" : " ", color, 0.92, 12);
        int label_w = panel_w >= 520 ? panel_w * 2 / 5 : panel_w - 58;
        draw_text_ellipsis(c, x + 38, yy + 4, 1, item->label, color, selected ? 0.98 : 0.82,
                           label_w);
        if (panel_w >= 520 && item->detail[0])
            draw_text_ellipsis(c, x + 48 + label_w, yy + 4, 1, item->detail, C_DIM, 0.70,
                               panel_w - label_w - 72);
    }
    if (top > 0)
        draw_text(c, x + panel_w - 50, list_y - 1, 1, "▲ MORE", C_DIM, 0.66, 42);
    if (top + max_visible < modal->count)
        draw_text(c, x + panel_w - 50, list_y + max_visible * line_h - 12, 1, "▼ MORE", C_DIM, 0.66,
                  42);
    if (modal->footer[0])
        draw_text_ellipsis(c, x + 18, y + panel_h - 22, 1, modal->footer, C_DIM, 0.68,
                           panel_w - 36);
}

/* First live shell region on the retained scene path. Two fixed scene slots
 * preserve stable keyed identity across frames without allocating in the
 * compositor hot path; native_ui_diff supplies semantic damage alongside the
 * existing framebuffer tile diff used by the Kitty transport. */
static native_ui_scene_t s_masthead_scenes[2];
static int s_masthead_scene_index = 0;
static bool s_masthead_scene_valid = false;
static native_ui_scene_t s_composer_scenes[2];
static int s_composer_scene_index = 0;
static bool s_composer_scene_valid = false;
static native_trace_ui_view_t s_trace_ui_view, s_trace_ui_pressed;
static native_ui_rect_t s_trace_ui_rect;
static bool s_trace_ui_armed;

static native_ui_agent_state_t masthead_agent_state(pixel_tui_state_t state) {
    switch (state) {
        case PIXEL_TUI_REASONING:
            return NATIVE_UI_AGENT_REASONING;
        case PIXEL_TUI_EXECUTING:
            return NATIVE_UI_AGENT_EXECUTING;
        case PIXEL_TUI_RESPONDING:
            return NATIVE_UI_AGENT_RESPONDING;
        case PIXEL_TUI_IDLE:
        default:
            return NATIVE_UI_AGENT_IDLE;
    }
}

static bool draw_session_masthead(px_canvas_t *canvas, native_ui_rect_t frame, const char *model,
                                  pixel_tui_state_t state, bool show_compact_metrics,
                                  px_color_t accent);

static bool draw_session_composer(px_canvas_t *canvas, native_ui_rect_t frame,
                                  pixel_tui_state_t state, px_color_t accent, double accent_energy,
                                  int origin_x, int origin_y);

static native_ui_rect_t session_windows_work_area(int width, int height,
                                                  pixel_tui_state_t state) {
    native_ui_agent_shell_layout_t shell = native_ui_agent_shell_layout(width, height);
    session_deck_geometry_t deck = session_deck_geometry(width, height, state);
    int h = deck.deck_y - shell.transcript.y - 6;
    int w = width - shell.outer_margin * 2;
    return (native_ui_rect_t){shell.outer_margin, shell.transcript.y,
                              w > 0 ? w : 0, h > 0 ? h : 0};
}

/* Panel-local canvases provide a real raster clip, including CoreText glyphs
 * which do not use the pixel_fx clip stack. All coordinates remain logical. */
static void session_window_composite(px_canvas_t *dst, const px_canvas_t *src,
                                      int x, int y, native_ui_rect_t clip) {
    int left = x > clip.x ? x : clip.x, top = y > clip.y ? y : clip.y;
    int right = x + src->width < clip.x + clip.width ? x + src->width : clip.x + clip.width;
    int bottom = y + src->height < clip.y + clip.height ? y + src->height : clip.y + clip.height;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > dst->width) right = dst->width;
    if (bottom > dst->height) bottom = dst->height;
    if (right <= left || bottom <= top) return;
    int origin_x = device_px(dst, x), origin_y = device_px(dst, y);
    int px0 = device_px(dst, left), py0 = device_px(dst, top);
    int px1 = device_px(dst, right), py1 = device_px(dst, bottom);
    if (px1 > dst->pixel_width) px1 = dst->pixel_width;
    if (py1 > dst->pixel_height) py1 = dst->pixel_height;
    int sx = px0 - origin_x, sy = py0 - origin_y;
    if (sx < 0 || sy < 0 || sx >= src->pixel_width || sy >= src->pixel_height)
        return;
    int pw = px1 - px0, ph = py1 - py0;
    if (pw > src->pixel_width - sx) pw = src->pixel_width - sx;
    if (ph > src->pixel_height - sy) ph = src->pixel_height - sy;
    if (pw <= 0 || ph <= 0) return;
    for (int row = 0; row < ph; ++row) {
        size_t source_at = (size_t)(sy + row) * src->pixel_width + sx;
        size_t dest_at = (size_t)(py0 + row) * dst->pixel_width + px0;
        size_t bytes = (size_t)pw * sizeof(px_color_t);
        memcpy(dst->pixels + dest_at, src->pixels + source_at, bytes);
    }
}

static void session_window_label(px_canvas_t *c, native_ui_rect_t rect, const char *text,
                                  px_color_t color, double opacity) {
    int line_h = font_compat_line_height(text_point_size(1), false);
    if (line_h < 12) line_h = 12;
    if (rect.height < line_h || rect.width < 8) return;
    draw_text_ellipsis(c, rect.x + 6, rect.y + (rect.height - line_h) / 2,
                       1, text, color, opacity, rect.width - 12);
}

/* Use the model's exact word-wrap iterator so scrolling and painting agree.
 * One scroll unit is an 18px row; content remains pixels, not terminal bytes. */
static int session_window_text(px_canvas_t *c, const char *text, int top, int bottom,
                                int *row, int scroll, px_color_t color) {
    int columns = (c->width - 24) / 8;
    if (columns < 1) return 0;
    if (columns > 240) columns = 240;
    const char *at = text ? text : "";
    const char *start;
    size_t bytes;
    while (native_window_text_next(&at, columns, &start, &bytes)) {
        char line[1024];
        /* The iterator caps rows at 240 codepoints (at most 960 UTF-8 bytes). */
        if (bytes >= sizeof(line)) return 1;
        for (size_t i = 0; i < bytes; ++i) {
            unsigned char ch = (unsigned char)start[i];
            line[i] = ch < 0x20 || ch == 0x7f ? ' ' : (char)ch;
        }
        line[bytes] = '\0';
        int visible_row = (*row)++ - scroll;
        if (visible_row < 0) continue;
        int y = top + visible_row * 18;
        if (y + 18 > bottom) return 1;
        draw_text_ellipsis(c, 12, y, 1, line, color, 0.94, c->width - 24);
    }
    return 0;
}

static void draw_session_window(px_canvas_t *dst, const native_window_t *window,
                                uint64_t focused_id, bool keyboard_focus,
                                native_ui_rect_t work_area, px_color_t accent) {
    native_ui_rect_t r = window->rect;
    if (r.width < 1 || r.height < 1 || r.width > dst->width || r.height > dst->height)
        return;
    double scale = dst->backing_scale > 0 ? dst->backing_scale : 1.0;
    px_canvas_t *c = canvas_acquire_scaled(r.width, r.height, scale);
    if (!c) return;
    /* A translated fractional edge can need the panel's final ceil sample. */
    px_color_t panel = C_PANEL;
    for (size_t i = 0; i < (size_t)c->pixel_width * c->pixel_height; i++)
        c->pixels[i] = panel;
    bool focused = window->id == focused_id;
    px_color_t status = window->status == NATIVE_WINDOW_DONE ? C_GREEN :
        window->status == NATIVE_WINDOW_BLOCKED ? C_AMBER :
        window->status == NATIVE_WINDOW_RUNNING ? accent : C_DIM;
    int title_h = NATIVE_WINDOW_TITLE_HEIGHT;
    int footer_h = window->kind == NATIVE_WINDOW_WORKFLOW ? NATIVE_WINDOW_FOOTER_HEIGHT : 0;
    int footer_y = r.height - footer_h;
    fill_rect(c, 0, 0, r.width, title_h, focused ? C_PANEL_ALT : C_PANEL, 1.0);
    fill_rect(c, 0, 0, r.width, 2, focused ? accent : C_DIM, focused ? 0.92 : 0.28);
    fill_circle(c, 11, title_h / 2 + 1, 3, status, 0.9);
    session_window_label(c, (native_ui_rect_t){17, 0, r.width - 73, title_h},
                         window->title, C_TEXT, focused ? 1.0 : 0.82);
    session_window_label(c, (native_ui_rect_t){r.width - 56, 0, 28, title_h},
                         window->zoomed ? "−" : "+", C_TEXT, 0.9);
    session_window_label(c, (native_ui_rect_t){r.width - 28, 0, 28, title_h},
                         "×", C_TEXT, 0.9);
    draw_line(c, 0, title_h, r.width - 1, title_h, C_DIM, 0.28);

    char detail[192];
    if (window->kind == NATIVE_WINDOW_WORKFLOW)
        snprintf(detail, sizeof(detail), "%s · agent-reported", native_window_status_name(window->status));
    else if (window->kind == NATIVE_WINDOW_BUFFER)
        snprintf(detail, sizeof(detail), "%s · Ctrl-S save · Esc chat",
                 window->save_in_flight ? "Saving" : window->dirty ? "Unsaved edits" :
                 focused && keyboard_focus && window->editor_active ? "Editing" : "Click text to edit");
    else
        snprintf(detail, sizeof(detail), "note · #%llu", (unsigned long long)window->id);
    if(window->kind==NATIVE_WINDOW_BUFFER) {
        size_t line=1,col=1;
        for(size_t at=0;at<window->editor.cursor;at++) {
            if(window->text[at]=='\n'){line++;col=1;}
            else if(((unsigned char)window->text[at]&0xc0)!=0x80)col++;
        }
        size_t selected=window->editor.anchor>window->editor.cursor?window->editor.anchor-window->editor.cursor:window->editor.cursor-window->editor.anchor;
        if(window->searching)snprintf(detail,sizeof(detail),"Find: %.120s · Enter / Esc",window->search_text);
        else snprintf(detail,sizeof(detail),"%s · L%zu C%zu · %zu selected bytes · Ctrl-F find",window->dirty?"Unsaved":"Saved",line,col,selected);
    }
    session_window_label(c, (native_ui_rect_t){6, title_h + 3, r.width - 12, 20}, detail, status, 0.86);
    int row = 0, body_top = title_h + 29, body_bottom = footer_y - 9;
    int scroll = window->scroll > 0 ? window->scroll : 0;
    if(window->kind==NATIVE_WINDOW_BUFFER && window->editor.anchor!=window->editor.cursor) {
        size_t lo=window->editor.anchor<window->editor.cursor?window->editor.anchor:window->editor.cursor;
        size_t hi=window->editor.anchor>window->editor.cursor?window->editor.anchor:window->editor.cursor;
        const char *scan=window->text,*start;size_t bytes;int visual=0;
        int columns=(c->width-24)/8;if(columns<1)columns=1;if(columns>240)columns=240;
        while(native_window_text_next(&scan,columns,&start,&bytes)) {
            int yy=body_top+(visual++-scroll)*18;
            if(yy<body_top)continue;if(yy+18>body_bottom)break;
            size_t off=(size_t)(start-window->text),end=off+bytes;int col=0,first=-1,last=-1;
            for(size_t at=off;at<end;) {
                unsigned char ch=(unsigned char)window->text[at];
                size_t n=ch<128?1:(ch&0xe0)==0xc0?2:(ch&0xf0)==0xe0?3:4;
                if(at>=lo && at<hi){if(first<0)first=col;last=col+1;}at+=n;col++;
            }
            if(end>=lo && end<hi && window->text[end]=='\n'){if(first<0)first=col;last=col+1;}
            if(first>=0)fill_rect(c,12+first*8,yy,(last-first)*8,18,accent,.40);
        }
    }
    bool full = session_window_text(c, window->text, body_top, body_bottom, &row, scroll, C_TEXT);
    if (!full && window->next_step[0]) {
        full = session_window_text(c, "\nNEXT STEP", body_top, body_bottom, &row, scroll, accent);
        if (!full) full = session_window_text(c, window->next_step, body_top, body_bottom, &row, scroll, C_TEXT);
    }
    if (!full && window->evidence[0]) {
        full = session_window_text(c, "\nEVIDENCE", body_top, body_bottom, &row, scroll, C_GREEN);
        if (!full) full = session_window_text(c, window->evidence, body_top, body_bottom, &row, scroll, C_TEXT);
    }
    if (window->kind == NATIVE_WINDOW_BUFFER && focused && keyboard_focus && window->editor_active) {
        int columns = (c->width - 24) / 8;
        int caret_row = 0, caret_column = 0;
        if (native_window_editor_position(window, columns, &caret_row, &caret_column)) {
            int visible_row = caret_row - scroll;
            int caret_x = 12 + caret_column * 8;
            int caret_y = body_top + visible_row * 18;
            if (visible_row >= 0 && caret_y + 17 <= body_bottom && caret_x < c->width - 3) {
                if (caret_x < 12) caret_x = 12;
                fill_rect(c, caret_x, caret_y + 1, 2, 16, accent, 0.98);
            }
        }
    }
    if (full || scroll > 0) {
        char position[32];
        snprintf(position, sizeof(position), "ROW %d", scroll + 1);
        session_window_label(c, (native_ui_rect_t){r.width - 83, footer_y - 19, 79, 18},
                             position, C_DIM, 0.78);
    }
    if (footer_h) {
        static const char *labels[] = {"Continue", "Revise", "Retry", "Inspect"};
        fill_rect(c, 0, footer_y, r.width, footer_h, C_PANEL_ALT, 1.0);
        draw_line(c, 0, footer_y, r.width - 1, footer_y, C_DIM, 0.34);
        for (int i = 0; i < 4; ++i) {
            int x = r.width * i / 4, right = r.width * (i + 1) / 4;
            if (i) draw_line(c, x, footer_y + 6, x, r.height - 6, C_DIM, 0.28);
            session_window_label(c, (native_ui_rect_t){x, footer_y, right - x, footer_h},
                                 labels[i], i == 0 ? accent : C_TEXT, 0.95);
        }
    }
    /* The model gives this 16px corner priority over workflow footer input. */
    for (int i = 0; i < 3; ++i)
        draw_line(c, r.width - 4 - i * 4, r.height - 4, r.width - 4,
                  r.height - 4 - i * 4, focused ? accent : C_DIM, 0.7);
    stroke_rounded(c, 0, 0, r.width, r.height, 4,
                   focused ? accent : C_DIM, focused && keyboard_focus ? 1.0 : 0.58);
    session_window_composite(dst, c, r.x, r.y, work_area);
    free_canvas(c);
}

static bool draw_session_windows(px_canvas_t *c, pixel_tui_state_t state, px_color_t accent) {
    native_ui_rect_t area = session_windows_work_area(c->width, c->height, state);
    native_windows_set_work_area(area);
    if (g_session.command_help_active || !native_windows_visible()) return false;
    native_windows_snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) return false;
    native_windows_snapshot(snapshot); /* model lock released before drawing */
    if (!snapshot->visible || snapshot->count < 1) { free(snapshot); return false; }
    /* Free space and tile gutters belong to the workspace too. The retained
     * transcript becomes visible again when the workspace is hidden. */
    fill_rect(c, area.x, area.y, area.width, area.height, C_BG_TOP, 1.0);
    int toolbar_h = area.height < NATIVE_WINDOWS_TOOLBAR_HEIGHT ? area.height : NATIVE_WINDOWS_TOOLBAR_HEIGHT;
    fill_rounded(c, area.x, area.y, area.width, toolbar_h, 5, C_PANEL_ALT, 1.0);
    int button_w = area.width < 240 ? area.width / 3 : 80;
    int controls_x = area.x + area.width - button_w * 3;
    char label[256];
    snprintf(label, sizeof(label), "WORKSPACE · %d %s%s", snapshot->count,
             snapshot->count == 1 ? "WINDOW" : "WINDOWS",
             snapshot->keyboard_focus ? " · Esc to composer" : " · Ctrl+G to focus");
    const char *status_text = snapshot->feedback[0] ? snapshot->feedback : label;
    px_color_t status_color = snapshot->feedback[0] ? C_AMBER : C_TEXT;
    /* An action acknowledgement must not disappear behind an old completion
     * notice. Errors/warnings still take priority; equal levels are newest first. */
    double now = monotonic_s();
    int notice_priority = snapshot->feedback[0] ? 1 : 0;
    for (int i = 0; i < PIXEL_NOTICE_CAP; ++i) {
        const pixel_notice_t *notice = &g_session.notices[i];
        if (!session_notice_alive(notice, now)) continue;
        int priority = notice->level == PIXEL_TUI_NOTICE_ERROR ? 3 :
                       notice->level == PIXEL_TUI_NOTICE_WARNING ? 2 : 1;
        if (priority <= notice_priority) continue;
        notice_priority = priority;
        status_text = notice->text;
        status_color = notice_color(notice->level);
    }
    session_window_label(c, (native_ui_rect_t){area.x + 3, area.y, controls_x - area.x - 6, toolbar_h},
                         status_text, status_color, 0.95);
    static const char *labels[] = {"Tile", "Cascade", "Hide"};
    for (int i = 0; i < 3; ++i) {
        int x = controls_x + i * button_w;
        draw_line(c, x, area.y + 7, x, area.y + toolbar_h - 7, C_DIM, 0.28);
        session_window_label(c, (native_ui_rect_t){x, area.y, button_w, toolbar_h}, labels[i], C_TEXT, 0.94);
    }
    native_ui_rect_t body = area;
    body.y += NATIVE_WINDOWS_TOOLBAR_HEIGHT;
    body.height -= NATIVE_WINDOWS_TOOLBAR_HEIGHT;
    for (int i = 0; i < snapshot->count && i < NATIVE_WINDOWS_MAX; ++i)
        draw_session_window(c, &snapshot->windows[i], snapshot->focused_id,
                            snapshot->keyboard_focus, body, accent);
    free(snapshot);
    return true;
}

static px_canvas_t *render_session_frame(int width, int height, double backing_scale, int pixel_width,
                                         int pixel_height, const char *model,
                                         pixel_tui_state_t state) {
    px_canvas_t *c = canvas_acquire_device(width, height, pixel_width, pixel_height, backing_scale);
    if (!c)
        return NULL;
    g_session.activity_valid = false;
    canvas_background(c, 0x4453434fU + (uint32_t)state * 101U);
    px_color_t accent = session_animated_accent(state);
    double active_pulse = state == PIXEL_TUI_IDLE ? 0.0 : motion_pulse(0.90, 0.0);

    /* Activity belongs in the status indicator; keep the reading surface still. */

    /* The Kitty surface is the first backend of the shared agent shell, not a
     * second layout system. Breakpoints and regions come from native_ui. */
    native_ui_agent_shell_layout_t shell = native_ui_agent_shell_layout(width, height);
    bool transcript_focus = state == PIXEL_TUI_REASONING || state == PIXEL_TUI_RESPONDING;
    bool wide = shell.shows_inspector && !transcript_focus;
    int outer = shell.outer_margin;
    int inner = transcript_focus ? 8 : shell.inner_padding;
    int header_h = shell.header.height + shell.header.y;
    session_deck_geometry_t deck = session_deck_geometry(width, height, state);
    int deck_extra = deck.deck_extra;
    int transcript_y = shell.transcript.y;
    int transcript_h = shell.transcript.height - deck_extra;
    if (transcript_h < 0)
        transcript_h = 0;
    session_summary_t summary = session_summarize();
    int swarm_active = 0, swarm_errors = 0;
    int swarm_total = session_swarm_counts(&swarm_active, NULL, &swarm_errors);
    int running_tools = session_running_tool_count();
    const pixel_tool_visual_t *latest_tool = session_latest_running_tool();

    native_ui_rect_t masthead_frame = {outer, 4, width - outer * 2, header_h - 4};
    if (!draw_session_masthead(c, masthead_frame, model, state, !wide, accent)) {
        /* Degenerate geometry still gets a usable identity/status surface. */
        draw_panel(c, masthead_frame.x, masthead_frame.y, masthead_frame.width,
                   masthead_frame.height, 9, C_PANEL, 0.94);
        draw_text_ellipsis(c, masthead_frame.x + inner, masthead_frame.y + 12, 1,
                           "DSCO / AGENT WORKSPACE", C_TEXT, 0.92,
                           masthead_frame.width - inner * 2);
    }
    if (state != PIXEL_TUI_IDLE)
        draw_motion_sweep(c, outer + 6, 5, width - outer * 2 - 12, 2, accent, 1.60, 0.52);

    int content_x = outer;
    int rail_w = shell.inspector.width;
    int transcript_w = shell.transcript.width;
    int rail_x = shell.inspector.x;
    if (transcript_focus && shell.shows_inspector)
        transcript_w += shell.gap + shell.inspector.width;
    fill_rect(c, content_x, transcript_y, transcript_w, transcript_h, C_PANEL, 0.20);
    draw_text(c, content_x + inner, transcript_y + 7, 1,
              g_session.command_help_active ? "Commands" : "Conversation", C_DIM, 0.66,
              transcript_w - 180);
    if (!g_session.command_help_active && g_session.transcript_scroll > 0) {
        char scroll_label[48];
        snprintf(scroll_label, sizeof(scroll_label), "SCROLL +%d", g_session.transcript_scroll);
        draw_text(c, content_x + transcript_w - 114, transcript_y + 7, 1, scroll_label, C_AMBER,
                  0.78, 100);
    } else if (!g_session.command_help_active && latest_tool && running_tools > 0) {
        char tool_label[96];
        snprintf(tool_label, sizeof(tool_label), "TOOL  %s", latest_tool->name);
        draw_text_ellipsis(c, content_x + transcript_w - 172, transcript_y + 7, 1, tool_label,
                           C_AMBER, 0.76, 158);
    } else if (!g_session.command_help_active && swarm_total > 0) {
        char swarm_label[80];
        snprintf(swarm_label, sizeof(swarm_label), "SWARM %d/%d", swarm_active, swarm_total);
        draw_text(c, content_x + transcript_w - 92, transcript_y + 7, 1, swarm_label,
                  swarm_errors ? C_RED : C_CYAN, 0.72, 78);
    }
    int transcript_content_y = transcript_y + 21;
    int transcript_content_h = transcript_h - 27;
    if (g_session.command_help_active)
        draw_session_command_help(c, content_x + inner, transcript_y + 21, transcript_w - inner * 2,
                                  transcript_h - 27);
    else
        draw_session_transcript(c, content_x + inner, transcript_content_y,
                                transcript_w - inner * 2, transcript_content_h);
    if (wide)
        draw_session_rail(c, rail_x, transcript_y, rail_w, transcript_h, model, state, &summary);

    bool workspace_drawn = draw_session_windows(c, state, accent);

    int deck_y = deck.deck_y;
    if (g_session.composer_menu.kind != PIXEL_TUI_MENU_NONE)
        draw_session_composer_menu(c, outer, deck_y, width - outer * 2, 4);
    else if (!workspace_drawn)
        draw_session_notices(c, outer, deck_y - 2, width - outer * 2);
    /* The composer's accent spine breathes only while work is queued: a
     * quiet "input is waiting on the agent" signal. */
    double queue_breath = g_session.queue_depth > 0 ? motion_pulse(1.35, 0.2) : 0.0;
    native_ui_rect_t composer_frame = deck.composer;
    double composer_energy = 0.58 + active_pulse * 0.14 + queue_breath * 0.22;
    if (!draw_session_composer(c, composer_frame, state, accent, composer_energy, 0, 0)) {
        draw_panel(c, composer_frame.x, composer_frame.y, composer_frame.width,
                   composer_frame.height, 9, C_PANEL_ALT, 0.90);
        draw_text(c, composer_frame.x + inner, composer_frame.y + 4, 1, "COMPOSER", C_TEXT, 0.72,
                  composer_frame.width - inner * 2);
        draw_session_input(c, composer_frame.x + inner, composer_frame.y + 18,
                           composer_frame.width - inner * 2, composer_frame.height - 24);
    }
    draw_session_modal(c);
    return c;
}

static void free_canvas(px_canvas_t *c) {
    if (!c)
        return;
    (void)pthread_mutex_lock(&g_canvas_pool_mutex);
    for (int i = 0; i < CANVAS_POOL_SLOTS; i++) {
        if (c == &g_canvas_pool[i].canvas) {
            g_canvas_pool[i].in_use = false; /* slab retained for reuse */
            (void)pthread_mutex_unlock(&g_canvas_pool_mutex);
            return;
        }
    }
    (void)pthread_mutex_unlock(&g_canvas_pool_mutex);
    free(c->pixels);
    free(c);
}

static size_t compositor_retained_bytes(void) {
    size_t total = g_session.prev_frame_cap + g_session.activity_underlay_cap;
    if ((size_t)s_visual_line_cap <= SIZE_MAX / sizeof(*s_visual_lines)) {
        size_t visual_bytes = (size_t)s_visual_line_cap * sizeof(*s_visual_lines);
        if (visual_bytes <= SIZE_MAX - total)
            total += visual_bytes;
        else
            total = SIZE_MAX;
    }
    (void)pthread_mutex_lock(&g_canvas_pool_mutex);
    for (int i = 0; i < CANVAS_POOL_SLOTS; i++) {
        if (g_canvas_pool[i].cap <= SIZE_MAX - total)
            total += g_canvas_pool[i].cap;
        else
            total = SIZE_MAX;
    }
    (void)pthread_mutex_unlock(&g_canvas_pool_mutex);
    return total;
}

static bool canvas_write_ppm(const char *path, const px_canvas_t *c) {
    if (!path || !*path || !c || !c->pixels)
        return false;
    char tmp[4096];
    int wrote = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (wrote < 0 || (size_t)wrote >= sizeof(tmp))
        return false;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", c->pixel_width, c->pixel_height);
    size_t count = (size_t)c->pixel_width * (size_t)c->pixel_height;
    bool ok = fwrite(c->pixels, sizeof(px_color_t), count, f) == count;
    if (fclose(f) != 0)
        ok = false;
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        (void)unlink(tmp);
    return ok;
}

bool pixel_tui_write_plan_ppm(const char *path, int plan_id, int width) {
    return pixel_tui_write_plan_view_ppm(path, plan_id, width, PIXEL_PLAN_VIEW_TREE);
}

bool pixel_tui_write_plan_view_ppm(const char *path, int plan_id, int width,
                                   pixel_plan_view_t view) {
    if (!path || !*path)
        return false;
    px_canvas_t *c = view == PIXEL_PLAN_VIEW_ACTIONS ? render_plan_actions_frame(plan_id, width)
                                                     : render_plan_frame(plan_id, width, NULL);
    if (!c)
        return false;
    FILE *f = fopen(path, "wb");
    if (!f) {
        free_canvas(c);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", c->pixel_width, c->pixel_height);
    bool ok =
        fwrite(c->pixels, sizeof(px_color_t), (size_t)c->pixel_width * (size_t)c->pixel_height,
               f) == (size_t)c->pixel_width * (size_t)c->pixel_height;
    if (fclose(f) != 0)
        ok = false;
    free_canvas(c);
    return ok;
}

bool pixel_tui_write_session_ppm(const char *path, int width, int height, const char *model,
                                 pixel_tui_state_t state) {
    if (!path || !*path || width < 320 || height < 180 || state < PIXEL_TUI_IDLE ||
        state > PIXEL_TUI_RESPONDING)
        return false;
    session_lock();
    px_canvas_t *canvas = render_session_frame(width, height, 1, width, height,
                                               model && *model ? model : "native-session", state);
    bool ok = canvas && canvas_write_ppm(path, canvas);
    free_canvas(canvas);
    session_unlock();
    return ok;
}

static void fixture_density_metrics(int width, int height, pixel_tui_density_metrics_t *metrics) {
    if (!metrics)
        return;
    memset(metrics, 0, sizeof(*metrics));
    native_ui_agent_shell_layout_t shell = native_ui_agent_shell_layout(width, height);
    bool transcript_focus =
        g_session.state == PIXEL_TUI_REASONING || g_session.state == PIXEL_TUI_RESPONDING;
    int outer = shell.outer_margin;
    int inner = transcript_focus ? 8 : shell.inner_padding;
    int header_h = shell.header.height + shell.header.y;
    int deck_h = shell.composer.height + 6;
    int input_rows = session_input_visual_rows(width - (outer + inner) * 2 - 26);
    if (input_rows < 1)
        input_rows = 1;
    int deck_extra = (input_rows - 1) * session_input_line_height();
    int max_deck_h = height - header_h - 54;
    if (max_deck_h < deck_h)
        max_deck_h = deck_h;
    if (deck_h + deck_extra > max_deck_h)
        deck_extra = max_deck_h - deck_h;
    int transcript_h = shell.transcript.height - deck_extra;
    if (transcript_h < 0)
        transcript_h = 0;
    int transcript_w = shell.transcript.width;
    if (transcript_focus && shell.shows_inspector)
        transcript_w += shell.gap + shell.inspector.width;
    int content_w = transcript_w - inner * 2;
    if (content_w > 1200) content_w = 1200;
    int content_h = transcript_h - 27;
    int line_h = session_transcript_line_height();
    /* The live-op deck reserves the transcript tail, exactly as the renderer
     * computes it in draw_session_transcript. */
    const pixel_tool_visual_t *cards[PIXEL_TOOL_VIS_CAP];
    double presences[PIXEL_TOOL_VIS_CAP];
    int live_total = session_live_op_cards(cards, presences, PIXEL_TOOL_VIS_CAP);
    int live_shown = 0;
    bool live_compact = false;
    int live_deck_h = live_total > 0 ? live_op_deck_plan(presences, live_total, content_h,
                                                         &live_shown, &live_compact)
                                     : 0;
    int capacity = content_h > 8 ? (content_h - 8 - live_deck_h) / line_h : 0;
    if (capacity < 0)
        capacity = 0;
    int wrapped = s_visual_cached_line_count;
    int visible = wrapped < capacity ? wrapped : capacity;
    int first = wrapped - visible;
    int visible_messages = 0;
    uint64_t prior_sequence = 0;
    size_t visible_chars = 0;
    for (int i = first; i < wrapped; i++) {
        if (s_visual_lines[i].char_count > 0)
            visible_chars += (size_t)s_visual_lines[i].char_count;
        if (s_visual_lines[i].sequence != prior_sequence) {
            visible_messages++;
            prior_sequence = s_visual_lines[i].sequence;
        }
    }
    size_t source_chars = 0;
    for (int i = 0; i < g_session.message_count; i++) {
        int at = (g_session.message_start + i) % PIXEL_MESSAGE_CAP;
        const char *detail = g_session.messages[at].detail;
        const char *text = g_session.messages[at].text;
        int detail_chars = utf8_glyph_count(detail, strlen(detail));
        if (detail_chars > 0 && (size_t)detail_chars <= SIZE_MAX - source_chars)
            source_chars += (size_t)detail_chars;
        if (detail_chars > 0 && text[0] && source_chars <= SIZE_MAX - 5U)
            source_chars += 5U; /* rendered "  /  " detail separator */
        int chars = utf8_glyph_count(text, strlen(text));
        if (chars > 0 && (size_t)chars <= SIZE_MAX - source_chars)
            source_chars += (size_t)chars;
    }
    *metrics = (pixel_tui_density_metrics_t){
        .logical_width = width,
        .logical_height = height,
        .transcript_width = content_w,
        .transcript_height = content_h,
        .wrap_columns = s_visual_cached_chars,
        .line_capacity = capacity,
        .wrapped_lines = wrapped,
        .visible_lines = visible,
        .visible_messages = visible_messages,
        .source_chars = source_chars,
        .visible_chars = visible_chars,
    };
}

bool pixel_tui_write_fixture_ppm(const char *path, int width, int height,
                                 const pixel_tui_fixture_t *fixture,
                                 pixel_tui_density_metrics_t *metrics) {
    if (!path || !*path || width < 320 || height < 180 || !fixture ||
        fixture->state < PIXEL_TUI_IDLE || fixture->state > PIXEL_TUI_RESPONDING ||
        fixture->message_count < 0 || (fixture->message_count > 0 && !fixture->messages) ||
        fixture->tool_count < 0 || (fixture->tool_count > 0 && !fixture->tools))
        return false;
    session_lock();
    if (g_session.active) {
        session_unlock();
        return false;
    }
    session_messages_free();
    free(g_session.activity_underlay);
    memset(&g_session, 0, sizeof(g_session));
    visual_cache_invalidate();
    g_session.state = fixture->state;
    g_session.previous_state = fixture->state;
    g_session.animation_enabled = false;
    g_session.queue_capacity = 8;
    g_session.current_message = -1;
    g_session.started_s = monotonic_s();
    g_session.state_started_s = g_session.started_s;
    g_session.turn_started_s = g_session.started_s;
    g_session.turn = fixture->turn;
    g_session.input_tokens = fixture->input_tokens;
    g_session.output_tokens = fixture->output_tokens;
    g_session.tools_used = fixture->tools_used;
    g_session.cost_usd = fixture->cost_usd;
    g_session.context_percent = fixture->context_percent;
    snprintf(g_session.model, sizeof(g_session.model), "%s",
             fixture->model && *fixture->model ? fixture->model : "native-session");
    snprintf(g_session.slot_name, sizeof(g_session.slot_name), "%s",
             fixture->slot_name ? fixture->slot_name : "native");
    if (fixture->input) {
        plain_text_copy(g_session.input, sizeof(g_session.input), fixture->input);
        size_t input_len = strlen(g_session.input);
        g_session.input_cursor =
            fixture->input_cursor < input_len ? fixture->input_cursor : input_len;
    }
    g_session.input_active = fixture->input_active;
    int count = fixture->message_count;
    if (count > PIXEL_MESSAGE_CAP)
        count = PIXEL_MESSAGE_CAP;
    for (int i = 0; i < count; i++) {
        const pixel_tui_fixture_message_t *source = &fixture->messages[i];
        pixel_message_t *message = &g_session.messages[i];
        message_text_clear(message);
        memset(message, 0, sizeof(*message));
        plain_text_copy(message->role, sizeof(message->role),
                        source->role && *source->role ? source->role : "DSCO");
        if (source->detail)
            plain_text_copy(message->detail, sizeof(message->detail), source->detail);
        if (source->text)
            (void)message_text_set_plain(message, source->text);
        message->turn = fixture->turn;
        message->sequence = (uint64_t)i + 1U;
    }
    g_session.message_count = count;
    g_session.next_sequence = (uint64_t)count + 1U;
    int tool_count = fixture->tool_count;
    if (tool_count > PIXEL_TOOL_VIS_CAP)
        tool_count = PIXEL_TOOL_VIS_CAP;
    for (int i = 0; i < tool_count; i++) {
        const pixel_tui_fixture_tool_t *source = &fixture->tools[i];
        pixel_tool_visual_t *tool = &g_session.tool_visuals[i];
        tool->used = true;
        tool->sequence = (uint64_t)i + 1U;
        tool->status = source->status == 1   ? PIXEL_OP_DONE
                       : source->status == 2 ? PIXEL_OP_ERROR
                                             : PIXEL_OP_RUNNING;
        plain_text_copy(tool->name, sizeof(tool->name),
                        source->name && *source->name ? source->name : "tool");
        if (source->preview)
            plain_text_copy(tool->preview, sizeof(tool->preview), source->preview);
        double elapsed = source->elapsed_s > 0.0 ? source->elapsed_s : 0.0;
        tool->started_s = monotonic_s() - elapsed;
        tool->elapsed_ms = elapsed * 1000.0;
    }
    g_session.next_tool_sequence = (uint64_t)tool_count;
    ui_motion_init(&g_session.motion, true);

    px_canvas_t *canvas =
        render_session_frame(width, height, 1, width, height, g_session.model, fixture->state);
    bool ok = canvas && canvas_write_ppm(path, canvas);
    if (canvas)
        fixture_density_metrics(width, height, metrics);
    free_canvas(canvas);
    session_messages_free();
    free(g_session.activity_underlay);
    memset(&g_session, 0, sizeof(g_session));
    visual_cache_invalidate();
    session_unlock();
    return ok;
}

static bool env_false(const char *name) {
    const char *v = getenv(name);
    return v && (!strcmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no") ||
                 !strcasecmp(v, "off"));
}

static bool env_true(const char *name) {
    const char *v = getenv(name);
    return v &&
           (*v == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on"));
}

static bool native_mouse_enabled(void) {
    /* Terminal selection/copy is the default. Opt into compositor pointer
     * gestures explicitly so mouse tracking cannot steal ordinary highlights. */
    return env_true("DSCO_PIXEL_TUI_MOUSE") || env_true("DSCO_MOUSE");
}

bool pixel_tui_available(FILE *out) {
    /* The pixel compositor is opt-in while it matures: it activates only on
     * an explicit DSCO_PIXEL_TUI=1 (`--native` sets it) in a Kitty-capable
     * terminal. Unset means the established TUI everywhere. */
    return env_true("DSCO_PIXEL_TUI") && kitty_graphics_available(out);
}

static bool send_kitty_pixels(FILE *out, const char *control, const px_canvas_t *c,
                              bool animation_frame, kitty_graphics_send_stats_t *stats) {
    size_t raw_len = (size_t)c->pixel_width * (size_t)c->pixel_height * sizeof(px_color_t);
    kitty_graphics_send_options_t options;
    kitty_graphics_send_options_default(&options);
    options.continuation_control = animation_frame ? "a=f,q=2" : "q=2";
    return kitty_graphics_send_pixels_ex(out, control, c->pixels, raw_len, &options, stats);
}

static void perf_add_transport(pixel_tui_frame_sample_t *sample,
                               const kitty_graphics_send_stats_t *stats,
                               size_t extra_transient_bytes) {
    if (!sample || !stats)
        return;
    sample->has_encode = true;
    sample->has_upload = true;
    sample->chunks += stats->chunks;
    sample->raw_bytes += stats->input_bytes;
    sample->packed_bytes += stats->packed_bytes;
    sample->encoded_bytes += stats->encoded_bytes;
    sample->wire_bytes += stats->wire_bytes;
    sample->encode_ms += stats->compression_ms + stats->base64_ms;
    sample->upload_ms += stats->write_ms;
    size_t transient = stats->peak_heap_bytes;
    if (extra_transient_bytes <= SIZE_MAX - transient)
        transient += extra_transient_bytes;
    else
        transient = SIZE_MAX;
    if (transient > sample->transient_bytes)
        sample->transient_bytes = transient;
}

/* Metadata snapshots are collected under the session lock. The trace module
 * diffs them in memory; its own worker writes the bounded artifact later. */
static double session_repaint_interval_s(void);
static native_trace_component_t *trace_component(native_trace_component_t *all, size_t *count,
                                                  const char *id, native_ui_rect_t r, bool visible) {
    native_trace_component_t *c=&all[(*count)++];
    memset(c,0,sizeof(*c));snprintf(c->id,sizeof(c->id),"%s",id);
    c->value[NT_X]=r.x;c->value[NT_Y]=r.y;c->value[NT_WIDTH]=r.width;c->value[NT_HEIGHT]=r.height;
    c->value[NT_VISIBLE]=visible;
    if(visible && g_session.prev_frame && g_session.prev_frame_width>0 && g_session.prev_frame_height>0) {
        double scale=g_session.backing_scale>0 ? g_session.backing_scale : 1;
        int64_t x=(int64_t)r.x*scale,y=(int64_t)r.y*scale;
        int64_t right=((int64_t)r.x+r.width)*scale,bottom=((int64_t)r.y+r.height)*scale;
        if(x<0)x=0;if(y<0)y=0;
        if(right>g_session.prev_frame_width)right=g_session.prev_frame_width;
        if(bottom>g_session.prev_frame_height)bottom=g_session.prev_frame_height;
        uLong hash=crc32(0,NULL,0);
        if(right>x && bottom>y)for(int64_t row=y;row<bottom;row++)
            hash=crc32(hash,g_session.prev_frame+((size_t)row*g_session.prev_frame_width+(size_t)x)*3U,
                       (uInt)((right-x)*3));
        c->value[NT_PIXELS]=hash;
    }
    return c;
}
static void session_trace_components(void) {
    if(!native_trace_active())return;
    double began=monotonic_s();
    native_trace_component_t all[64];size_t count=0;
    native_ui_agent_shell_layout_t shell=native_ui_agent_shell_layout(g_session.width,g_session.height);
    session_deck_geometry_t deck=session_deck_geometry(g_session.width,g_session.height,g_session.state);
    bool workspace=native_windows_visible(), visible=!g_session.terminal_suspended;
    native_trace_component_t *c=trace_component(all,&count,"viewport",
        (native_ui_rect_t){0,0,g_session.width,g_session.height},false);
    c->value[NT_VISIBLE]=visible;c->value[NT_STATUS]=g_session.state;c->value[NT_COUNT]=g_session.backing_scale;
    c=trace_component(all,&count,"header",shell.header,visible && !g_session.modal.active && !g_session.overlay_image_id);
    c->value[NT_STATUS]=g_session.state;c->value[NT_ANIMATED]=g_session.animation_enabled && g_session.state!=PIXEL_TUI_IDLE;
    c->value[NT_INTERVAL]=(int64_t)ceil(session_repaint_interval_s()*1000.0);
    native_ui_rect_t transcript=shell.transcript;
    bool transcript_focus=g_session.state==PIXEL_TUI_REASONING || g_session.state==PIXEL_TUI_RESPONDING;
    if(transcript_focus && shell.shows_inspector)transcript.width+=shell.gap+shell.inspector.width;
    transcript.height-=deck.deck_extra;
    c=trace_component(all,&count,"transcript",transcript,visible && !workspace && !g_session.modal.active);
    c->value[NT_COUNT]=g_session.message_count;c->value[NT_SCROLL]=g_session.transcript_scroll;
    c->value[NT_PENDING]=g_session.stream_repaint_pending;c->value[NT_REVISION]=g_session.next_sequence;
    c->value[NT_INTERVAL]=(int64_t)ceil(session_repaint_interval_s()*1000.0);
    for(int i=0;i<g_session.message_count;i++) {
        pixel_message_t *m=&g_session.messages[(g_session.message_start+i)%PIXEL_MESSAGE_CAP];
        c->value[NT_BYTES]+=(int64_t)m->text_len;c->value[NT_CURSOR]+=(int64_t)m->reveal_len;
        if(m->reveal_pending && m->reveal_len<m->text_len)c->value[NT_ANIMATED]=1;
    }
    c=trace_component(all,&count,"inspector",shell.inspector,visible && shell.shows_inspector && !transcript_focus && !workspace);
    c->value[NT_STATUS]=g_session.state;
    c=trace_component(all,&count,"composer",deck.composer,visible && !g_session.modal.active);
    c->value[NT_BYTES]=(int64_t)strlen(g_session.input);c->value[NT_CURSOR]=(int64_t)g_session.input_cursor;
    c->value[NT_FOCUSED]=g_session.input_active && !native_windows_focused();
    c->value[NT_COUNT]=g_session.queue_depth;c->value[NT_PENDING]=g_session.composer_repaint_pending;
    c->value[NT_ANIMATED]=g_session.input_active && g_session.animation_enabled;
    c->value[NT_INTERVAL]=620;
    c=trace_component(all,&count,"ui_diagnostics",s_trace_ui_rect,
        visible && !g_session.modal.active && s_trace_ui_rect.width>0);
    c->value[NT_STATUS]=s_trace_ui_view.state;c->value[NT_REVISION]=s_trace_ui_view.generation;
    c->value[NT_PENDING]=native_trace_ui_action_pending();
    c=trace_component(all,&count,"live_tool_deck",g_session.activity_rect,
        visible && g_session.activity_valid && !workspace && !g_session.modal.active && !g_session.scene_image_id);
    c->value[NT_COUNT]=session_running_tool_count();c->value[NT_REVISION]=g_session.next_tool_sequence;
    c->value[NT_ANIMATED]=g_session.animation_enabled && c->value[NT_COUNT]>0;
    c->value[NT_INTERVAL]=g_session.animation_interval_ms;
    int menu_height=27+g_session.composer_menu.count*20;
    c=trace_component(all,&count,"menu",(native_ui_rect_t){deck.composer.x,deck.deck_y-menu_height-6,deck.composer.width,menu_height},
        visible && g_session.composer_menu.kind!=PIXEL_TUI_MENU_NONE);
    c->value[NT_STATUS]=g_session.composer_menu.kind;c->value[NT_COUNT]=g_session.composer_menu.count;
    c->value[NT_CURSOR]=g_session.composer_menu.selected;
    c=trace_component(all,&count,"modal",(native_ui_rect_t){0,0,g_session.width,g_session.height},visible && g_session.modal.active);
    c->value[NT_STATUS]=g_session.modal.kind;c->value[NT_COUNT]=g_session.modal.count;c->value[NT_CURSOR]=g_session.modal.selected;
    c=trace_component(all,&count,"retained_scene",(native_ui_rect_t){0},visible && g_session.scene_placed && !workspace);
    c->value[NT_REVISION]=g_session.scene_generation;c->value[NT_PENDING]=g_session.scene_dirty;
    c->value[NT_WIDTH]=g_session.scene_width;c->value[NT_HEIGHT]=g_session.scene_height;
    for(int i=0;i<PIXEL_TOOL_VIS_CAP && count<sizeof(all)/sizeof(all[0]);i++) {
        const pixel_tool_visual_t *tool=&g_session.tool_visuals[i];
        if(!tool->used)continue;
        char id[64];snprintf(id,sizeof(id),"tool/%llu/%.24s",(unsigned long long)tool->sequence,tool->name);
        c=trace_component(all,&count,id,(native_ui_rect_t){0},false);
        c->value[NT_STATUS]=tool->status;c->value[NT_REVISION]=tool->sequence;
        c->value[NT_PENDING]=tool->status==PIXEL_OP_RUNNING;
    }
    native_windows_snapshot_t *windows=malloc(sizeof(*windows));
    if(windows) {
        native_windows_snapshot(windows);
        for(int i=0;i<windows->count && count<sizeof(all)/sizeof(all[0]);i++) {
            native_window_t *w=&windows->windows[i];char id[64];
            snprintf(id,sizeof(id),"window/%llu",(unsigned long long)w->id);
            c=trace_component(all,&count,id,w->rect,visible && windows->visible && !g_session.modal.active);
            c->value[NT_REVISION]=w->revision;c->value[NT_BYTES]=(int64_t)strlen(w->text);
            c->value[NT_CURSOR]=(int64_t)w->editor.cursor;c->value[NT_SCROLL]=w->scroll;
            c->value[NT_FOCUSED]=windows->keyboard_focus && w->id==windows->focused_id;
            c->value[NT_DIRTY]=w->dirty;c->value[NT_STATUS]=w->kind;c->value[NT_PENDING]=w->save_in_flight;
        }
        free(windows);
    }
    native_trace_components(all,count);
    native_trace_event("trace_sample_cost",(monotonic_s()-began)*1000.0,0,0,0);
}
static void perf_finish_frame(pixel_tui_frame_sample_t *sample, double frame_started_ms) {
    if (!sample || !pixel_tui_perf_enabled())
        return;
    size_t retained = compositor_retained_bytes();
    if (retained > sample->retained_bytes)
        sample->retained_bytes = retained;
    sample->frame_ms = monotonic_s() * 1000.0 - frame_started_ms;
    if (sample->frame_ms < 0.0)
        sample->frame_ms = 0.0;
    pixel_tui_perf_record(sample);
    native_trace_frame(sample);
    session_trace_components();
}

static bool terminal_geometry(FILE *out, int *cols, int *rows, int *pixel_width,
                              int *pixel_height) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(fileno(out), TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0)
        return false;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    *pixel_width = ws.ws_xpixel;
    *pixel_height = ws.ws_ypixel;
    return true;
}

bool pixel_tui_session_active(void) {
    return atomic_load_explicit(&g_session_active_fast, memory_order_acquire);
}

bool pixel_tui_session_terminal_suspended(void) {
    return atomic_load_explicit(&g_session_active_fast, memory_order_acquire) &&
           atomic_load_explicit(&g_session_suspended_fast, memory_order_acquire);
}

/* TIOCGWINSZ reports physical pixels. On HiDPI/Retina backing (2x-3x) the
 * compositor's absolute pixel metrics (glyphs, badges, meters, margins) are
 * tuned for ~1x density, so an unscaled canvas renders at half visual size
 * the moment the terminal lands on a Retina panel (e.g. primary-monitor
 * disconnect). Infer the backing ratio from cell pixel density and compose
 * in logical pixels; the Kitty placement already stretches the surface over
 * the full cell rectangle, restoring native visual size. DSCO_PIXEL_TUI_DPR
 * overrides the device scale (1-4). DSCO_PIXEL_TUI_ZOOM is a separate
 * positive display-size multiplier, including values below 1. Auto
 * uses 1.25 on reported 1x displays and 1 on HiDPI displays. */
static int requested_device_scale(void) {
    const char *env = getenv("DSCO_PIXEL_TUI_DPR");
    if (env && *env) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end != env && *end == '\0' && v >= 1 && v <= 4)
            return (int)v;
    }
    return 0;
}

/* A session override avoids mutating the environment while render threads run. */
static double s_display_zoom_override = -1.0;

static double requested_display_zoom(void) {
    if (s_display_zoom_override >= 0.0) return s_display_zoom_override;
    double zoom = 0.0;
    (void)native_display_zoom_parse(getenv("DSCO_PIXEL_TUI_ZOOM"), &zoom);
    return zoom;
}

static double render_logical_scale(int cols, int rows, int pixel_width, int pixel_height) {
    int requested = requested_device_scale();
    int device_scale = native_ui_terminal_viewport(cols, rows, pixel_width, pixel_height,
                                                   requested).backing_scale;
    double zoom = requested_display_zoom();
    if (zoom == 0.0) {
        /* Gentle external-display enlargement; never infer a full 2x zoom
         * from a 1x monitor. Explicit DPR keeps its diagnostic semantics. */
        zoom = requested == 0 && pixel_width > 0 && pixel_height > 0 && device_scale == 1
                   ? 1.25 : 1.0;
    }
    return device_scale * zoom;
}

static double render_device_scale(int cols, int rows, int pixel_width, int pixel_height) {
    return render_logical_scale(cols, rows, pixel_width, pixel_height);
}

static void session_render_geometry(int cols, int rows, int pixel_width, int pixel_height,
                                    int *width, int *height, double *backing_scale, int *surface_width,
                                    int *surface_height) {
    native_ui_viewport_metrics_t viewport = native_ui_terminal_viewport(
        cols, rows, pixel_width, pixel_height, requested_device_scale());
    double scale = render_device_scale(cols, rows, pixel_width, pixel_height);
    double source_width = pixel_width > 0 ? pixel_width : (double)viewport.logical_width * viewport.backing_scale;
    double source_height = pixel_height > 0 ? pixel_height : (double)viewport.logical_height * viewport.backing_scale;
    double logical_width = source_width / scale;
    double logical_height = source_height / scale;
    double fit = 1.0;
    /* Automatic layout keeps its legacy bounds. Explicit zoom owns the
     * logical viewport: 5% at 1080p means 38400x21600 logical pixels,
     * rasterized directly into 1920x1080 physical pixels. */
    if (requested_display_zoom() == 0.0) {
        fit = fmax(1.0, fmax(320.0 / logical_width, 180.0 / logical_height));
        fit = fmin(fit, fmin(1920.0 / logical_width, 1200.0 / logical_height));
    }
    /* Leave headroom for signed layout arithmetic at numerical extremes. */
    double limit = INT32_MAX / 64;
    *width = (int)fmax(1.0, fmin(limit, floor(logical_width * fit)));
    *height = (int)fmax(1.0, fmin(limit, floor(logical_height * fit)));
    if (backing_scale) *backing_scale = scale;
    if (surface_width) *surface_width = (int)fmax(1.0, source_width * fit + 0.5);
    if (surface_height) *surface_height = (int)fmax(1.0, source_height * fit + 0.5);
}

static uint32_t session_image_id(uint32_t generation, pixel_tui_state_t state) {
    uint32_t id = 0x44530000U ^ ((uint32_t)getpid() << 5) ^ (generation * 0x9e3779b9U) ^
                  ((uint32_t)state + 1U) * 0x101U;
    while (!id || id == g_session.scene_image_id || id == g_session.overlay_image_id)
        id++;
    return id;
}

static bool session_upload_state(FILE *out, const char *model, int width, int height,
                                 double backing_scale, int surface_width, int surface_height,
                                 uint32_t generation, pixel_tui_state_t state,
                                 uint32_t *out_image_id, uint8_t **out_frame,
                                 size_t *out_frame_size) {
    bool perf = pixel_tui_perf_enabled();
    double frame_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    pixel_tui_frame_sample_t sample = {.kind = PIXEL_TUI_FRAME_FAILED};
    char control[256];
    uint32_t image_id = session_image_id(generation, state);
    if (out_image_id)
        *out_image_id = image_id;
    if (out_frame)
        *out_frame = NULL;
    if (out_frame_size)
        *out_frame_size = 0;
    double render_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    px_canvas_t *canvas = render_session_frame(width, height, backing_scale, surface_width,
                                               surface_height, model, state);
    if (perf) {
        sample.has_render = true;
        sample.render_ms = monotonic_s() * 1000.0 - render_started_ms;
    }
    if (!canvas) {
        perf_finish_frame(&sample, frame_started_ms);
        return false;
    }
    /* The canvas is placed over the complete terminal grid below. Its raster
     * dimensions must therefore already match the terminal's logical surface:
     * never let Kitty perform a second resize of an undersized upload. */
    snprintf(control, sizeof(control), "a=t,t=d,f=24,s=%d,v=%d,i=%u,q=2,o=z", canvas->pixel_width,
             canvas->pixel_height, image_id);
    kitty_graphics_send_stats_t stats = {0};
    double upload_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    bool sent = send_kitty_pixels(out, control, canvas, false, perf ? &stats : NULL);
    if (perf)
        perf_add_transport(&sample, &stats, 0);
    double flush_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    if (fflush(out) != 0)
        sent = false;
    if (perf) {
        sample.flush_ms = monotonic_s() * 1000.0 - flush_started_ms;
        sample.upload_ms = monotonic_s() * 1000.0 - upload_started_ms;
    }
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SESSION_SNAPSHOT");
    if (snapshot && *snapshot &&
        ((!g_session.active && state == PIXEL_TUI_IDLE) ||
         (g_session.active && state == g_session.state)))
        (void)canvas_write_ppm(snapshot, canvas);
    if (sent && out_frame && out_frame_size) {
        size_t frame_size =
            (size_t)canvas->pixel_width * (size_t)canvas->pixel_height * sizeof(px_color_t);
        uint8_t *baseline = malloc(frame_size);
        if (baseline) {
            memcpy(baseline, canvas->pixels, frame_size);
            *out_frame = baseline;
            *out_frame_size = frame_size;
            size_t retained = compositor_retained_bytes();
            sample.retained_bytes =
                frame_size <= SIZE_MAX - retained ? retained + frame_size : SIZE_MAX;
        }
    }
    sample.kind = sent ? PIXEL_TUI_FRAME_FULL : PIXEL_TUI_FRAME_FAILED;
    perf_finish_frame(&sample, frame_started_ms);
    free_canvas(canvas);
    return sent;
}

static void session_delete_image(FILE *out, uint32_t image_id, bool free_data) {
    if (!out || !image_id)
        return;
    fprintf(out, "\033_Ga=d,d=%c,i=%u,q=2\033\\", free_data ? 'I' : 'i', image_id);
}

/* The retained scene is a separate Kitty image above the transcript, confined
 * to that region so it cannot cover the shared editor. Temporary UI takes
 * precedence by hiding the placement, never by discarding the scene model. */
static void session_place_scene(FILE *out, bool reanchor) {
    bool visible = g_session.active && !g_session.terminal_suspended &&
                   g_session.scene_json && g_session.scene_image_id && !g_session.scene_dirty &&
                   !native_windows_visible() &&
                   !g_session.overlay_image_id && !g_session.modal.active &&
                   !g_session.command_help_active &&
                   g_session.composer_menu.kind == PIXEL_TUI_MENU_NONE;
    if (!visible) {
        if (g_session.scene_placed)
            session_delete_image(out, g_session.scene_image_id, false);
        g_session.scene_placed = false;
        return;
    }
    if (!reanchor && g_session.scene_placed)
        return;
    if (g_session.scene_placed)
        session_delete_image(out, g_session.scene_image_id, false);
    fprintf(out, "\0337\033[%d;%dH\033_Ga=p,i=%u,p=1,c=%d,r=%d,C=1,z=2,q=2\033\\\0338",
            g_session.scene_row, g_session.scene_col, g_session.scene_image_id,
            g_session.scene_cols, g_session.scene_rows);
    g_session.scene_placed = true;
}

static void session_release_scene(FILE *out) {
    session_delete_image(out, g_session.scene_image_id, true);
    free(g_session.scene_json);
    g_session.scene_json = NULL;
    g_session.scene_image_id = 0;
    g_session.scene_placed = false;
    g_session.scene_dirty = false;
    g_session.scene_width = g_session.scene_height = 0;
    g_session.scene_cols = g_session.scene_rows = 0;
}

static bool session_refresh_scene(FILE *out) {
    if (g_session.terminal_suspended)
        return true;
    if (!g_session.scene_json) {
        if (g_session.scene_image_id)
            session_release_scene(out);
        return true;
    }
    native_ui_agent_shell_layout_t shell =
        native_ui_agent_shell_layout(g_session.width, g_session.height);
    session_deck_geometry_t deck =
        session_deck_geometry(g_session.width, g_session.height, g_session.state);
    native_ui_rect_t area = shell.transcript;
    area.height -= deck.deck_extra;
    if (shell.shows_inspector && (g_session.state == PIXEL_TUI_REASONING ||
                                 g_session.state == PIXEL_TUI_RESPONDING))
        area.width += shell.gap + shell.inspector.width;
    int cols = g_session.cols, rows = g_session.rows;
    if (cols < 1 || rows < 1 || g_session.width < 1 || g_session.height < 1)
        return false;
    int left = (area.x * cols + g_session.width - 1) / g_session.width;
    int top = (area.y * rows + g_session.height - 1) / g_session.height;
    int right = (area.x + area.width) * cols / g_session.width;
    int bottom = (area.y + area.height) * rows / g_session.height;
    int placement_cols = right - left, placement_rows = bottom - top;
    int width = placement_cols * g_session.width / cols;
    int height = placement_rows * g_session.height / rows;
    if (width > 1280) width = 1280;
    if (height > 900) height = 900;
    if (width < 160 || height < 56 || placement_rows < 1 || placement_cols < 1) {
        g_session.scene_dirty = true;
        session_place_scene(out, false);
        return false;
    }
    if (!g_session.scene_dirty && g_session.scene_width == width &&
        g_session.scene_height == height && g_session.scene_col == left + 1 &&
        g_session.scene_row == top + 1 && g_session.scene_cols == placement_cols &&
        g_session.scene_rows == placement_rows) {
        session_place_scene(out, false);
        return true;
    }
    px_canvas_t *canvas = render_scene_frame(g_session.scene_json, width, height);
    if (!canvas)
        return false;
    uint32_t image_id = 0x4453474eU ^ ((uint32_t)getpid() << 5) ^
                        (++g_session.scene_generation * 0x85ebca6bU);
    for (;;) {
        bool taken = !image_id || image_id == g_session.scene_image_id ||
                     image_id == g_session.overlay_image_id;
        for (int i = 0; i < 4; i++)
            taken = taken || image_id == g_session.image_ids[i];
        if (!taken) break;
        image_id++;
    }
    char control[192];
    snprintf(control, sizeof(control), "a=t,t=d,f=24,s=%d,v=%d,i=%u,q=2,o=z",
             canvas->pixel_width, canvas->pixel_height, image_id);
    bool sent = send_kitty_pixels(out, control, canvas, false, NULL);
    free_canvas(canvas);
    if (sent && fflush(out) != 0)
        sent = false;
    if (!sent) {
        session_delete_image(out, image_id, true);
        return false;
    }
    uint32_t previous_id = g_session.scene_image_id;
    g_session.scene_image_id = image_id;
    g_session.scene_col = left + 1;
    g_session.scene_row = top + 1;
    g_session.scene_cols = placement_cols;
    g_session.scene_rows = placement_rows;
    g_session.scene_width = width;
    g_session.scene_height = height;
    g_session.scene_dirty = false;
    g_session.scene_placed = false;
    session_place_scene(out, false);
    session_delete_image(out, previous_id, true);
    return true;
}

static void session_clear_overlay(FILE *out) {
    if (!g_session.overlay_image_id)
        return;
    session_delete_image(out, g_session.overlay_image_id, true);
    g_session.overlay_image_id = 0;
    session_place_scene(out, false);
}

static void session_place_current(FILE *out) {
    if (!g_session.active || !out)
        return;
    uint32_t image_id = g_session.image_ids[g_session.state];
    /* Re-anchor at screen origin after transcript scrolling. The placement is
     * image-only state; save/restore keeps the composer's cursor untouched. */
    fprintf(out, "\0337\033[H");
    /* Re-anchor the existing image placement. Do not delete the image data
     * before placing it: Kitty's d=i removes the image, leaving a black canvas
     * when another native session has caused a repaint/re-anchor. */
    /* Positive z-index makes this framebuffer authoritative. Cell-oriented
     * fallback output can continue behind it without becoming a second UI. */
    fprintf(out, "\033_Ga=p,i=%u,p=1,c=%d,r=%d,C=1,z=1,q=2\033\\", image_id, g_session.cols,
            g_session.rows);
    fprintf(out, "\0338");
    session_place_scene(out, true);
}

static double monotonic_s(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* ── Damage-based frame updates ─────────────────────────────────────────
 * A full session frame is a multi-megabyte encode. Between semantic events
 * most repaints change a caret, a meter tip, a ring segment — so the
 * compositor keeps a copy of the last uploaded frame, tile-diffs the new
 * one against it, and edits only the dirty rectangles of the resident image
 * (Kitty graphics `a=f,r=1`: compose new pixels into the root frame at an
 * offset). Kitty repaints the placement in place: no new image id, no
 * placement swap, no full-screen retransmit. Anything structural — resize,
 * state change (new image id), wide damage — falls back to the proven full
 * upload path. DSCO_PIXEL_TUI_PATCH=0 disables patching outright. */

#define SESSION_DAMAGE_TILE 32
#define SESSION_DAMAGE_MAX_RECTS 8
/* Above this fraction of the frame, one full upload beats many patches. */
#define SESSION_DAMAGE_MAX_COVERAGE 0.70
/* Periodic full refresh bounds drift from any lost/undelivered patch. */
#define SESSION_PATCH_STREAK_LIMIT 64

typedef struct {
    int x, y, w, h;
} session_rect_t;

static bool session_rects_touch(session_rect_t a, session_rect_t b) {
    return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}

static session_rect_t session_rect_union(session_rect_t a, session_rect_t b) {
    int left = a.x < b.x ? a.x : b.x;
    int top = a.y < b.y ? a.y : b.y;
    int right = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int bottom = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (session_rect_t){left, top, right - left, bottom - top};
}

static int session_damage_add(session_rect_t *rects, int count, session_rect_t rect) {
    for (int i = 0; i < count;) {
        if (session_rects_touch(rects[i], rect)) {
            rect = session_rect_union(rects[i], rect);
            rects[i] = rects[--count];
            i = 0; /* the grown union may now touch earlier rects */
        } else {
            i++;
        }
    }
    if (count >= SESSION_DAMAGE_MAX_RECTS)
        return -1;
    rects[count++] = rect;
    return count;
}

/* Diff the rendered canvas against the resident frame in coarse tiles and
 * coalesce dirty tiles into a handful of rectangles. Returns the rect count,
 * 0 when nothing changed, or -1 when patching is not worthwhile. */
static int session_collect_damage(const px_canvas_t *canvas, session_rect_t *rects) {
    if (!g_session.prev_frame || g_session.prev_frame_width != canvas->pixel_width ||
        g_session.prev_frame_height != canvas->pixel_height)
        return -1;
    const uint8_t *now_px = (const uint8_t *)canvas->pixels;
    const uint8_t *was_px = g_session.prev_frame;
    size_t stride = (size_t)canvas->pixel_width * 3;
    int count = 0;
    size_t dirty_area = 0;
    for (int ty = 0; ty < canvas->pixel_height; ty += SESSION_DAMAGE_TILE) {
        int th = canvas->pixel_height - ty < SESSION_DAMAGE_TILE ? canvas->pixel_height - ty
                                                                 : SESSION_DAMAGE_TILE;
        for (int tx = 0; tx < canvas->pixel_width; tx += SESSION_DAMAGE_TILE) {
            int tw = canvas->pixel_width - tx < SESSION_DAMAGE_TILE ? canvas->pixel_width - tx
                                                                    : SESSION_DAMAGE_TILE;
            bool dirty = false;
            for (int row = 0; row < th; row++) {
                size_t at = (size_t)(ty + row) * stride + (size_t)tx * 3;
                if (memcmp(now_px + at, was_px + at, (size_t)tw * 3) != 0) {
                    dirty = true;
                    break;
                }
            }
            if (!dirty)
                continue;
            dirty_area += (size_t)tw * (size_t)th;
            count = session_damage_add(rects, count, (session_rect_t){tx, ty, tw, th});
            if (count < 0)
                return -1;
        }
    }
    double coverage =
        (double)dirty_area / ((double)canvas->pixel_width * (double)canvas->pixel_height);
    if (coverage > SESSION_DAMAGE_MAX_COVERAGE)
        return -1;
    return count;
}

static bool session_send_patch(FILE *out, const px_canvas_t *canvas, uint32_t image_id,
                               session_rect_t rect, int origin_x, int origin_y,
                               kitty_graphics_send_stats_t *stats) {
    size_t bytes = (size_t)rect.w * (size_t)rect.h * 3;
    if (g_session.patch_buffer_cap < bytes) {
        uint8_t *grown = realloc(g_session.patch_buffer, bytes);
        if (!grown)
            return false;
        g_session.patch_buffer = grown;
        g_session.patch_buffer_cap = bytes;
    }
    uint8_t *region = g_session.patch_buffer;
    size_t stride = (size_t)canvas->pixel_width * 3;
    const uint8_t *src = (const uint8_t *)canvas->pixels;
    for (int row = 0; row < rect.h; row++)
        memcpy(region + (size_t)row * rect.w * 3,
               src + (size_t)(rect.y + row) * stride + (size_t)rect.x * 3, (size_t)rect.w * 3);
    bool sent = kitty_graphics_send_rgb_patch(out, image_id, 1,
                                               origin_x + rect.x, origin_y + rect.y, rect.w, rect.h,
                                               region, bytes, stats);
    return sent;
}

/* Retain the uploaded canvas as the diff baseline for `image_id`. */
static void session_store_frame(const px_canvas_t *canvas, uint32_t image_id) {
    size_t need = (size_t)canvas->pixel_width * (size_t)canvas->pixel_height * 3;
    if (g_session.prev_frame_cap < need) {
        uint8_t *grown = realloc(g_session.prev_frame, need);
        if (!grown) {
            free(g_session.prev_frame);
            g_session.prev_frame = NULL;
            g_session.prev_frame_cap = 0;
            g_session.prev_frame_image = 0;
            return;
        }
        g_session.prev_frame = grown;
        g_session.prev_frame_cap = need;
    }
    memcpy(g_session.prev_frame, canvas->pixels, need);
    g_session.prev_frame_width = canvas->pixel_width;
    g_session.prev_frame_height = canvas->pixel_height;
    g_session.prev_frame_image = image_id;
}

static void session_store_region(const px_canvas_t *canvas, int x, int y) {
    if (!canvas || !canvas->pixels || !g_session.prev_frame || x < 0 || y < 0 ||
        x + canvas->pixel_width > g_session.prev_frame_width ||
        y + canvas->pixel_height > g_session.prev_frame_height)
        return;
    size_t destination_stride = (size_t)g_session.prev_frame_width * 3U;
    size_t source_stride = (size_t)canvas->pixel_width * 3U;
    for (int row = 0; row < canvas->pixel_height; row++) {
        memcpy(g_session.prev_frame + (size_t)(y + row) * destination_stride + (size_t)x * 3U,
               (const uint8_t *)canvas->pixels + (size_t)row * source_stride, source_stride);
    }
}

static double session_repaint_interval_s(void) {
    double interval = (double)g_session.animation_interval_ms / 1000.0;
    if (interval < 0.016)
        interval = 0.016;
    bool input_active =
        atomic_load_explicit(&g_composer_input_active, memory_order_acquire);
    /* Keep raster+transport below roughly 70% duty cycle, or 35% while the
     * composer owns input, on expensive Retina viewports. Healthy frames keep
     * the near-60 Hz target; slow frames back off in proportion to measured
     * cost instead of forcing every active editor down to a visibly stale
     * fixed cadence. Keystroke echo remains on its independent patch clock. */
    if (g_session.background_frame_cost_ema_ms > 0.0) {
        double duty_per_mille = input_active ? 350.0 : 700.0;
        double cost_limited = g_session.background_frame_cost_ema_ms / duty_per_mille;
        if (interval < cost_limited)
            interval = cost_limited;
    }
    return interval;
}

/* Editing has its own latency budget.  Background animation and transcript
 * raster are deliberately slower because they touch the full Retina surface;
 * tying both classes to one deadline lets a keypress wake accidentally launch
 * a full-screen frame before the small composer patch is visible. */
static double session_composer_repaint_interval_s(void) {
    /* Eight milliseconds keeps key echo inside a 120 Hz interaction budget
     * while coalescing key-repeat and paste bursts before terminal transport. */
    return 0.008;
}

/* Composer menu item count contained in the resident Kitty frame (-1 until
 * a full frame establishes it). */
static int s_patched_menu_count = -1;

static void session_note_background_frame(double frame_started_s) {
    double finished_s = monotonic_s();
    double cost_ms = (finished_s - frame_started_s) * 1000.0;
    if (cost_ms < 0.0)
        cost_ms = 0.0;
    if (g_session.background_frame_cost_ema_ms <= 0.0)
        g_session.background_frame_cost_ema_ms = cost_ms;
    else
        g_session.background_frame_cost_ema_ms =
            g_session.background_frame_cost_ema_ms * 0.80 + cost_ms * 0.20;
    /* Start-to-start pacing skips missed slots instead of adding render time
     * to the requested interval. A full frame also refreshes the composer. */
    g_session.last_paint_s = frame_started_s;
    g_session.last_composer_paint_s = frame_started_s;
    g_session.last_activity_paint_s = frame_started_s;
    s_patched_menu_count = g_session.composer_menu.kind != PIXEL_TUI_MENU_NONE
                               ? g_session.composer_menu.count
                               : 0;
}

/* Fast path for ordinary editing: raster only the retained composer and edit
 * that rectangle in the resident Kitty image.  No transcript shaping, full
 * Retina canvas fill, frame-wide tile scan, or full-image compression occurs.
 * Structural changes (multiline deck growth, menus, overlays, modal layers)
 * are rejected by the caller and use session_repaint() instead. */
static bool session_repaint_composer(FILE *out) {
    if (!g_session.active || !out || g_session.terminal_suspended || !g_session.patch_enabled ||
        g_session.patch_streak >= SESSION_PATCH_STREAK_LIMIT || g_session.overlay_image_id != 0 ||
        g_session.modal.active)
        return false;
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SESSION_SNAPSHOT");
    if (snapshot && *snapshot)
        return false;
    uint32_t image_id = g_session.image_ids[g_session.state];
    if (image_id == 0 || g_session.prev_frame_image != image_id || !g_session.prev_frame ||
        g_session.prev_frame_width != g_session.surface_width ||
        g_session.prev_frame_height != g_session.surface_height)
        return false;

    session_deck_geometry_t deck =
        session_deck_geometry(g_session.width, g_session.height, g_session.state);
    native_ui_rect_t frame = deck.composer;
    double scale = g_session.backing_scale > 0 ? g_session.backing_scale : 1;
    /* Slash command/image menus render above the deck box (over transcript
     * rows) and their selection/labels change while typing. Extend the patch
     * rect up over the menu panel so menu-open typing still rides this cheap
     * path instead of forcing a full Retina frame per keystroke. The resident
     * frame must already contain a menu of the same item count (established
     * by a full frame) or the patch could leave stale rows from a differently
     * sized panel. */
    const pixel_composer_menu_t *menu = &g_session.composer_menu;
    bool menu_active = menu->kind != PIXEL_TUI_MENU_NONE && menu->count > 0 && frame.width >= 120;
    if (menu_active && scale != floor(scale))
        return false; /* Fractional menu underlay needs the full transcript. */
    if (menu_active) {
        if (menu->count != s_patched_menu_count)
            return false;
        int panel_h = 27 + menu->count * 20;
        int menu_top = deck.deck_y - panel_h - 6;
        if (menu_top < 4)
            menu_top = 4;
        if (menu_top >= deck.deck_y)
            menu_active = false;
        else
            frame = (native_ui_rect_t){frame.x, menu_top, frame.width,
                                       frame.y + frame.height - menu_top};
    } else if (s_patched_menu_count > 0) {
        /* Resident frame contains a menu that just closed: patching the deck
         * box cannot erase the panel pixels above it. One full frame clears. */
        return false;
    }
    /* Fractional text origins must retain their global device-pixel phase.
     * Use the pooled full-coordinate surface but clear and paint only the
     * composer rectangle. This preserves arbitrary zoom (including 0.65)
     * without translating the UI or rasterizing the rest of the viewport. */
    bool global_raster = scale != floor(scale);
    int patch_x = (int)llround(frame.x * scale);
    int patch_y = (int)llround(frame.y * scale);
    int patch_width = (int)llround((frame.x + frame.width) * scale) - patch_x;
    int patch_height = (int)llround((frame.y + frame.height) * scale) - patch_y;
    if (frame.width < 160 || frame.height < 56 || patch_x < 0 || patch_y < 0 ||
        patch_x + frame.width * scale > g_session.surface_width ||
        patch_y + frame.height * scale > g_session.surface_height)
        return false;

    bool perf = pixel_tui_perf_enabled();
    double frame_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    pixel_tui_frame_sample_t sample = {.kind = PIXEL_TUI_FRAME_FAILED};
    if (perf && g_session.composer_repaint_pending_since_s > 0.0) {
        sample.has_queue = true;
        sample.queue_ms = monotonic_s() * 1000.0 -
                          g_session.composer_repaint_pending_since_s * 1000.0;
    }
    double render_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    px_canvas_t *canvas = global_raster
        ? canvas_acquire_device(g_session.width, g_session.height, g_session.surface_width,
                                 g_session.surface_height, scale)
        : canvas_acquire_scaled(frame.width, frame.height, scale);
    if (!canvas) {
        perf_finish_frame(&sample, frame_started_ms);
        return false;
    }
    int raster_x = global_raster ? patch_x : 0;
    int raster_y = global_raster ? patch_y : 0;
    if (global_raster) {
        int denominator = g_session.surface_height > 1 ? g_session.surface_height - 1 : 1;
        for (int y = 0; y < patch_height; ++y) {
            px_color_t color = color_mix(C_BG_TOP, C_BG_BOTTOM,
                                         (double)(patch_y + y) / denominator);
            px_color_t *row = canvas->pixels + (size_t)(raster_y + y) * canvas->pixel_width + raster_x;
            for (int x = 0; x < patch_width; ++x) row[x] = color;
        }
    } else {
        canvas_background_slice(canvas, patch_y, g_session.surface_height);
    }
    px_color_t accent = session_animated_accent(g_session.state);
    double active_pulse =
        g_session.state == PIXEL_TUI_IDLE ? 0.0 : motion_pulse(0.90, 0.0);
    double queue_breath = g_session.queue_depth > 0 ? motion_pulse(1.35, 0.2) : 0.0;
    double composer_energy = 0.58 + active_pulse * 0.14 + queue_breath * 0.22;
    if (menu_active)
        draw_session_composer_menu(canvas, 0, deck.deck_y - frame.y, frame.width, 0);
    bool rendered = draw_session_composer(
        canvas, (native_ui_rect_t){deck.composer.x - (global_raster ? 0 : frame.x),
                                  deck.composer.y - (global_raster ? 0 : frame.y),
                                  deck.composer.width, deck.composer.height},
        g_session.state, accent, composer_energy,
        global_raster ? 0 : frame.x, global_raster ? 0 : frame.y);
    if (perf) {
        sample.has_render = true;
        sample.render_ms = monotonic_s() * 1000.0 - render_started_ms;
    }
    if (!rendered) {
        perf_finish_frame(&sample, frame_started_ms);
        free_canvas(canvas);
        return false;
    }

    /* A keystroke changes only a handful of glyphs. Find their exact device
     * bounds inside this small retained panel before encoding; uploading the
     * whole composer spent more time compressing unchanged pixels than text. */
    double diff_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    session_rect_t dirty = {patch_width, patch_height, 0, 0};
    int right = 0, bottom = 0;
    for (int y = 0; y < patch_height; ++y) {
        const px_color_t *now = canvas->pixels + (size_t)(raster_y + y) * canvas->pixel_width + raster_x;
        const px_color_t *was = (const px_color_t *)g_session.prev_frame +
            (size_t)(patch_y + y) * g_session.prev_frame_width + patch_x;
        if (!memcmp(now, was, (size_t)patch_width * sizeof(*now)))
            continue;
        int left = 0, end = patch_width;
        while (left < end && !memcmp(now + left, was + left, sizeof(*now))) ++left;
        while (end > left && !memcmp(now + end - 1, was + end - 1, sizeof(*now))) --end;
        if (left < dirty.x) dirty.x = left;
        if (y < dirty.y) dirty.y = y;
        if (end > right) right = end;
        bottom = y + 1;
    }
    dirty.w = right - dirty.x;
    dirty.h = bottom - dirty.y;
    if (perf) {
        sample.has_diff = true;
        sample.diff_ms = monotonic_s() * 1000.0 - diff_started_ms;
    }
    bool changed = dirty.w > 0 && dirty.h > 0;
    session_rect_t source_dirty = {raster_x + dirty.x, raster_y + dirty.y, dirty.w, dirty.h};
    kitty_graphics_send_stats_t stats = {0};
    double upload_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    bool sent = !changed || session_send_patch(out, canvas, image_id, source_dirty,
        patch_x - raster_x, patch_y - raster_y, perf ? &stats : NULL);
    if (perf) {
        perf_add_transport(&sample, &stats, 0);
        sample.damage_rects = changed ? 1 : 0;
    }
    if (sent && changed) {
        kitty_graphics_send_stats_t select_stats = {0};
        sent = kitty_graphics_select_frame(out, image_id, 1, perf ? &select_stats : NULL);
        if (perf)
            perf_add_transport(&sample, &select_stats, 0);
    }
    double flush_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    if (sent && fflush(out) != 0)
        sent = false;
    if (perf) {
        sample.flush_ms = monotonic_s() * 1000.0 - flush_started_ms;
        sample.upload_ms += monotonic_s() * 1000.0 - upload_started_ms;
    }
    if (sent) {
        if (changed) {
            for (int row = 0; row < dirty.h; ++row)
                memcpy(g_session.prev_frame + ((size_t)(patch_y + dirty.y + row) *
                           g_session.prev_frame_width + patch_x + dirty.x) * 3U,
                       canvas->pixels + (size_t)(source_dirty.y + row) * canvas->pixel_width + source_dirty.x,
                       (size_t)dirty.w * 3U);
        }
        if (changed) g_session.patch_streak++;
        g_session.last_composer_paint_s = monotonic_s();
        g_session.composer_repaint_pending = false;
        g_session.composer_repaint_pending_since_s = 0.0;
        g_session.composer_fast_eligible = false;
        sample.kind = changed ? PIXEL_TUI_FRAME_PATCH : PIXEL_TUI_FRAME_IDENTICAL;
    }
    perf_finish_frame(&sample, frame_started_ms);
    free_canvas(canvas);
    return sent;
}

/* Live tools keep moving while the follow-up editor accepts input. This
 * independent clock patches only the retained deck; semantic updates still
 * belong to the full-frame deadline and cannot be cleared by this path. */
static bool session_activity_visible(void) {
    return g_session.activity_valid && g_session.animation_enabled &&
           !g_session.terminal_suspended && !g_session.overlay_image_id &&
           !g_session.scene_image_id && !g_session.modal.active &&
           !g_session.command_help_active &&
           g_session.composer_menu.kind == PIXEL_TUI_MENU_NONE &&
           !native_windows_visible() && !session_has_live_notice();
}

static bool session_repaint_activity(FILE *out) {
    if (!session_activity_visible() || !g_session.patch_enabled ||
        g_session.backing_scale != floor(g_session.backing_scale) ||
        g_session.patch_streak >= SESSION_PATCH_STREAK_LIMIT ||
        g_session.stream_repaint_pending || g_session.composer_repaint_pending)
        return false;
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SESSION_SNAPSHOT");
    if (snapshot && *snapshot) return false;
    uint32_t id = g_session.image_ids[g_session.state];
    if (!id || g_session.prev_frame_image != id ||
        g_session.prev_frame_width != g_session.surface_width ||
        g_session.prev_frame_height != g_session.surface_height)
        return false;
    const pixel_tool_visual_t *cards[PIXEL_TOOL_VIS_CAP];
    double presences[PIXEL_TOOL_VIS_CAP];
    int total = session_live_op_cards(cards, presences, PIXEL_TOOL_VIS_CAP);
    int shown = 0; bool compact = false;
    int height = live_op_deck_plan(presences, total, g_session.activity_avail_h, &shown, &compact);
    if (total != g_session.activity_total || shown != g_session.activity_shown ||
        compact != g_session.activity_compact || height != g_session.activity_rect.height)
        return false; /* Completion/geometry requires one semantic frame. */
    native_ui_rect_t r = g_session.activity_rect;
    int scale = g_session.backing_scale;
    bool perf = pixel_tui_perf_enabled();
    double started = monotonic_s();
    pixel_tui_frame_sample_t sample = {.kind = PIXEL_TUI_FRAME_FAILED};
    px_canvas_t *canvas = canvas_acquire_scaled(r.width, r.height, scale);
    if (!canvas) return false;
    size_t bytes = (size_t)canvas->pixel_width * canvas->pixel_height * 3U;
    memcpy(canvas->pixels, g_session.activity_underlay, bytes);
    draw_live_op_deck(canvas, 0, 0, r.width, cards, presences, total, shown, compact);
    if (perf) {
        sample.has_render = true;
        sample.render_ms = (monotonic_s() - started) * 1000.0;
    }
    kitty_graphics_send_stats_t stats = {0};
    bool sent = kitty_graphics_send_rgb_patch(out, id, 1, r.x * scale, r.y * scale,
        canvas->pixel_width, canvas->pixel_height, (uint8_t *)canvas->pixels, bytes,
        perf ? &stats : NULL);
    if (perf) perf_add_transport(&sample, &stats, 0);
    if (sent) {
        kitty_graphics_send_stats_t select = {0};
        sent = kitty_graphics_select_frame(out, id, 1, perf ? &select : NULL);
        if (perf) perf_add_transport(&sample, &select, 0);
    }
    double flush_started = monotonic_s();
    if (sent) sent = fflush(out) == 0;
    if (perf) sample.flush_ms = (monotonic_s() - flush_started) * 1000.0;
    if (sent) {
        session_store_region(canvas, r.x * scale, r.y * scale);
        g_session.patch_streak++;
        g_session.last_activity_paint_s = started;
        sample.kind = PIXEL_TUI_FRAME_PATCH;
        sample.damage_rects = 1;
    } else {
        /* Partial transport cannot remain the baseline for future diffs. */
        g_session.patch_streak = SESSION_PATCH_STREAK_LIMIT;
    }
    free_canvas(canvas);
    perf_finish_frame(&sample, started * 1000.0);
    return sent;
}

static bool session_repaint(FILE *out, bool force) {
    if (!g_session.active || !out)
        return false;
    if (g_session.terminal_suspended)
        return true;
    (void)session_refresh_scene(out);
    bool perf = pixel_tui_perf_enabled();
    double frame_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    pixel_tui_frame_sample_t sample = {.kind = PIXEL_TUI_FRAME_FAILED};
    double now = monotonic_s();
    if (!force && g_session.last_paint_s > 0.0 &&
        now - g_session.last_paint_s < session_repaint_interval_s()) {
        pixel_tui_perf_note_throttled();
        return true;
    }
    /* This frame samples all mutations made while holding the session lock. */
    double oldest_pending_s = 0.0;
    if (g_session.stream_repaint_pending_since_s > 0.0)
        oldest_pending_s = g_session.stream_repaint_pending_since_s;
    if (g_session.composer_repaint_pending_since_s > 0.0 &&
        (oldest_pending_s <= 0.0 || g_session.composer_repaint_pending_since_s < oldest_pending_s))
        oldest_pending_s = g_session.composer_repaint_pending_since_s;
    if (perf && oldest_pending_s > 0.0) {
        sample.has_queue = true;
        sample.queue_ms = now * 1000.0 - oldest_pending_s * 1000.0;
    }
    g_session.stream_repaint_pending = false;
    g_session.structural_repaint_pending = false;
    g_session.composer_repaint_pending = false;
    g_session.stream_repaint_pending_since_s = 0.0;
    g_session.composer_repaint_pending_since_s = 0.0;
    g_session.composer_fast_eligible = false;

    double render_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    px_canvas_t *canvas = render_session_frame(
        g_session.width, g_session.height, g_session.backing_scale, g_session.surface_width,
        g_session.surface_height, g_session.model, g_session.state);
    if (perf) {
        sample.has_render = true;
        sample.render_ms = monotonic_s() * 1000.0 - render_started_ms;
    }
    if (!canvas) {
        perf_finish_frame(&sample, frame_started_ms);
        return false;
    }
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SESSION_SNAPSHOT");
    if (snapshot && *snapshot)
        (void)canvas_write_ppm(snapshot, canvas);

    double upload_started_ms = 0.0;
    uint32_t current_id = g_session.image_ids[g_session.state];
    if (g_session.patch_enabled && current_id != 0 && g_session.prev_frame_image == current_id &&
        g_session.patch_streak < SESSION_PATCH_STREAK_LIMIT) {
        session_rect_t rects[SESSION_DAMAGE_MAX_RECTS];
        double diff_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
        int count = session_collect_damage(canvas, rects);
        if (perf) {
            sample.has_diff = true;
            sample.diff_ms = monotonic_s() * 1000.0 - diff_started_ms;
        }
        if (count == 0) {
            /* Bit-identical frame: nothing to transmit. */
            session_note_background_frame(now);
            sample.kind = PIXEL_TUI_FRAME_IDENTICAL;
            perf_finish_frame(&sample, frame_started_ms);
            free_canvas(canvas);
            return true;
        }
        if (count > 0) {
            if (perf)
                upload_started_ms = monotonic_s() * 1000.0;
            bool sent = true;
            for (int i = 0; i < count && sent; i++) {
                kitty_graphics_send_stats_t stats = {0};
                sent = session_send_patch(out, canvas, current_id, rects[i], 0, 0, perf ? &stats : NULL);
                if (perf) {
                    size_t region_bytes = (size_t)rects[i].w * (size_t)rects[i].h * 3U;
                    perf_add_transport(&sample, &stats, region_bytes);
                }
            }
            if (sent) {
                kitty_graphics_send_stats_t select_stats = {0};
                sent = kitty_graphics_select_frame(out, current_id, 1,
                                                   perf ? &select_stats : NULL);
                if (perf)
                    perf_add_transport(&sample, &select_stats, 0);
            }
            sample.damage_rects = (uint32_t)count;
            if (sent) {
                session_store_frame(canvas, current_id);
                g_session.patch_streak++;
                double flush_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
                if (fflush(out) != 0)
                    sent = false;
                if (perf) {
                    sample.flush_ms = monotonic_s() * 1000.0 - flush_started_ms;
                    sample.upload_ms = monotonic_s() * 1000.0 - upload_started_ms;
                }
            }
            if (sent) {
                session_note_background_frame(now);
                sample.kind = PIXEL_TUI_FRAME_PATCH;
                perf_finish_frame(&sample, frame_started_ms);
                free_canvas(canvas);
                return true;
            }
            /* A failed patch leaves the resident image undefined; fall
             * through to a full authoritative upload. */
        }
    }

    uint32_t generation = g_session.generation + 1;
    uint32_t next_id = session_image_id(generation, g_session.state);
    char control[256];
    /* Keep Kitty's declared source size identical to the actual raster. */
    snprintf(control, sizeof(control), "a=t,t=d,f=24,s=%d,v=%d,i=%u,q=2,o=z", canvas->pixel_width,
             canvas->pixel_height, next_id);
    if (perf && upload_started_ms == 0.0)
        upload_started_ms = monotonic_s() * 1000.0;
    kitty_graphics_send_stats_t stats = {0};
    if (!send_kitty_pixels(out, control, canvas, false, perf ? &stats : NULL)) {
        if (perf) {
            perf_add_transport(&sample, &stats, 0);
            sample.upload_ms = monotonic_s() * 1000.0 - upload_started_ms;
        }
        session_delete_image(out, next_id, true);
        perf_finish_frame(&sample, frame_started_ms);
        free_canvas(canvas);
        return false;
    }
    if (perf)
        perf_add_transport(&sample, &stats, 0);
    uint32_t old_id = g_session.image_ids[g_session.state];
    g_session.generation = generation;
    g_session.image_ids[g_session.state] = next_id;
    g_session.image_widths[g_session.state] = canvas->pixel_width;
    g_session.image_heights[g_session.state] = canvas->pixel_height;
    session_store_frame(canvas, next_id);
    g_session.patch_streak = 0;
    session_place_current(out);
    session_delete_image(out, old_id, true);
    double flush_started_ms = perf ? monotonic_s() * 1000.0 : 0.0;
    bool flushed = fflush(out) == 0;
    if (flushed)
        session_note_background_frame(now);
    if (perf) {
        sample.flush_ms += monotonic_s() * 1000.0 - flush_started_ms;
        sample.upload_ms = monotonic_s() * 1000.0 - upload_started_ms;
    }
    sample.kind = flushed ? PIXEL_TUI_FRAME_FULL : PIXEL_TUI_FRAME_FAILED;
    perf_finish_frame(&sample, frame_started_ms);
    free_canvas(canvas);
    return flushed;
}

/* Keep the framebuffer coupled to the live Kitty grid even while the agent is
 * idle. SIGWINCH is delivered to an arbitrary process thread, and the legacy
 * composer only consumes its deferred resize flag while it owns input. A
 * native session therefore also samples the TTY geometry from its compositor
 * loop so an OS-window resize cannot strand a stale, smaller surface. */
static bool session_refresh_geometry_locked(FILE *out, bool reanchor_unchanged) {
    int cols = g_session.cols, rows = g_session.rows;
    int pixel_width = 0, pixel_height = 0;
    if (!terminal_geometry(out, &cols, &rows, &pixel_width, &pixel_height)) {
        if (reanchor_unchanged) {
            session_place_current(out);
            fflush(out);
        }
        return false;
    }

    int width = 0, height = 0;
    double backing_scale = 1;
    int surface_width = 0, surface_height = 0;
    session_render_geometry(cols, rows, pixel_width, pixel_height, &width, &height, &backing_scale,
                            &surface_width, &surface_height);
    if (cols == g_session.cols && rows == g_session.rows && width == g_session.width &&
        height == g_session.height && backing_scale == g_session.backing_scale &&
        surface_width == g_session.surface_width && surface_height == g_session.surface_height) {
        if (reanchor_unchanged) {
            session_place_current(out);
            fflush(out);
        }
        return false;
    }

    /* Kitty may clear/reflow the placement during a live resize. Stretch the
     * resident surface immediately, then atomically replace it at the new
     * native pixel dimensions. */
    g_session.cols = cols;
    g_session.rows = rows;
    g_session.scene_dirty = g_session.scene_json != NULL;
    session_place_scene(out, false);
    session_clear_overlay(out);
    session_place_current(out);
    fflush(out);
    g_session.width = width;
    g_session.height = height;
    g_session.backing_scale = backing_scale;
    g_session.surface_width = surface_width;
    g_session.surface_height = surface_height;
    (void)session_repaint(out, true);
    return true;
}

/* True while any message still has undisclosed streamed text. */
static bool session_reveal_active(void) {
    for (int i = 0; i < g_session.message_count; i++) {
        int at = (g_session.message_start + i) % PIXEL_MESSAGE_CAP;
        const pixel_message_t *m = &g_session.messages[at];
        if (m->reveal_pending && m->reveal_len < m->text_len)
            return true;
    }
    return false;
}

/* Advance every pending reveal by an adaptive glyph budget. The base rate
 * reads as typing; the backlog term guarantees the reveal converges within
 * ~250ms of the newest delta, so the transcript never lags the stream. */
static bool session_reveal_step(double now) {
    bool advanced = false;
    double last = g_session.reveal_last_s;
    double dt = last > 0.0 ? now - last : session_repaint_interval_s();
    if (dt < 0.0)
        dt = 0.0;
    if (dt > 0.25)
        dt = 0.25;
    bool any_pending = false;
    for (int i = 0; i < g_session.message_count; i++) {
        int at = (g_session.message_start + i) % PIXEL_MESSAGE_CAP;
        pixel_message_t *m = &g_session.messages[at];
        if (!m->reveal_pending)
            continue;
        if (m->reveal_len >= m->text_len) {
            m->reveal_len = m->text_len;
            if (!m->streaming)
                m->reveal_pending = false;
            else
                any_pending = true;
            continue;
        }
        any_pending = true;
        size_t backlog = m->text_len - m->reveal_len;
        double rate = 360.0 + (double)backlog * 4.0; /* glyphs per second */
        size_t budget = (size_t)(rate * dt) + 1;
        size_t pos = m->reveal_len;
        while (budget > 0 && pos < m->text_len) {
            size_t step = utf8_char_bytes(m->text + pos);
            if (step < 1)
                step = 1;
            pos += step;
            budget--;
        }
        if (pos > m->text_len)
            pos = m->text_len;
        if (pos != m->reveal_len) {
            advanced = true;
            if (m->sequence != g_session.next_sequence)
                s_transcript_epoch++;
        }
        m->reveal_len = pos;
    }
    g_session.reveal_last_s = any_pending ? now : 0.0;
    return advanced;
}

static bool session_animation_fast(void) {
    /* Accepting a follow-up is not a pause command. Active phases and reveal
     * must advance on their own clock even when no input events arrive. The
     * separate composer clock and measured frame-cost backoff protect typing. */
    if (g_session.animation_enabled && g_session.state != PIXEL_TUI_IDLE)
        return true;
    /* Animate the discrete phase transition and any live timeline track,
     * then park. Streaming, tools and swarms already trigger semantic
     * repaints when their data changes; a perpetual full-screen pulse only
     * burns a core and adds no information. */
    if (session_transition_progress() < 1.0)
        return true;
    if (ui_motion_active(&g_session.motion, monotonic_s()))
        return true;
    /* An in-flight gradual reveal must keep the compositor at frame cadence
     * until the transcript has caught up with the stream. */
    if (session_reveal_active())
        return true;
    /* Live-op cards are the liveness guarantee: while any tool runs, keep the
     * compositor at animation cadence so the spinner arc, bottom sweep, and
     * tenths-of-a-second elapsed readout actually move. Reduced motion skips
     * this — cards still update on telemetry and the 500ms park tick. */
    if (g_session.animation_enabled && session_running_tool_count() > 0)
        return true;
    return false;
}

static void animation_deadline(struct timespec *deadline, int delay_ms) {
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += delay_ms / 1000;
    deadline->tv_nsec += (long)(delay_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static int session_deadline_delay_ms(double last_s, double interval_s, double now_s) {
    if (last_s <= 0.0 || interval_s <= 0.0)
        return 1;
    double remaining_s = last_s + interval_s - now_s;
    if (remaining_s <= 0.0)
        return 1;
    double delay_ms = ceil(remaining_s * 1000.0);
    return delay_ms > 1000.0 ? 1000 : (int)delay_ms;
}

static int session_caret_deadline_ms(double now_s) {
    double phase = session_caret_phase(now_s);
    double remaining_s = phase < 0.62 ? 0.62 - phase : 1.0 - phase;
    int delay_ms = (int)ceil(remaining_s * 1000.0);
    return delay_ms < 1 ? 1 : delay_ms;
}

static void session_animation_wake(void) {
    if (g_session.animation_thread_started)
        (void)pthread_cond_signal(&g_animation_cond);
}

/* Composer producers intentionally never wait for a frame-wide session lock.
 * Pair their condition signal with a try-lock handshake so a publication
 * cannot land between the render thread's predicate check and timed wait. */
static void session_animation_wake_async(void) {
    if (!atomic_load_explicit(&g_animation_thread_fast, memory_order_acquire))
        return;
    bool locked = pthread_mutex_trylock(&g_session_mutex) == 0;
    (void)pthread_cond_signal(&g_animation_cond);
    if (locked)
        (void)pthread_mutex_unlock(&g_session_mutex);
}

/* Token callbacks mutate the retained transcript and return. The compositor
 * thread coalesces every delta that arrives before the next frame boundary,
 * preventing provider/network work from paying raster + compression cost. */
static void session_schedule_repaint(FILE *out, bool force_if_synchronous) {
    native_trace_event("repaint_requested", force_if_synchronous, g_session.state,
                       g_session.stream_repaint_pending, g_session.input_active);
    if (g_session.animation_thread_started) {
        pixel_tui_perf_note_stream_request(g_session.stream_repaint_pending);
        if (!g_session.stream_repaint_pending)
            g_session.stream_repaint_pending_since_s = monotonic_s();
        g_session.stream_repaint_pending = true;
        /* Phase, tool, modal and other structural publications must remain
         * visible while the shared editor is active. Plain text deltas use
         * false and keep their coalesced background cadence. */
        g_session.structural_repaint_pending |= force_if_synchronous;
        session_animation_wake();
    } else {
        pixel_tui_perf_note_stream_request(false);
        (void)session_repaint(out, force_if_synchronous);
    }
}

static void session_mark_composer_repaint(bool fast_eligible) {
    if (!g_session.composer_repaint_pending) {
        g_session.composer_fast_eligible = fast_eligible;
        g_session.composer_repaint_pending_since_s = monotonic_s();
    } else {
        g_session.composer_fast_eligible &= fast_eligible;
    }
    g_session.composer_repaint_pending = true;
}

static bool s_caret_blink_visible = true;

/* Mirror of the eligibility checks in session_repaint_composer(): true when
 * a cheap composer-region patch is possible without falling back to a full
 * Retina frame. Used by the caret-blink tick so blinking never forces
 * full-frame raster while the editor owns input. */
static bool session_composer_patch_possible(void) {
    if (!g_session.patch_enabled ||
        g_session.patch_streak >= SESSION_PATCH_STREAK_LIMIT ||
        g_session.overlay_image_id != 0 || g_session.modal.active)
        return false;
    if (g_session.composer_menu.kind != PIXEL_TUI_MENU_NONE &&
        (g_session.composer_menu.count <= 0 ||
         g_session.composer_menu.count != s_patched_menu_count))
        return false;
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SESSION_SNAPSHOT");
    if (snapshot && *snapshot)
        return false;
    uint32_t image_id = g_session.image_ids[g_session.state];
    return image_id != 0 && g_session.prev_frame_image == image_id &&
           g_session.prev_frame_width == g_session.surface_width &&
           g_session.prev_frame_height == g_session.surface_height;
}

static bool session_stream_mailbox_publish(const char *text, size_t bytes) {
    if (!text || bytes == 0)
        return false;
    bool perf = pixel_tui_perf_enabled();
    double wait_started_s = perf ? monotonic_s() : 0.0;
    (void)pthread_mutex_lock(&g_stream_mailbox_mutex);
    if (perf)
        pixel_tui_perf_note_producer_wait((monotonic_s() - wait_started_s) * 1000.0);
    bool coalesced = g_stream_mailbox.pending;
    if (!g_stream_mailbox.pending)
        g_stream_mailbox.pending_since_s = monotonic_s();

    if (bytes >= PIXEL_STREAM_MAILBOX_CAP) {
        text += bytes - PIXEL_STREAM_MAILBOX_CAP;
        bytes = PIXEL_STREAM_MAILBOX_CAP;
        while (bytes > 0 && ((unsigned char)*text & 0xc0) == 0x80) {
            text++;
            bytes--;
        }
        g_stream_mailbox.len = 0;
    } else if (g_stream_mailbox.len > PIXEL_STREAM_MAILBOX_CAP - bytes) {
        size_t drop = g_stream_mailbox.len - (PIXEL_STREAM_MAILBOX_CAP - bytes);
        while (drop < g_stream_mailbox.len &&
               ((unsigned char)g_stream_mailbox.text[drop] & 0xc0) == 0x80)
            drop++;
        size_t kept = g_stream_mailbox.len - drop;
        memmove(g_stream_mailbox.text, g_stream_mailbox.text + drop, kept);
        g_stream_mailbox.len = kept;
    }
    memcpy(g_stream_mailbox.text + g_stream_mailbox.len, text, bytes);
    g_stream_mailbox.len += bytes;
    g_stream_mailbox.text[g_stream_mailbox.len] = '\0';
    g_stream_mailbox.pending = true;
    atomic_store_explicit(&g_stream_mailbox_pending, true, memory_order_release);
    (void)pthread_mutex_unlock(&g_stream_mailbox_mutex);
    pixel_tui_perf_note_stream_request(coalesced);
    return true;
}

/* Drain provider deltas only on the render owner. The producer-facing path
 * never takes g_session_mutex, so token decoding cannot stall behind raster,
 * compression, terminal writes, or a slow flush. */
static bool session_drain_stream_mailbox_locked(FILE *out) {
    if (!atomic_load_explicit(&g_stream_mailbox_pending, memory_order_acquire))
        return false;
    (void)pthread_mutex_lock(&g_stream_mailbox_mutex);
    if (!g_stream_mailbox.pending || g_stream_mailbox.len == 0) {
        g_stream_mailbox.pending = false;
        g_stream_mailbox.len = 0;
        g_stream_mailbox.pending_since_s = 0.0;
        atomic_store_explicit(&g_stream_mailbox_pending, false, memory_order_release);
        (void)pthread_mutex_unlock(&g_stream_mailbox_mutex);
        return false;
    }
    if (g_session.current_message < 0) {
        (void)session_new_message("ASSISTANT", NULL);
        session_capture_set_muted(true);
    }
    pixel_message_t *message = &g_session.messages[g_session.current_message];
    bool appended = message_text_append(message, g_stream_mailbox.text, g_stream_mailbox.len);
    double pending_since_s = g_stream_mailbox.pending_since_s;
    g_stream_mailbox.pending = false;
    g_stream_mailbox.len = 0;
    g_stream_mailbox.text[0] = '\0';
    g_stream_mailbox.pending_since_s = 0.0;
    atomic_store_explicit(&g_stream_mailbox_pending, false, memory_order_release);
    (void)pthread_mutex_unlock(&g_stream_mailbox_mutex);
    if (!appended)
        return false;

    /* Provider deltas are already paced by arrival. Show all available text
     * on the next frame; animation must not add a second typewriter queue. */
    message->reveal_len = message->text_len;
    message->reveal_pending = false;
    if (!g_session.stream_repaint_pending) {
        g_session.stream_repaint_pending = true;
        g_session.stream_repaint_pending_since_s =
            pending_since_s > 0.0 ? pending_since_s : monotonic_s();
    } else if (pending_since_s > 0.0 &&
               (g_session.stream_repaint_pending_since_s <= 0.0 ||
                pending_since_s < g_session.stream_repaint_pending_since_s)) {
        g_session.stream_repaint_pending_since_s = pending_since_s;
    }
    (void)out;
    return true;
}

/* Consume one latest-state editor publication while the render thread owns
 * g_session_mutex.  Producers never wait for this work: their only critical
 * section is the bounded mailbox copy below. */
static bool session_apply_composer_mailbox_locked(FILE *out) {
    if (!atomic_load_explicit(&g_composer_mailbox_pending, memory_order_acquire))
        return false;
    composer_mailbox_t update;
    (void)pthread_mutex_lock(&g_composer_mailbox_mutex);
    if (!g_composer_mailbox.pending) {
        atomic_store_explicit(&g_composer_mailbox_pending, false, memory_order_release);
        (void)pthread_mutex_unlock(&g_composer_mailbox_mutex);
        return false;
    }
    update = g_composer_mailbox;
    g_composer_mailbox.pending = false;
    atomic_store_explicit(&g_composer_mailbox_pending, false, memory_order_release);
    (void)pthread_mutex_unlock(&g_composer_mailbox_mutex);

    session_deck_geometry_t before =
        session_deck_geometry(g_session.width, g_session.height, g_session.state);
    bool input_changed = strcmp(g_session.input, update.input) != 0;
    bool activation_changed = g_session.input_active != update.active;
    bool had_overlay = g_session.overlay_image_id != 0;
    bool reset_scroll = input_changed && g_session.transcript_scroll != 0;
    bool close_help =
        g_session.command_help_active && update.input[0] && strcmp(update.input, "/help") != 0;
    pixel_tui_menu_kind_t previous_menu = g_session.composer_menu.kind;
    if (update.input[0] && input_changed)
        session_clear_overlay(out);
    if (update.input[0] && input_changed)
        g_session.transcript_scroll = 0;
    snprintf(g_session.input, sizeof(g_session.input), "%s", update.input);
    size_t input_len = strlen(g_session.input);
    size_t next_cursor = update.cursor > input_len ? input_len : update.cursor;
    bool cursor_changed = g_session.input_cursor != next_cursor;
    g_session.input_cursor = next_cursor;
    g_session.input_active = update.active;
    if (input_changed || cursor_changed || activation_changed) {
        g_session.caret_epoch_s = monotonic_s();
        s_caret_blink_visible = true;
    }
    g_session.composer_menu = update.menu;
    if (close_help)
        g_session.command_help_active = false;
    session_place_scene(out, false);

    session_deck_geometry_t after =
        session_deck_geometry(g_session.width, g_session.height, g_session.state);
    bool same_frame = before.composer.x == after.composer.x &&
                      before.composer.y == after.composer.y &&
                      before.composer.width == after.composer.width &&
                      before.composer.height == after.composer.height;
    bool same_menu_shape = previous_menu == update.menu.kind &&
                           (update.menu.kind == PIXEL_TUI_MENU_NONE ||
                            g_session.composer_menu.count == s_patched_menu_count);
    bool fast_eligible = same_frame && same_menu_shape && !activation_changed && !close_help && !had_overlay &&
                         !reset_scroll;
    session_mark_composer_repaint(fast_eligible);
    return true;
}

/* Tool batches and swarm streams can emit telemetry from many workers at
 * once. Let the compositor coalesce those updates instead of serializing
 * agent work behind a full framebuffer encode/upload per event. The thread
 * remains available under reduced motion; only the animation tracks stop. */
static void session_telemetry_repaint(FILE *out) {
    session_schedule_repaint(out, true);
}

static void *session_animation_thread_main(void *arg) {
    (void)arg;
    session_lock();
    while (g_session.active && !g_session.animation_stop) {
        FILE *out = session_output(stderr);
        (void)session_drain_stream_mailbox_locked(out);
        (void)session_apply_composer_mailbox_locked(out);
        bool fast_before_wait = session_animation_fast();
        bool transient_before_wait = session_has_live_notice();
        int delay_ms = transient_before_wait ? 250 : 500;
        if (g_session.terminal_suspended) {
            delay_ms = 500;
        } else {
            double wait_now_s = monotonic_s();
            bool background_animation = fast_before_wait && !session_activity_visible();
            if (background_animation || g_session.stream_repaint_pending) {
                int background_delay = session_deadline_delay_ms(
                    g_session.last_paint_s, session_repaint_interval_s(), wait_now_s);
                /* Wait for the actual publication deadline. An expired full
                 * frame clock must not poll while a focused editor coalesces
                 * stream text or the live-tool region owns animation. */
                if (!background_animation && g_session.input_active &&
                    !g_session.structural_repaint_pending &&
                    g_session.stream_repaint_pending_since_s > 0.0) {
                    int semantic_delay = session_deadline_delay_ms(
                        g_session.stream_repaint_pending_since_s, 0.250, wait_now_s);
                    if (semantic_delay > background_delay) background_delay = semantic_delay;
                }
                if (background_delay < delay_ms)
                    delay_ms = background_delay;
            }
            if (session_activity_visible()) {
                int activity_delay = session_deadline_delay_ms(g_session.last_activity_paint_s,
                    (double)g_session.animation_interval_ms / 1000.0, wait_now_s);
                if (activity_delay < delay_ms) delay_ms = activity_delay;
            }
            if (g_session.composer_repaint_pending) {
                int composer_delay = session_deadline_delay_ms(
                    g_session.last_composer_paint_s,
                    session_composer_repaint_interval_s(), wait_now_s);
                if (!g_session.composer_fast_eligible || !session_composer_patch_possible()) {
                    int fallback_delay = session_deadline_delay_ms(
                        g_session.last_paint_s, session_repaint_interval_s(), wait_now_s);
                    if (fallback_delay > composer_delay) composer_delay = fallback_delay;
                }
                if (composer_delay < delay_ms)
                    delay_ms = composer_delay;
            }
            if (g_session.input_active && g_session.animation_enabled &&
                session_composer_patch_possible()) {
                int caret_delay = session_caret_deadline_ms(wait_now_s);
                if (caret_delay < delay_ms)
                    delay_ms = caret_delay;
            }
        }
        struct timespec deadline;
        animation_deadline(&deadline, delay_ms);
        double trace_wait_started = native_trace_active() ? monotonic_s() : 0.0;
        (void)pthread_cond_timedwait(&g_animation_cond, &g_session_mutex, &deadline);
        if (!g_session.active || g_session.animation_stop)
            break;
        if (g_session.terminal_suspended)
            continue;
        if (native_trace_active()) {
            native_trace_event("scheduler", delay_ms,
                trace_wait_started>0.0 ? (monotonic_s() - trace_wait_started) * 1000.0 : -1.0,
                g_session.state, fast_before_wait);
            session_trace_components();
        }
        (void)session_drain_stream_mailbox_locked(out);
        (void)session_apply_composer_mailbox_locked(out);
        /* The caret has its own clock even when background animation parks. */
        native_trace_ui_view_t trace_view;
        native_trace_ui_snapshot(&trace_view);
        if(trace_view.generation!=s_trace_ui_view.generation ||
           trace_view.state!=s_trace_ui_view.state ||
           strcmp(trace_view.label,s_trace_ui_view.label) ||
           strcmp(trace_view.detail,s_trace_ui_view.detail))
            session_mark_composer_repaint(true);
        if (g_session.input_active && session_composer_patch_possible()) {
            bool caret_visible = session_caret_visible(monotonic_s());
            if (caret_visible != s_caret_blink_visible) {
                s_caret_blink_visible = caret_visible;
                session_mark_composer_repaint(true);
            }
        }
        bool animate = session_animation_fast();
        bool transient = session_has_live_notice();
        bool resized = session_refresh_geometry_locked(out, false);
        double now = monotonic_s();
        double since_paint = now - g_session.last_paint_s;
        bool frame_due = g_session.last_paint_s <= 0.0 ||
                         since_paint >= session_repaint_interval_s();
        double since_composer_paint = now - g_session.last_composer_paint_s;
        bool composer_due = g_session.composer_repaint_pending &&
                            (g_session.last_composer_paint_s <= 0.0 ||
                             since_composer_paint >= session_composer_repaint_interval_s());
        bool repaint_due = g_session.stream_repaint_pending && frame_due;
        if (animate)
            g_session.animation_frame++;
        /* A keypress wake is not permission to run background work. Reveal,
         * transient notices, and decorative motion advance only on the
         * background deadline; otherwise each of them can turn an 8 ms editor
         * wake into a full-screen Retina paint. */
        bool revealed = session_reveal_active() && frame_due ? session_reveal_step(now) : false;
        if (revealed) {
            /* A drained stream can still reveal new transcript pixels. Retain
             * that semantic damage until published, even with editor focus;
             * otherwise only a later mouse/structural event exposes it. */
            session_schedule_repaint(out, false);
            repaint_due = frame_due;
        }
        bool animate_due = animate && frame_due;
        bool transient_due = (transient || transient_before_wait) && frame_due;
        bool composer_painted = false;
        /* Input is latency-critical even while transcript animation or a
         * provider stream is pending. Publish the small composer patch first
         * and leave the latest transcript state queued for the next frame. */
        if (!resized && composer_due && g_session.composer_fast_eligible)
            composer_painted = session_repaint_composer(out);
        bool activity_due = session_activity_visible() &&
            now - g_session.last_activity_paint_s >=
                (double)g_session.animation_interval_ms / 1000.0;
        bool activity_painted = !resized && activity_due && !composer_painted &&
                                session_repaint_activity(out);
        if (activity_due && !activity_painted && !composer_painted) {
            /* Bounded fallback (completion, patch rollover, occlusion): never
             * spin or leave a vanished card's old pixels resident. */
            g_session.last_activity_paint_s = now;
            session_schedule_repaint(out, true);
            repaint_due = frame_due;
        }
        bool composer_fallback = composer_due && !composer_painted;
        bool background_due = repaint_due || (animate_due && !activity_painted) ||
                              revealed || transient_due;
        /* Active phases keep their animation clock while the editor is ready.
         * Structural events paint at the bounded background cadence; streamed
         * text gets a coalesced opportunity after 250ms even during typing.
         * A successful caret/input patch must not starve either indefinitely. */
        bool semantic_due = repaint_due &&
            (g_session.structural_repaint_pending ||
             (g_session.stream_repaint_pending_since_s > 0.0 &&
              now - g_session.stream_repaint_pending_since_s >= 0.250));
        if (!resized && (semantic_due || (!composer_painted &&
            (composer_fallback || (background_due &&
             (!g_session.input_active || animate_due || transient_due))))))
            /* Composer fallback must respect the repaint interval: forcing an
             * unthrottled full Retina frame per keystroke (menu open, patch
             * streak capped) serializes input behind multi-hundred-ms frames.
             * Throttled calls keep composer_repaint_pending set, so the
             * fallback fires on the next cadence tick. */
            (void)session_repaint(out, repaint_due);
        ui_motion_prune(&g_session.motion, monotonic_s(), 2.0);
    }
    session_unlock();
    return NULL;
}

static void session_animation_start(void) {
    if (g_session.animation_thread_started)
        return;
    g_session.animation_stop = false;
    if (pthread_create(&g_session.animation_thread, NULL, session_animation_thread_main, NULL) ==
        0) {
        g_session.animation_thread_started = true;
        atomic_store_explicit(&g_animation_thread_fast, true, memory_order_release);
    }
}

bool pixel_tui_session_begin(FILE *out, const char *model) {
    session_lock();
    if (g_session.active) {
        session_unlock();
        return true;
    }
    if (!pixel_tui_available(out)) {
        session_unlock();
        return false;
    }
    /* Only a new session resets window ownership. Tool calls, phase changes,
     * resizes and temporary terminal handoffs retain all panels. */
    native_windows_reset();
    native_trace_ui_reset();
    s_trace_ui_rect=(native_ui_rect_t){0};s_trace_ui_armed=false;
    pixel_tui_perf_reset();
    int cols = 80, rows = 24, pixel_width = 0, pixel_height = 0;
    terminal_geometry(out, &cols, &rows, &pixel_width, &pixel_height);
    int width = 0, height = 0;
    double backing_scale = 1;
    int surface_width = 0, surface_height = 0;
    session_render_geometry(cols, rows, pixel_width, pixel_height, &width, &height, &backing_scale,
                            &surface_width, &surface_height);
    uint32_t generation = 1;
    uint32_t image_ids[4] = {0};
    uint8_t *initial_frame = NULL;
    size_t initial_frame_size = 0;

    /* Button-motion tracking supplies drags without all-motion hover traffic. */
    fprintf(out, "\033[?1049h\033[2J\033[H\033[?25l%s",
            native_mouse_enabled() ? "\033[?1000h\033[?1002h\033[?1006h" : "");
    /* Upload only the visible state. The previous four-frame eager upload made
     * startup encode and transmit four full screens before input became live;
     * other state images are produced lazily on transition. */
    bool ok = session_upload_state(out, model, width, height, backing_scale, surface_width,
                                   surface_height, generation, PIXEL_TUI_IDLE,
                                   &image_ids[PIXEL_TUI_IDLE], &initial_frame, &initial_frame_size);
    if (!ok) {
        free(initial_frame);
        for (int i = 0; i < 4; i++)
            session_delete_image(out, image_ids[i], true);
        fprintf(out, "\033[?1000l\033[?1002l\033[?1006l\033[?25h\033[?1049l");
        fflush(out);
        session_unlock();
        return false;
    }
    double started_s = monotonic_s();
    bool animations = !env_false("DSCO_PIXEL_TUI_ANIMATIONS") && !env_true("DSCO_REDUCED_MOTION") &&
                      !env_true("NO_MOTION") && !env_true("ACCESSIBILITY_REDUCE_MOTION");
    /* Native motion targets near 60 Hz. The independent composer path above has an
     * 8 ms budget, while measured expensive background frames dynamically
     * lower their own cadence instead of flooding the terminal. */
    int animation_interval_ms = 17;
    const char *fps_env = getenv("DSCO_PIXEL_TUI_FPS");
    if (fps_env && *fps_env) {
        char *end = NULL;
        long fps = strtol(fps_env, &end, 10);
        if (end != fps_env && *end == '\0') {
            if (fps < 2)
                fps = 2;
            if (fps > 60)
                fps = 60;
            animation_interval_ms = (int)((1000 + fps - 1) / fps);
        }
    }
    session_messages_free();
    free(g_session.activity_underlay);
    g_session = (pixel_session_t){.active = true,
                                  .generation = generation,
                                  .cols = cols,
                                  .rows = rows,
                                  .width = width,
                                  .height = height,
                                  .state = PIXEL_TUI_IDLE,
                                  .backing_scale = backing_scale,
                                  .surface_width = surface_width,
                                  .surface_height = surface_height,
                                  .last_paint_s = started_s,
                                  .last_composer_paint_s = started_s,
                                  .started_s = started_s,
                                  .state_started_s = started_s,
                                  .turn_started_s = started_s,
                                  .queue_capacity = 8,
                                  .saved_stdout_fd = -1,
                                  .saved_stderr_fd = -1,
                                  .devnull_fd = -1,
                                  .capture_read_fd = -1,
                                  .capture_write_fd = -1,
                                  .capture_message = -1,
                                  .animation_enabled = animations,
                                  .animation_interval_ms = animation_interval_ms,
                                  .previous_state = PIXEL_TUI_IDLE,
                                  .transition_started_s = started_s,
                                  .patch_enabled = !env_false("DSCO_PIXEL_TUI_PATCH"),
                                  .prev_frame = initial_frame,
                                  .prev_frame_cap = initial_frame_size,
                                  .prev_frame_width = surface_width,
                                  .prev_frame_height = surface_height,
                                  .prev_frame_image = image_ids[PIXEL_TUI_IDLE]};
    s_patched_menu_count = 0;
    s_caret_blink_visible = true;
    g_session.tool_view = pixel_tui_tool_view_parse(getenv("DSCO_PIXEL_TUI_TOOLS"));
    ui_motion_init(&g_session.motion, !animations);
    memcpy(g_session.image_ids, image_ids, sizeof(image_ids));
    for (int i = 0; i < 4; i++) {
        g_session.image_widths[i] = surface_width;
        g_session.image_heights[i] = surface_height;
    }
    snprintf(g_session.model, sizeof(g_session.model), "%s", model ? model : "");
    g_session.current_message = -1;
    (void)pthread_mutex_lock(&g_composer_mailbox_mutex);
    memset(&g_composer_mailbox, 0, sizeof(g_composer_mailbox));
    (void)pthread_mutex_unlock(&g_composer_mailbox_mutex);
    (void)pthread_mutex_lock(&g_stream_mailbox_mutex);
    memset(&g_stream_mailbox, 0, sizeof(g_stream_mailbox));
    (void)pthread_mutex_unlock(&g_stream_mailbox_mutex);
    atomic_store_explicit(&g_composer_mailbox_pending, false, memory_order_release);
    atomic_store_explicit(&g_stream_mailbox_pending, false, memory_order_release);
    atomic_store_explicit(&g_composer_input_active, false, memory_order_release);
    atomic_store_explicit(&g_session_suspended_fast, false, memory_order_release);
    session_place_current(out);
    fprintf(out, "\033[H\033[?25l");
    fflush(out);
    g_session.saved_stdout_fd = dup(STDOUT_FILENO);
    g_session.saved_stderr_fd = dup(STDERR_FILENO);
    int tty_dup = g_session.saved_stderr_fd >= 0 ? dup(g_session.saved_stderr_fd) : -1;
    if (tty_dup >= 0) {
        g_session.tty_out = fdopen(tty_dup, "w");
        if (g_session.tty_out)
            setvbuf(g_session.tty_out, NULL, _IONBF, 0);
        else
            close(tty_dup);
    }
    (void)session_capture_start();
    (void)session_suppress_stdio();
    session_animation_start();
    atomic_store_explicit(&g_composer_accepting_input, true, memory_order_release);
    atomic_store_explicit(&g_session_active_fast, true, memory_order_release);
    session_unlock();
    return true;
}

void pixel_tui_session_set_state(FILE *out, pixel_tui_state_t state) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (state < PIXEL_TUI_IDLE || state > PIXEL_TUI_RESPONDING)
        state = PIXEL_TUI_IDLE;
    if (state == g_session.state) {
        session_unlock();
        return;
    }
    session_clear_overlay(out);
    /* A phase is retained state, not image ownership. Keep the currently
     * placed image visible until a replacement is uploaded and placed. */
    uint32_t resident = g_session.image_ids[g_session.state];
    if (g_session.image_ids[state] && g_session.image_ids[state] != resident)
        session_delete_image(out, g_session.image_ids[state], true);
    g_session.image_ids[state] = resident;
    g_session.image_widths[state] = g_session.image_widths[g_session.state];
    g_session.image_heights[state] = g_session.image_heights[g_session.state];
    g_session.image_ids[g_session.state] = 0;
    g_session.activity_valid = false;
    g_session.previous_state = g_session.state;
    g_session.state = state;
    g_session.state_started_s = monotonic_s();
    g_session.transition_started_s = g_session.state_started_s;
    pixel_turn_visual_t *turn_visual = session_current_turn_visual();
    if (turn_visual)
        turn_visual->phase_mask |= 1u << (unsigned)state;
    session_schedule_repaint(out, true);
    session_unlock();
}

static pixel_message_t *session_new_message(const char *role, const char *detail) {
    g_session.command_help_active = false;
    if (!role || strcasecmp(role, "DSCO") != 0)
        g_session.capture_message = -1;
    int at;
    if (g_session.message_count < PIXEL_MESSAGE_CAP) {
        at = (g_session.message_start + g_session.message_count) % PIXEL_MESSAGE_CAP;
        g_session.message_count++;
    } else {
        at = g_session.message_start;
        g_session.message_start = (g_session.message_start + 1) % PIXEL_MESSAGE_CAP;
    }
    pixel_message_t *message = &g_session.messages[at];
    message_text_clear(message);
    memset(message, 0, sizeof(*message));
    snprintf(message->role, sizeof(message->role), "%s", role && *role ? role : "SYSTEM");
    if (detail && *detail) {
        plain_text_copy(message->detail, sizeof(message->detail), detail);
        /* Tool arguments are useful as a glanceable preview, not a command-log
         * dump. The complete call remains in the trace and baseline journal. */
        if (strlen(message->detail) > 112) {
            size_t cut = 109;
            while (cut > 0 && ((unsigned char)message->detail[cut] & 0xc0) == 0x80)
                cut--;
            memcpy(message->detail + cut, "...", 4);
        }
    }
    message->turn = g_session.turn;
    message->sequence = ++g_session.next_sequence;
    message->streaming = true;
    g_session.current_message = at;
    double now = monotonic_s();
    ui_motion_snap(&g_session.motion, MOTION_KEY_MESSAGE(message->sequence), MOTION_PROP_ENTRANCE,
                   0.0);
    ui_motion_set(&g_session.motion, MOTION_KEY_MESSAGE(message->sequence), MOTION_PROP_ENTRANCE,
                  1.0, 0.18, UI_MOTION_EASE_OUT, now);
    session_animation_wake();
    return message;
}

typedef enum {
    CAPTURE_TEXT = 0,
    CAPTURE_ESC,
    CAPTURE_CSI,
    CAPTURE_STRING,
    CAPTURE_STRING_ESC,
} capture_parse_state_t;

static bool capture_is_progress_tick(const char *line, size_t len) {
    if (!line || len < 3 || len > 96)
        return false;
    while (len > 0 && isspace((unsigned char)line[len - 1]))
        len--;
    if (len > 1 && line[len - 1] == ')')
        len--; /* "(12.3s)" elapsed suffix */
    if (len < 3 || line[len - 1] != 's' || !isdigit((unsigned char)line[len - 2]))
        return false;
    bool decimal = false;
    for (size_t i = len > 10 ? len - 10 : 0; i + 2 < len; i++) {
        if (isdigit((unsigned char)line[i]) && line[i + 1] == '.' &&
            isdigit((unsigned char)line[i + 2])) {
            decimal = true;
            break;
        }
    }
    if (!decimal)
        return false;
    /* Preserve semantic completion rows; only discard transient spinner ticks. */
    char preview[97];
    size_t n = len < sizeof(preview) - 1 ? len : sizeof(preview) - 1;
    memcpy(preview, line, n);
    preview[n] = '\0';
    return !strstr(preview, "response") && !strstr(preview, "completed") &&
           !strstr(preview, "failed");
}

/* Publish one real stdout/stderr line into the native transcript. This is the
 * compatibility boundary for existing dsco features: command behavior stays
 * in its original implementation while only its presentation is translated. */
static bool session_capture_publish(const char *line, size_t len) {
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    size_t start = 0;
    while (start < len && (line[start] == ' ' || line[start] == '\t'))
        start++;
    if (start == len)
        return false;

    /* The cell composer echoes its prompt before the command loop receives
     * the submitted line. User input is already inserted through the semantic
     * USER event path, so recapturing this echo creates a duplicate DSCO row
     * and can displace transient native surfaces such as /help. */
    size_t trimmed_len = len - start;
    const unsigned char *trimmed = (const unsigned char *)line + start;
    bool composer_chevron =
        trimmed_len >= 3 && ((trimmed[0] == 0xe2 && trimmed[1] == 0x80 && trimmed[2] == 0xba) ||
                             (trimmed[0] == 0xe2 && trimmed[1] == 0x9d && trimmed[2] == 0xaf));
    if (composer_chevron)
        return false;
    if (capture_is_progress_tick(line + start, trimmed_len))
        return false;

    session_lock();
    if (!g_session.active || g_session.terminal_suspended || g_session.capture_muted) {
        session_unlock();
        return false;
    }
    size_t n = len - start;
    pixel_message_t *message = NULL;
    if (g_session.capture_message >= 0 && g_session.capture_message < PIXEL_MESSAGE_CAP) {
        message = &g_session.messages[g_session.capture_message];
        if (message->text_len > 0 && (!message_text_append(message, "\n", 1) ||
                                      !message_text_append(message, line + start, n))) {
            message = NULL;
        } else if (message && message->sequence != g_session.next_sequence) {
            s_transcript_epoch++;
        }
    }
    if (!message) {
        message = session_new_message("DSCO", NULL);
        g_session.capture_message = (int)(message - g_session.messages);
    }
    if (message->text_len == 0 && !message_text_append(message, line + start, n)) {
        session_unlock();
        return false;
    }
    message->reveal_len = message->text_len;
    message->streaming = false;
    g_session.current_message = -1;
    session_unlock();
    return true;
}

void pixel_capture_parser_init(pixel_capture_parser_t *p) {
    memset(p, 0, sizeof(*p));
    p->state = CAPTURE_TEXT;
}

/* Repaint tracking: legacy spinners redraw in place with erase-line /
 * cursor-motion CSI sequences and bare '\r'. Lines built under those markers
 * are transient animation frames, not transcript content — a real terminal
 * overwrites them, so the native transcript must too. */
bool pixel_capture_parser_feed(pixel_capture_parser_t *p, const char *bytes, size_t n,
                               pixel_capture_publish_fn publish, void *ctx) {
    bool dirty = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)bytes[i];
        if (p->state == CAPTURE_ESC) {
            if (ch == '[')
                p->state = CAPTURE_CSI;
            else if (ch == ']' || ch == '_' || ch == 'P' || ch == '^')
                p->state = CAPTURE_STRING;
            else
                p->state = CAPTURE_TEXT;
            continue;
        }
        if (p->state == CAPTURE_CSI) {
            if (ch >= 0x40 && ch <= 0x7e) {
                /* Erase / cursor-motion finals mean the emitter is redrawing
                 * over existing rows (spinner frames, batch progress).
                 * Everything on this line is a repaint. */
                if (ch == 'A' || ch == 'F' || ch == 'G' || ch == 'H' || ch == 'J' || ch == 'K')
                    p->line_repaint = true;
                p->state = CAPTURE_TEXT;
            }
            continue;
        }
        if (p->state == CAPTURE_STRING) {
            if (ch == '\a')
                p->state = CAPTURE_TEXT;
            else if (ch == 0x1b)
                p->state = CAPTURE_STRING_ESC;
            continue;
        }
        if (p->state == CAPTURE_STRING_ESC) {
            p->state = (ch == '\\') ? CAPTURE_TEXT : CAPTURE_STRING;
            continue;
        }
        if (ch == 0x1b) {
            p->state = CAPTURE_ESC;
        } else if (ch == '\n') {
            if (p->len > 0 && !p->line_repaint)
                dirty |= publish(p->line, p->len, ctx);
            p->len = 0;
            p->line_repaint = false;
            p->cr_pending = false;
        } else if (ch == '\r') {
            /* Hold the line: '\r' + more text overwrites it in place
             * (progress frame, discard); '\r' + '\n' is a CRLF ending
             * (publish above). Never publish on a bare '\r'. */
            p->cr_pending = true;
        } else if (ch == '\b' || ch == 0x7f) {
            if (p->len > 0)
                p->len--;
        } else if (ch == '\t' || ch >= 0x20) {
            if (p->cr_pending) {
                p->len = 0; /* prior content was overwritten in place */
                p->cr_pending = false;
            }
            if (ch == '\t') {
                do {
                    if (p->len + 1 < sizeof(p->line))
                        p->line[p->len++] = ' ';
                } while (p->len % 4 != 0);
                continue;
            }
            if (p->len + 1 >= sizeof(p->line)) {
                if (!p->line_repaint)
                    dirty |= publish(p->line, p->len, ctx);
                p->len = 0;
            }
            p->line[p->len++] = (char)ch;
        }
    }
    return dirty;
}

bool pixel_capture_parser_finish(pixel_capture_parser_t *p, pixel_capture_publish_fn publish,
                                 void *ctx) {
    bool dirty = false;
    if (p->len > 0 && !p->line_repaint)
        dirty = publish(p->line, p->len, ctx);
    p->len = 0;
    return dirty;
}

static bool session_capture_publish_cb(const char *line, size_t len, void *ctx) {
    (void)ctx;
    return session_capture_publish(line, len);
}

static void *session_capture_thread_main(void *arg) {
    (void)arg;
    char input[2048];
    pixel_capture_parser_t parser;
    pixel_capture_parser_init(&parser);
    bool dirty = false;
    double dirty_since = 0.0;

    for (;;) {
        struct pollfd pfd = {.fd = g_session.capture_read_fd, .events = POLLIN | POLLHUP};
        int ready = poll(&pfd, 1, 50);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            if (dirty) {
                session_lock();
                if (g_session.active && !g_session.terminal_suspended)
                    session_schedule_repaint(session_output(stderr), true);
                session_unlock();
                dirty = false;
                dirty_since = 0.0;
            }
            if (g_session.capture_stop)
                break;
            continue;
        }
        if (!(pfd.revents & (POLLIN | POLLHUP)))
            continue;
        ssize_t got = read(g_session.capture_read_fd, input, sizeof(input));
        if (got <= 0) {
            if (g_session.capture_stop || (pfd.revents & POLLHUP))
                break;
            continue;
        }

        dirty |= pixel_capture_parser_feed(&parser, input, (size_t)got, session_capture_publish_cb,
                                           NULL);

        double now = monotonic_s();
        if (dirty && dirty_since <= 0.0)
            dirty_since = now;
        /* Prefer one frame after a short quiet boundary. For truly continuous
         * output, cap capture-driven repaints at 4 fps so rendering cannot
         * consume the producer or pin a core during startup/tool bursts. */
        if (dirty && now - dirty_since >= 0.25) {
            session_lock();
            if (g_session.active && !g_session.terminal_suspended)
                session_schedule_repaint(session_output(stderr), true);
            session_unlock();
            dirty = false;
            dirty_since = 0.0;
        }
    }

    dirty |= pixel_capture_parser_finish(&parser, session_capture_publish_cb, NULL);
    if (dirty) {
        session_lock();
        if (g_session.active && !g_session.terminal_suspended)
            session_schedule_repaint(session_output(stderr), true);
        session_unlock();
    }
    return NULL;
}

void pixel_tui_session_begin_message(FILE *out, const char *role, const char *detail) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    (void)session_drain_stream_mailbox_locked(out);
    session_clear_overlay(out);
    g_session.transcript_scroll = 0;
    (void)session_new_message(role, detail);
    session_capture_set_muted(true);
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_append_text(FILE *out, const char *text) {
    if (!out || !text || !*text ||
        !atomic_load_explicit(&g_session_active_fast, memory_order_acquire))
        return;
    native_trace_event("stream_input", strlen(text), 0, 0, 0);
    if (!session_stream_mailbox_publish(text, strlen(text)))
        return;
    if (atomic_load_explicit(&g_animation_thread_fast, memory_order_acquire)) {
        session_animation_wake_async();
        return;
    }

    /* Thread creation failure is rare but must still converge synchronously. */
    session_lock();
    if (g_session.active) {
        out = session_output(out);
        if (session_drain_stream_mailbox_locked(out))
            (void)session_repaint(out, true);
    }
    session_unlock();
}

void pixel_tui_session_end_message(FILE *out) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    (void)session_drain_stream_mailbox_locked(out);
    if (g_session.current_message >= 0)
        g_session.messages[g_session.current_message].streaming = false;
    g_session.current_message = -1;
    session_capture_set_muted(false);
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_add_message(FILE *out, const char *role, const char *text) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    session_clear_overlay(out);
    g_session.transcript_scroll = 0;
    pixel_message_t *message = session_new_message(role, NULL);
    if (text && *text && !message_text_set_plain(message, text)) {
        session_unlock();
        return;
    }
    message->streaming = false;
    g_session.current_message = -1;
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_show_commands(FILE *out, const pixel_tui_command_t *commands, int count) {
    session_lock();
    if (!g_session.active || !out || !commands || count < 1) {
        session_unlock();
        return;
    }
    out = session_output(out);
    session_clear_overlay(out);
    if (count > PIXEL_COMMAND_CAP)
        count = PIXEL_COMMAND_CAP;
    g_session.command_count = count;
    for (int i = 0; i < count; i++) {
        snprintf(g_session.commands[i].command, sizeof(g_session.commands[i].command), "%s",
                 commands[i].command ? commands[i].command : "");
        snprintf(g_session.commands[i].description, sizeof(g_session.commands[i].description), "%s",
                 commands[i].description ? commands[i].description : "");
    }
    g_session.command_help_active = true;
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_composer(FILE *out, const char *text, size_t cursor, bool active,
                                    pixel_tui_menu_kind_t menu_kind,
                                    const pixel_tui_menu_item_t *items, int item_count,
                                    int selected) {
    if (!out || !atomic_load_explicit(&g_composer_accepting_input, memory_order_acquire))
        return;
    const char *next = text ? text : "";
    bool perf = pixel_tui_perf_enabled();
    double wait_started_s = perf ? monotonic_s() : 0.0;
    (void)pthread_mutex_lock(&g_composer_mailbox_mutex);
    if (perf)
        pixel_tui_perf_note_producer_wait((monotonic_s() - wait_started_s) * 1000.0);
    bool simple_unchanged = strcmp(g_composer_mailbox.input, next) == 0 &&
                            g_composer_mailbox.cursor == cursor &&
                            g_composer_mailbox.active == active &&
                            g_composer_mailbox.menu.kind == PIXEL_TUI_MENU_NONE &&
                            menu_kind == PIXEL_TUI_MENU_NONE;
    if (simple_unchanged) {
        (void)pthread_mutex_unlock(&g_composer_mailbox_mutex);
        return;
    }
    native_trace_event("composer_input", strlen(next), cursor, active, item_count);
    snprintf(g_composer_mailbox.input, sizeof(g_composer_mailbox.input), "%s", next);
    size_t next_len = strlen(g_composer_mailbox.input);
    g_composer_mailbox.cursor = cursor > next_len ? next_len : cursor;
    g_composer_mailbox.active = active;
    memset(&g_composer_mailbox.menu, 0, sizeof(g_composer_mailbox.menu));
    if (menu_kind != PIXEL_TUI_MENU_NONE && items && item_count > 0) {
        if (item_count > PIXEL_COMPOSER_MENU_CAP)
            item_count = PIXEL_COMPOSER_MENU_CAP;
        g_composer_mailbox.menu.kind = menu_kind;
        g_composer_mailbox.menu.count = item_count;
        if (selected < 0)
            selected = 0;
        if (selected >= item_count)
            selected = item_count - 1;
        g_composer_mailbox.menu.selected = selected;
        for (int i = 0; i < item_count; i++) {
            pixel_composer_item_t *dst = &g_composer_mailbox.menu.items[i];
            plain_text_copy(dst->label, sizeof(dst->label), items[i].label ? items[i].label : "");
            plain_text_copy(dst->detail, sizeof(dst->detail),
                            items[i].detail ? items[i].detail : "");
            dst->disabled = items[i].disabled;
        }
    }
    g_composer_mailbox.pending = true;
    atomic_store_explicit(&g_composer_mailbox_pending, true, memory_order_release);
    atomic_store_explicit(&g_composer_input_active, active, memory_order_release);
    (void)pthread_mutex_unlock(&g_composer_mailbox_mutex);

    if (atomic_load_explicit(&g_animation_thread_fast, memory_order_acquire)) {
        bool painted = false;
        if (pthread_mutex_trylock(&g_session_mutex) == 0) {
            if (g_session.active && g_session.overlay_image_id == 0 && !g_session.modal.active &&
                g_session.patch_streak < SESSION_PATCH_STREAK_LIMIT) {
                FILE *session_out = session_output(out);
                if (session_apply_composer_mailbox_locked(session_out) &&
                    g_session.composer_fast_eligible)
                    painted = session_repaint_composer(session_out);
            }
            session_unlock();
        }
        if (!painted)
            session_animation_wake_async();
        return;
    }

    /* Thread creation failure is rare but must preserve a usable editor. */
    session_lock();
    if (g_session.active) {
        out = session_output(out);
        if (session_apply_composer_mailbox_locked(out) &&
            (!g_session.composer_fast_eligible || !session_repaint_composer(out)))
            (void)session_repaint(out, true);
    }
    session_unlock();
}

void pixel_tui_session_set_input(FILE *out, const char *text, size_t cursor, bool active) {
    pixel_tui_session_set_composer(out, text, cursor, active, PIXEL_TUI_MENU_NONE, NULL, 0, 0);
}

void pixel_tui_session_set_model(FILE *out, const char *model, const char *slot_name) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (model)
        snprintf(g_session.model, sizeof(g_session.model), "%s", model);
    if (slot_name)
        snprintf(g_session.slot_name, sizeof(g_session.slot_name), "%s", slot_name);
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_usage(FILE *out, int input_tokens, int output_tokens, double cost_usd,
                                 int turn, int tools_used) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    g_session.input_tokens = input_tokens > 0 ? input_tokens : 0;
    g_session.output_tokens = output_tokens > 0 ? output_tokens : 0;
    g_session.tools_used = tools_used > 0 ? tools_used : 0;
    g_session.cost_usd = cost_usd > 0.0 ? cost_usd : 0.0;
    (void)turn; /* semantic turn history is advanced by set_turn(). */
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_cost_details(FILE *out, double reported_usd, int reported_samples,
                                      double estimated_usd, int estimated_samples, int unpriced) {
    session_lock();
    if (g_session.active && out) {
        bool changed = g_session.reported_cost_usd != reported_usd ||
            g_session.estimated_cost_usd != estimated_usd ||
            g_session.reported_cost_samples != reported_samples ||
            g_session.estimated_cost_samples != estimated_samples ||
            g_session.unpriced_responses != unpriced;
        g_session.reported_cost_usd = reported_usd;
        g_session.estimated_cost_usd = estimated_usd;
        g_session.reported_cost_samples = reported_samples;
        g_session.estimated_cost_samples = estimated_samples;
        g_session.unpriced_responses = unpriced;
        if (changed) session_schedule_repaint(session_output(out), true);
    }
    session_unlock();
}

void pixel_tui_session_set_budget(FILE *out, double limit_usd, double burn_rate, double percent,
                                  double runway_seconds) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    g_session.budget_limit_usd = limit_usd > 0.0 ? limit_usd : 0.0;
    g_session.budget_burn_rate = burn_rate > 0.0 ? burn_rate : 0.0;
    if (percent < 0.0)
        percent = 0.0;
    if (percent > 999.0)
        percent = 999.0;
    g_session.budget_percent = percent;
    g_session.budget_runway_s = runway_seconds;
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_clock(FILE *out, bool show_clock) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    g_session.show_clock = show_clock;
    session_schedule_repaint(out, true);
    session_unlock();
}

const char *pixel_tui_theme_name(void) {
    return px_theme_active()->name;
}

bool pixel_tui_session_set_theme(FILE *out, const char *name) {
    if (!px_theme_find(name))
        return false;
    px_theme_set_active(name);
    session_lock();
    if (g_session.active && out) {
        out = session_output(out);
        session_schedule_repaint(out, true);
    }
    session_unlock();
    return true;
}

const char *pixel_tui_session_cycle_theme(FILE *out, int direction) {
    const px_theme_t *theme = px_theme_cycle(direction);
    session_lock();
    if (g_session.active && out) {
        out = session_output(out);
        session_schedule_repaint(out, true);
    }
    session_unlock();
    return theme->name;
}

void pixel_tui_session_notify(FILE *out, pixel_tui_notice_level_t level, const char *text) {
    session_lock();
    if (!g_session.active || !out || !text || !*text) {
        session_unlock();
        return;
    }
    out = session_output(out);
    for (int i = PIXEL_NOTICE_CAP - 1; i > 0; i--)
        g_session.notices[i] = g_session.notices[i - 1];
    pixel_notice_t *notice = &g_session.notices[0];
    memset(notice, 0, sizeof(*notice));
    notice->used = true;
    notice->level = level >= PIXEL_TUI_NOTICE_INFO && level <= PIXEL_TUI_NOTICE_ACTIVITY
                        ? level
                        : PIXEL_TUI_NOTICE_INFO;
    plain_text_copy(notice->text, sizeof(notice->text), text);
    notice->created_s = monotonic_s();
    notice->sequence = ++g_session.next_notice_sequence;
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_clear_notifications(FILE *out) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    memset(g_session.notices, 0, sizeof(g_session.notices));
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_show_modal(FILE *out, pixel_tui_modal_kind_t kind, const char *title,
                                  const char *subtitle, const pixel_tui_menu_item_t *items,
                                  int item_count, int selected, const char *footer) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    pixel_modal_t *modal = &g_session.modal;
    memset(modal, 0, sizeof(*modal));
    modal->active = true;
    modal->kind =
        kind >= PIXEL_TUI_MODAL_INFO && kind <= PIXEL_TUI_MODAL_MENU ? kind : PIXEL_TUI_MODAL_INFO;
    plain_text_copy(modal->title, sizeof(modal->title), title ? title : "DSCO");
    plain_text_copy(modal->subtitle, sizeof(modal->subtitle), subtitle ? subtitle : "");
    plain_text_copy(modal->footer, sizeof(modal->footer), footer ? footer : "");
    if (item_count < 0)
        item_count = 0;
    if (item_count > PIXEL_MODAL_ITEM_CAP)
        item_count = PIXEL_MODAL_ITEM_CAP;
    modal->count = item_count;
    if (selected < 0)
        selected = 0;
    if (item_count > 0 && selected >= item_count)
        selected = item_count - 1;
    modal->selected = selected;
    for (int i = 0; i < item_count; i++) {
        pixel_composer_item_t *dst = &modal->items[i];
        plain_text_copy(dst->label, sizeof(dst->label),
                        items && items[i].label ? items[i].label : "");
        plain_text_copy(dst->detail, sizeof(dst->detail),
                        items && items[i].detail ? items[i].detail : "");
        dst->disabled = items && items[i].disabled;
    }
    session_clear_overlay(out);
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_clear_modal(FILE *out) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (g_session.modal.active) {
        memset(&g_session.modal, 0, sizeof(g_session.modal));
        session_schedule_repaint(out, true);
    }
    session_unlock();
}

void pixel_tui_session_scroll(FILE *out, int lines) {
    session_lock();
    if (!g_session.active || !out || lines == 0 || g_session.command_help_active) {
        session_unlock();
        return;
    }
    out = session_output(out);
    long next = (long)g_session.transcript_scroll + lines;
    if (next < 0)
        next = 0;
    if (next > 100000)
        next = 100000;
    g_session.transcript_scroll = (int)next;
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_turn(FILE *out, int turn) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (turn < 0)
        turn = 0;
    if (turn == g_session.turn) {
        session_unlock();
        return;
    }
    session_clear_overlay(out);
    double now = monotonic_s();
    pixel_turn_visual_t *previous = session_current_turn_visual();
    if (previous && previous->turn != turn && previous->ended_s <= 0.0)
        previous->ended_s = now;
    if (!previous || previous->turn != turn) {
        if (g_session.turn_visual_count >= PIXEL_TURN_VIS_CAP) {
            memmove(&g_session.turn_visuals[0], &g_session.turn_visuals[1],
                    sizeof(g_session.turn_visuals[0]) * (PIXEL_TURN_VIS_CAP - 1));
            g_session.turn_visual_count = PIXEL_TURN_VIS_CAP - 1;
        }
        pixel_turn_visual_t *next = &g_session.turn_visuals[g_session.turn_visual_count++];
        memset(next, 0, sizeof(*next));
        next->turn = turn;
        next->started_s = now;
        next->phase_mask = 1u << (unsigned)g_session.state;
    }
    g_session.turn = turn;
    g_session.turn_started_s = now;
    g_session.thinking_bytes = 0;
    session_schedule_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_runtime_metrics(FILE *out, double cost_usd, double context_percent) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (cost_usd < 0.0)
        cost_usd = 0.0;
    if (context_percent < 0.0)
        context_percent = 0.0;
    if (context_percent > 100.0)
        context_percent = 100.0;
    bool changed = g_session.cost_usd != cost_usd || g_session.context_percent != context_percent;
    g_session.cost_usd = cost_usd;
    if (changed)
        ui_motion_set(&g_session.motion, MOTION_KEY_CONTEXT, MOTION_PROP_VALUE, context_percent,
                      0.6, UI_MOTION_SPRING, monotonic_s());
    g_session.context_percent = context_percent;
    if (changed) {
        session_schedule_repaint(out, true);
    }
    session_unlock();
}

void pixel_tui_session_set_queue_depth(FILE *out, int depth, int capacity) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (capacity < 1)
        capacity = 1;
    if (depth < 0)
        depth = 0;
    if (depth > capacity)
        depth = capacity;
    bool changed = g_session.queue_depth != depth || g_session.queue_capacity != capacity;
    g_session.queue_depth = depth;
    g_session.queue_capacity = capacity;
    if (changed)
        session_schedule_repaint(out, true);
    session_unlock();
}

uint64_t pixel_tui_session_tool_begin(FILE *out, const char *name, const char *input_json) {
    session_lock();
    if (!g_session.active || !out || !name || !*name) {
        session_unlock();
        return 0;
    }
    out = session_output(out);
    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    int completed_slot = -1;
    uint64_t oldest_completed = UINT64_MAX;
    for (int i = 0; i < PIXEL_TOOL_VIS_CAP; i++) {
        if (!g_session.tool_visuals[i].used) {
            slot = i;
            completed_slot = -1;
            break;
        }
        if (g_session.tool_visuals[i].status != PIXEL_OP_RUNNING &&
            g_session.tool_visuals[i].sequence < oldest_completed) {
            oldest_completed = g_session.tool_visuals[i].sequence;
            completed_slot = i;
        }
        if (g_session.tool_visuals[i].sequence < oldest) {
            oldest = g_session.tool_visuals[i].sequence;
            slot = i;
        }
    }
    /* Keep slow calls visible when bursts reuse the bounded card deck. */
    if (completed_slot >= 0)
        slot = completed_slot;
    pixel_tool_visual_t *tool = &g_session.tool_visuals[slot];
    memset(tool, 0, sizeof(*tool));
    tool->used = true;
    tool->sequence = ++g_session.next_tool_sequence;
    tool->status = PIXEL_OP_RUNNING;
    tool->started_s = monotonic_s();
    plain_text_copy(tool->name, sizeof(tool->name), name);
    tool_preview_extract(name, input_json, tool->preview, sizeof(tool->preview));
    /* Card entrance mirrors message arrival: 0 → 1 over 0.18s. */
    ui_motion_snap(&g_session.motion, MOTION_KEY_TOOL(tool->sequence), MOTION_PROP_ENTRANCE, 0.0);
    ui_motion_set(&g_session.motion, MOTION_KEY_TOOL(tool->sequence), MOTION_PROP_ENTRANCE, 1.0,
                  0.18, UI_MOTION_EASE_OUT, tool->started_s);
    pixel_turn_visual_t *turn_visual = session_current_turn_visual();
    if (turn_visual) {
        turn_visual->tool_count++;
        turn_visual->phase_mask |= 1u << PIXEL_TUI_EXECUTING;
    }
    session_clear_overlay(out);
    uint64_t operation_id = tool->sequence;
    /* Durable transcript row for this call. It starts as a running line and
     * is completed in place by tool_end — never a second message. */
    pixel_message_t *row = session_new_message("TOOL", tool->preview);
    row->tool_row = true;
    row->tool_status = -1;
    row->tool_operation_id = operation_id;
    plain_text_copy(row->tool_name, sizeof(row->tool_name), name);
    /* The row is not a streaming text target; assistant deltas must open
     * their own message. */
    g_session.current_message = -1;
    session_telemetry_repaint(out);
    session_unlock();
    return operation_id;
}

void pixel_tui_session_tool_end(FILE *out, uint64_t operation_id, const char *name, bool ok,
                                double elapsed_ms, const char *result) {
    session_lock();
    if (!g_session.active || !out || !name || !*name) {
        session_unlock();
        return;
    }
    out = session_output(out);
    pixel_tool_visual_t *match = NULL;
    for (int i = 0; i < PIXEL_TOOL_VIS_CAP; i++) {
        pixel_tool_visual_t *tool = &g_session.tool_visuals[i];
        if (!tool->used || tool->status != PIXEL_OP_RUNNING)
            continue;
        if (operation_id != 0 && tool->sequence == operation_id) {
            match = tool;
            break;
        }
        if (operation_id == 0 && !strcmp(tool->name, name) &&
            (!match || tool->sequence > match->sequence))
            match = tool;
    }
    if (match) {
        match->status = ok ? PIXEL_OP_DONE : PIXEL_OP_ERROR;
        match->elapsed_ms = elapsed_ms >= 0.0 ? elapsed_ms : 0.0;
        double now = monotonic_s();
        ui_motion_snap(&g_session.motion, MOTION_KEY_TOOL(match->sequence), MOTION_PROP_FLASH, 1.0);
        ui_motion_set(&g_session.motion, MOTION_KEY_TOOL(match->sequence), MOTION_PROP_FLASH, 0.0,
                      ok ? 0.9 : 1.6, UI_MOTION_EASE_OUT, now);
        /* Presence: hold, then ease the card away while the durable TOOL row
         * slides in — a crossfade morph with zero coordination code. */
        ui_motion_snap(&g_session.motion, MOTION_KEY_TOOL(match->sequence), MOTION_PROP_VALUE, 1.0);
        ui_motion_set(&g_session.motion, MOTION_KEY_TOOL(match->sequence), MOTION_PROP_VALUE, 0.0,
                      0.45, UI_MOTION_EASE_OUT, now);
    }
    pixel_message_t *row = session_find_tool_message(operation_id, name);
    if (row) {
        if (row->sequence != g_session.next_sequence)
            s_transcript_epoch++;
        row->tool_status = ok ? 1 : 0;
        row->tool_elapsed_ms = elapsed_ms >= 0.0 ? (float)elapsed_ms : 0.0f;
        row->streaming = false;
        char preview[PIXEL_TOOL_RESULT_FULL_BYTES + 1U];
        pixel_tui_tool_result_preview(result, g_session.tool_view, preview, sizeof(preview),
                                      &row->tool_tail_lines, &row->tool_total_bytes);
        (void)message_text_set_plain(row, preview);
    }
    session_telemetry_repaint(out);
    session_unlock();
}

void pixel_tui_session_swarm_update(FILE *out, int child_id, const char *status, const char *task,
                                    const char *model, size_t output_bytes, double cost_usd) {
    /* Streaming telemetry is lossy by design: never stop pipe draining or
     * worker reaping behind a framebuffer-wide lock. Spawn/terminal lifecycle
     * updates remain lossless and may wait briefly for the compositor. */
    bool streaming = status && strcmp(status, "streaming") == 0;
    if (streaming) {
        if (pthread_mutex_trylock(&g_session_mutex) != 0)
            return;
    } else {
        session_lock();
    }
    if (!g_session.active || !out || child_id < 0) {
        session_unlock();
        return;
    }
    out = session_output(out);
    int slot = -1;
    int oldest_slot = 0;
    double oldest_update = 1e100;
    for (int i = 0; i < PIXEL_SWARM_VIS_CAP; i++) {
        pixel_swarm_visual_t *agent = &g_session.swarm_visuals[i];
        if (agent->used && agent->child_id == child_id) {
            slot = i;
            break;
        }
        if (!agent->used && slot < 0)
            slot = i;
        if (agent->updated_s < oldest_update) {
            oldest_update = agent->updated_s;
            oldest_slot = i;
        }
    }
    if (slot < 0)
        slot = oldest_slot;
    pixel_swarm_visual_t *agent = &g_session.swarm_visuals[slot];
    bool is_new = !agent->used || agent->child_id != child_id;
    char previous_status[sizeof(agent->status)];
    snprintf(previous_status, sizeof(previous_status), "%s", agent->status);
    double now = monotonic_s();
    bool new_lifecycle =
        is_new || (swarm_visual_terminal(previous_status) && !swarm_visual_terminal(status));
    if (new_lifecycle)
        memset(agent, 0, sizeof(*agent));
    agent->used = true;
    agent->child_id = child_id;
    plain_text_copy(agent->status, sizeof(agent->status), status ? status : "running");
    if (task && *task)
        plain_text_copy(agent->task, sizeof(agent->task), task);
    if (model && *model)
        plain_text_copy(agent->model, sizeof(agent->model), model);
    agent->output_bytes = output_bytes;
    agent->cost_usd = cost_usd > 0.0 ? cost_usd : 0.0;
    bool status_changed = strcmp(previous_status, agent->status) != 0;
    bool repaint = new_lifecycle || status_changed || now - agent->updated_s >= 0.16;
    agent->updated_s = now;
    if (new_lifecycle) {
        pixel_turn_visual_t *turn_visual = session_current_turn_visual();
        if (turn_visual)
            turn_visual->swarm_count++;
    }
    if (repaint)
        session_telemetry_repaint(out);
    session_unlock();
}

/* Reasoning streams (GLM's reasoning_content, OpenRouter's reasoning) carry
 * model-native newlines verbatim — providers concatenate chunks with no
 * separator, so token-boundary and soft-wrap newlines land in the text. The
 * transcript renderer is line-oriented (every '\n' is a hard visual break),
 * which stacks each short fragment on its own row. Flow reasoning as prose:
 * collapse any run of whitespace to a single space, dropping a leading space
 * when the buffer is empty or already ends in one so cross-delta boundaries
 * don't double-space. */
static bool message_text_append_flowed(pixel_message_t *message, const char *delta) {
    if (!message || !delta)
        return false;
    size_t n = strlen(delta);
    if (n == 0)
        return true;
    char *flowed = malloc(n + 1U);
    if (!flowed)
        return message_text_append(message, delta, n);
    size_t w = 0;
    bool prev_space = message->text_len == 0 ||
                      (unsigned char)message->text[message->text_len - 1] == ' ';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)delta[i];
        bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
        if (space) {
            if (!prev_space)
                flowed[w++] = ' ';
            prev_space = true;
        } else {
            flowed[w++] = (char)c;
            prev_space = false;
        }
    }
    bool ok = w > 0 ? message_text_append(message, flowed, w) : true;
    free(flowed);
    return ok;
}

void pixel_tui_session_note_thinking(FILE *out, const char *delta) {
    /* Reasoning preview is cosmetic and high frequency. Provider callbacks
     * must never block on an in-progress Retina repaint; a later delta/final
     * answer will advance the transcript. */
    if (pthread_mutex_trylock(&g_session_mutex) != 0)
        return;
    if (!g_session.active || !out || !delta) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (g_session.current_message < 0) {
        (void)session_new_message("THINKING", NULL);
        session_capture_set_muted(true);
        g_session.thinking_bytes = 0;
    }
    pixel_message_t *message = &g_session.messages[g_session.current_message];
    g_session.thinking_bytes += strlen(delta);
    snprintf(message->detail, sizeof(message->detail), "reasoning stream / ~%zu tokens",
             g_session.thinking_bytes / 4);
    /* The token counter above is a glanceable summary; without appending the
     * actual delta the transcript shows no reasoning text at all while the
     * model is thinking. Stream it as flowing prose so the line-oriented
     * transcript wraps it by width instead of honoring every model newline. */
    if (message_text_append_flowed(message, delta)) {
        message_text_trim_front(message, PIXEL_THINKING_TAIL_MAX);
        message->reveal_len = message->text_len;
        message->reveal_pending = false;
    }
    session_schedule_repaint(out, false);
    session_animation_wake();
    session_unlock();
}

void pixel_tui_session_terminal_control(FILE *out, const char *sequence) {
    session_lock();
    if (!g_session.active || !out || !sequence) {
        session_unlock();
        return;
    }
    out = session_output(out);
    fputs(sequence, out);
    fflush(out);
    session_unlock();
}

bool pixel_tui_session_pointer(FILE *out, int button, int column, int row, bool released) {
    session_lock();
    if (!out || !g_session.active || g_session.terminal_suspended ||
        g_session.cols < 1 || g_session.rows < 1 || column < 1 || row < 1 || button < 0) {
        session_unlock();
        return false;
    }
    if (column > g_session.cols || row > g_session.rows) {
        s_trace_ui_armed=false;
        if (released) native_windows_cancel_gesture();
        session_unlock();
        return false;
    }
    if (g_session.modal.active || g_session.command_help_active ||
        g_session.composer_menu.kind != PIXEL_TUI_MENU_NONE) {
        s_trace_ui_armed=false;
        native_windows_cancel_gesture();
        session_unlock();
        return true; /* modal/menu owns the packet; do not scroll behind it */
    }
    /* SGR 1006 coordinates are one-based cells. Map their centers to the
     * actual logical canvas (including clamped/DPR geometry), not pixels. */
    int x = (int)(((int64_t)column * 2 - 1) * g_session.width / ((int64_t)g_session.cols * 2));
    int y = (int)(((int64_t)row * 2 - 1) * g_session.height / ((int64_t)g_session.rows * 2));
    native_trace_event("pointer", button, x, y, released);
    /* The footer is shorter than some terminal cells. Include their centers
     * around the badge so every supported geometry has a usable mouse target. */
    int trace_pad=(g_session.height+g_session.rows-1)/g_session.rows/2;
    bool trace_hit=s_trace_ui_rect.width>0 && x>=s_trace_ui_rect.x &&
        x<s_trace_ui_rect.x+s_trace_ui_rect.width && y>=s_trace_ui_rect.y-trace_pad &&
        y<s_trace_ui_rect.y+s_trace_ui_rect.height+trace_pad;
    if(button==0 && !released && trace_hit) {
        native_windows_cancel_gesture();
        s_trace_ui_pressed=s_trace_ui_view;s_trace_ui_armed=true;
        session_unlock();return true;
    }
    if(s_trace_ui_armed) {
        bool activate=released && button==0 && trace_hit;
        native_trace_ui_view_t pressed=s_trace_ui_pressed;
        if(released || !trace_hit || (button!=0 && button!=32))s_trace_ui_armed=false;
        session_unlock();
        if(activate) {
            (void)native_trace_ui_activate(&pressed,getenv("DSCO_TRUST_TIER"));
            session_lock();
            if(g_session.active) {session_mark_composer_repaint(true);session_animation_wake();}
            session_unlock();
        }
        return true;
    }
    native_windows_set_work_area(session_windows_work_area(g_session.width, g_session.height, g_session.state));
    native_windows_set_pointer_cell((g_session.width + g_session.cols - 1) / g_session.cols,
                                    (g_session.height + g_session.rows - 1) / g_session.rows);
    bool was_focused = native_windows_focused();
    bool consumed = native_windows_pointer(button, x, y, released);
    if (consumed || was_focused != native_windows_focused()) {
        out = session_output(out);
        session_place_scene(out, false);
        session_schedule_repaint(out, true);
    }
    session_unlock();
    return consumed;
}

bool pixel_tui_session_window_key(FILE *out, int key, unsigned modifiers) {
    session_lock();
    if (!out || !g_session.active || g_session.terminal_suspended || g_session.modal.active ||
        g_session.composer_menu.kind != PIXEL_TUI_MENU_NONE ||
        (g_session.command_help_active && key != NATIVE_WINDOW_KEY_TOGGLE_FOCUS)) {
        session_unlock();
        return false;
    }
    if (key == NATIVE_WINDOW_KEY_TOGGLE_FOCUS)
        g_session.command_help_active = false;
    native_windows_set_work_area(session_windows_work_area(g_session.width, g_session.height, g_session.state));
    bool consumed = native_windows_key(key, modifiers);
    /* Ctrl-S publishes a bounded request only after native_windows_key has
     * released its model mutex. The governed buffer adapter may re-enter the
     * model for observation and must never run while retained state is locked. */
    if(consumed && (key==0x03 || key==0x18 || key==0x16)) {
        session_unlock();bool clip_ok=native_windows_clipboard_key(key);session_lock();
        if(!g_session.active || g_session.terminal_suspended){session_unlock();return consumed;}
        native_windows_editor_feedback(clip_ok?"Clipboard action completed.":"Clipboard action rejected: denied, sensitive, empty, invalid, or changed selection; original retained.");
    }
    if(consumed && (key==0x02 || key==0x04)) {
        session_unlock();(void)native_windows_reuse_key(key);session_lock();
        if(!g_session.active || g_session.terminal_suspended){session_unlock();return consumed;}
    }
    bool save_attempted=false;
    char save_result[65536];
    if(consumed && (key==NATIVE_WINDOW_KEY_SAVE || key==0x13)) {
        /* Never run filesystem/governed callbacks under the compositor mutex. */
        session_unlock();
        (void)native_windows_save_pending(&save_attempted,save_result,sizeof(save_result));
        session_lock();
        if(!g_session.active || g_session.terminal_suspended) { session_unlock(); return consumed; }
    }
    if (consumed) {
        out = session_output(out);
        session_place_scene(out, false);
        session_schedule_repaint(out, true);
    }
    session_unlock();
    return consumed;
}

void pixel_tui_session_windows_changed(FILE *out) {
    /* Model mutations release their own lock before notifying the compositor. */
    session_lock();
    if (out && g_session.active && !g_session.terminal_suspended) {
        out = session_output(out);
        native_windows_set_work_area(session_windows_work_area(g_session.width, g_session.height, g_session.state));
        session_place_scene(out, false);
        session_schedule_repaint(out, true);
    }
    session_unlock();
}

void pixel_tui_session_suspend_terminal(FILE *out) {
    session_lock();
    if (!g_session.active || g_session.terminal_suspended) {
        session_unlock();
        return;
    }
    out = session_output(out);
    native_windows_cancel_gesture();
    session_clear_overlay(out);
    if (g_session.scene_placed)
        session_delete_image(out, g_session.scene_image_id, false);
    g_session.scene_placed = false;
    session_delete_image(out, g_session.image_ids[g_session.state], false);
    fputs("\033[?1000l\033[?1002l\033[?1006l\033[?25h\033[0m\033[?1049l", out);
    fflush(out);
    session_restore_stdio();
    g_session.terminal_suspended = true;
    atomic_store_explicit(&g_session_suspended_fast, true, memory_order_release);
    session_animation_wake();
    session_unlock();
}

void pixel_tui_session_resume_terminal(FILE *out) {
    session_lock();
    if (!g_session.active || !g_session.terminal_suspended) {
        session_unlock();
        return;
    }
    out = session_output(out);
    fputs("\033[?1049h\033[2J\033[H\033[?25l\033[?1000h\033[?1002h\033[?1006h", out);
    g_session.terminal_suspended = false;
    g_session.scene_dirty = g_session.scene_json != NULL;
    atomic_store_explicit(&g_session_suspended_fast, false, memory_order_release);
    (void)session_suppress_stdio();
    (void)session_repaint(out, true);
    session_animation_wake();
    session_unlock();
}

bool pixel_tui_session_zoom(FILE *out, const char *value, char *result, size_t result_cap) {
    session_lock();
    bool ok = false;
    if (!g_session.active || !out || g_session.terminal_suspended) {
        snprintf(result, result_cap, "Zoom is available in a native session (dsco --native).");
        goto done;
    }
    out = session_output(out);
    int cols = g_session.cols, rows = g_session.rows, pw = 0, ph = 0;
    (void)terminal_geometry(out, &cols, &rows, &pw, &ph);
    int dpr = native_ui_terminal_viewport(cols, rows, pw, ph, requested_device_scale()).backing_scale;
    double zoom = render_logical_scale(cols, rows, pw, ph) / dpr;
    if (value && *value) {
        if (!strcmp(value, "+")) zoom = zoom * 1.1;
        else if (!strcmp(value, "-")) zoom = zoom / 1.1;
        else if (!native_display_zoom_parse(value, &zoom)) {
            snprintf(result, result_cap, "Use any positive zoom, e.g. /zoom 0.75, /zoom 0.05, /zoom 150%%, or /zoom auto.");
            goto done;
        }
        s_display_zoom_override = zoom;
        (void)session_refresh_geometry_locked(out, true);
        zoom = render_logical_scale(cols, rows, pw, ph) / dpr;
    }
    snprintf(result, result_cap, "Native zoom: %.6g%%. Adjust with /zoom + or /zoom -, reset with /zoom auto.", zoom * 100.0);
    ok = true;
done:
    session_unlock();
    return ok;
}

void pixel_tui_session_refresh(FILE *out) {
    session_lock();
    if (!g_session.active || !out || g_session.terminal_suspended) {
        session_unlock();
        return;
    }
    out = session_output(out);
    (void)session_refresh_geometry_locked(out, true);
    session_unlock();
}

void pixel_tui_session_end(FILE *out) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    atomic_store_explicit(&g_session_active_fast, false, memory_order_release);
    atomic_store_explicit(&g_composer_accepting_input, false, memory_order_release);
    atomic_store_explicit(&g_animation_thread_fast, false, memory_order_release);
    atomic_store_explicit(&g_composer_input_active, false, memory_order_release);
    out = session_output(out);
    session_clear_overlay(out);
    session_release_scene(out);
    native_windows_reset();
    for (int i = 0; i < 4; i++)
        session_delete_image(out, g_session.image_ids[i], true);
    fprintf(out, "\033[?1000l\033[?1002l\033[?1006l\033[?25h\033[0m\033[?1049l");
    fflush(out);
    session_restore_stdio();
    g_session.terminal_suspended = true;
    g_session.animation_stop = true;
    (void)pthread_cond_broadcast(&g_animation_cond);
    g_session.capture_stop = true;
    int capture_write_fd = g_session.capture_write_fd;
    g_session.capture_write_fd = -1;
    bool join_capture = g_session.capture_thread_started;
    pthread_t capture_thread = g_session.capture_thread;
    bool join_animation = g_session.animation_thread_started;
    pthread_t animation_thread = g_session.animation_thread;
    if (capture_write_fd >= 0)
        close(capture_write_fd);
    session_unlock();

    if (join_animation)
        (void)pthread_join(animation_thread, NULL);
    if (join_capture)
        (void)pthread_join(capture_thread, NULL);
    native_trace_shutdown();

    session_lock();
    g_session.animation_thread_started = false;
    g_session.capture_thread_started = false;
    if (g_session.capture_read_fd >= 0)
        close(g_session.capture_read_fd);
    if (g_session.tty_out)
        fclose(g_session.tty_out);
    if (g_session.saved_stdout_fd >= 0)
        close(g_session.saved_stdout_fd);
    if (g_session.saved_stderr_fd >= 0)
        close(g_session.saved_stderr_fd);
    if (g_session.devnull_fd >= 0)
        close(g_session.devnull_fd);
    free(g_session.prev_frame);
    free(g_session.activity_underlay);
    free(g_session.patch_buffer);
    free(s_visual_lines);
    s_visual_lines = NULL;
    s_visual_line_cap = 0;
    free(s_rich_tokens);
    s_rich_tokens = NULL;
    s_rich_token_cap = 0;
    s_visual_cached_chars = 0;
    s_visual_cached_message_start = 0;
    s_visual_cached_message_count = 0;
    s_visual_cached_line_count = 0;
    s_visual_cached_last_sequence = 0;
    session_messages_free();
    memset(&g_session, 0, sizeof(g_session));
    (void)pthread_mutex_lock(&g_composer_mailbox_mutex);
    memset(&g_composer_mailbox, 0, sizeof(g_composer_mailbox));
    (void)pthread_mutex_unlock(&g_composer_mailbox_mutex);
    (void)pthread_mutex_lock(&g_stream_mailbox_mutex);
    memset(&g_stream_mailbox, 0, sizeof(g_stream_mailbox));
    (void)pthread_mutex_unlock(&g_stream_mailbox_mutex);
    atomic_store_explicit(&g_composer_mailbox_pending, false, memory_order_release);
    atomic_store_explicit(&g_stream_mailbox_pending, false, memory_order_release);
    atomic_store_explicit(&g_session_suspended_fast, false, memory_order_release);
    session_unlock();
    (void)pixel_tui_perf_report_from_env(stderr);
}

int pixel_tui_render_plan(FILE *out, int plan_id) {
    pixel_plan_view_t view = PIXEL_PLAN_VIEW_TREE;
    const char *requested = getenv("DSCO_PLAN_VIEW");
    if (requested && (!strcasecmp(requested, "actions") || !strcasecmp(requested, "action") ||
                      !strcasecmp(requested, "dag")))
        view = PIXEL_PLAN_VIEW_ACTIONS;
    return pixel_tui_render_plan_view(out, plan_id, view);
}

int pixel_tui_render_plan_view(FILE *out, int plan_id, pixel_plan_view_t view) {
    session_lock();
    bool in_session = g_session.active;
    if (in_session)
        out = session_output(out);
    else if (!pixel_tui_available(out)) {
        session_unlock();
        return 0;
    }
    int cols = 80, rows = 24, pixel_width = 0, pixel_height = 0;
    terminal_geometry(out, &cols, &rows, &pixel_width, &pixel_height);
    double dpr = render_device_scale(cols, rows, pixel_width, pixel_height);
    int width = pixel_width > 0 ? pixel_width / dpr - 24 : cols * 10;
    if (width < 640)
        width = 640;
    if (width > 1280)
        width = 1280;
    px_canvas_t *c = view == PIXEL_PLAN_VIEW_ACTIONS ? render_plan_actions_frame(plan_id, width)
                                                     : render_plan_frame(plan_id, width, NULL);
    if (!c) {
        session_unlock();
        return 0;
    }
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SNAPSHOT");
    if (snapshot && *snapshot)
        (void)pixel_tui_write_plan_view_ppm(snapshot, plan_id, width, view);

    int cell_h = (pixel_height > 0 && rows > 0) ? pixel_height / dpr / rows : 20;
    if (cell_h < 8)
        cell_h = 20;
    int occupied_rows = (c->height + cell_h - 1) / cell_h;
    if (occupied_rows < 4)
        occupied_rows = 4;
    int row_cap = in_session ? rows - 7 : rows - 2;
    if (occupied_rows > row_cap)
        occupied_rows = row_cap;
    int placement_cols = cols > 2 ? cols - 2 : cols;

    uint32_t image_id = 0x44534300U ^ ((uint32_t)getpid() << 5) ^ (uint32_t)plan_id;
    if (image_id == 0)
        image_id = 1;
    if (in_session)
        session_clear_overlay(out);
    char control[256];
    snprintf(control, sizeof(control), "a=T,t=d,f=24,s=%d,v=%d,i=%u,c=%d,r=%d,C=1,z=2,q=2,o=z",
             c->pixel_width, c->pixel_height, image_id, placement_cols, occupied_rows);
    bool sent = send_kitty_pixels(out, control, c, false, NULL);
    free_canvas(c);
    if (!sent) {
        session_unlock();
        return 0;
    }
    if (in_session)
        g_session.overlay_image_id = image_id;
    else
        for (int i = 0; i < occupied_rows; i++)
            fputc('\n', out);
    if (in_session)
        session_place_scene(out, false);
    fflush(out);
    int result = ferror(out) ? 0 : occupied_rows;
    session_unlock();
    return result;
}

/* ── Generative UI: native_ui scenes on the pixel surface ────────────────
 * This is the first real runtime backend for the retained scene graph:
 * agents emit declarative JSON (native_ui_scene_from_json), the compositor
 * lays it out, and this backend paints it with the same effects core as the
 * session shell — AA rounded surfaces, shadows, token-mapped color. */

typedef struct {
    px_canvas_t *canvas;
    pixel_fx_surface_t fx;
    int origin_x;
    int origin_y;
    pixel_tui_state_t state;
} scene_pixel_host_t;

static native_ui_rect_t scene_host_rect(const scene_pixel_host_t *host, native_ui_rect_t rect) {
    rect.x += host->origin_x;
    rect.y += host->origin_y;
    return rect;
}

static px_color_t scene_host_color(px_backend_color_t color) {
    return (px_color_t){color.r, color.g, color.b};
}

static px_backend_color_t scene_backend_color(px_color_t color) {
    return (px_backend_color_t){color.r, color.g, color.b};
}

static px_backend_palette_t scene_backend_palette(px_color_t accent) {
    px_backend_palette_t palette = px_theme_palette(NULL);
    palette.colors[NATIVE_UI_COLOR_ACCENT] = scene_backend_color(accent);
    palette.colors[NATIVE_UI_COLOR_FOCUS] = scene_backend_color(accent);
    return palette;
}

static void scene_host_push_clip(void *surface, native_ui_rect_t rect) {
    scene_pixel_host_t *host = surface;
    rect = scene_host_rect(host, rect);
    double scale = host->canvas->backing_scale;
    (void)pixel_fx_clip_push(&host->fx, rect.x * scale, rect.y * scale, rect.width * scale,
                             rect.height * scale);
}

static void scene_host_pop_clip(void *surface) {
    scene_pixel_host_t *host = surface;
    pixel_fx_clip_pop(&host->fx);
}

static void scene_host_fill_rect(void *surface, native_ui_rect_t rect, px_backend_color_t color,
                                 uint8_t opacity, uint8_t radius, bool raised) {
    scene_pixel_host_t *host = surface;
    rect = scene_host_rect(host, rect);
    double scale = host->canvas->backing_scale;
    double alpha = (double)opacity / 255.0;
    if (raised && radius > 0)
        pixel_fx_shadow(&host->fx, rect.x * scale, rect.y * scale, rect.width * scale,
                        rect.height * scale, radius * scale, 12 * scale, 3 * scale,
                        (pixel_fx_rgb_t){0, 0, 0}, 0.38 * alpha);
    pixel_fx_fill_rounded(&host->fx, rect.x * scale, rect.y * scale, rect.width * scale,
                          rect.height * scale, radius * scale, fx_color(scene_host_color(color)),
                          alpha);
}

static void scene_host_stroke_rect(void *surface, native_ui_rect_t rect, px_backend_color_t color,
                                   uint8_t opacity, uint8_t width, uint8_t radius) {
    scene_pixel_host_t *host = surface;
    rect = scene_host_rect(host, rect);
    double scale = host->canvas->backing_scale;
    pixel_fx_stroke_rounded(&host->fx, rect.x * scale, rect.y * scale, rect.width * scale,
                            rect.height * scale, radius * scale, (width > 0 ? width : 1) * scale,
                            fx_color(scene_host_color(color)), (double)opacity / 255.0);
}

static void scene_host_draw_text(void *surface, native_ui_rect_t rect, const char *text,
                                 native_ui_type_token_t type, px_backend_color_t color,
                                 uint8_t opacity) {
    scene_pixel_host_t *host = surface;
    rect = scene_host_rect(host, rect);
    if (!text || !*text || rect.width < 4)
        return;
    bool title = type == NATIVE_UI_TYPE_TITLE || type == NATIVE_UI_TYPE_METRIC;
    float size = title ? 15.0f : 11.0f;
    int line_h = font_compat_prose_line_height(size, title);
    if (line_h < 1) line_h = title ? 18 : 14;
    int y = rect.y + (rect.height - line_h) / 2;
    if (y < rect.y)
        y = rect.y;
    draw_ui_label(host->canvas, rect.x, y, size, title, text, scene_host_color(color),
                  (double)opacity / 255.0, rect.width);
}

static void scene_host_draw_icon(void *surface, native_ui_rect_t rect, const char *name,
                                 px_backend_color_t color, uint8_t opacity) {
    scene_pixel_host_t *host = surface;
    (void)name;
    rect = scene_host_rect(host, rect);
    int cx = rect.x + rect.width / 2;
    int cy = rect.y + rect.height / 2;
    int radius = (rect.width < rect.height ? rect.width : rect.height) / 3;
    if (radius < 2)
        radius = 2;
    px_color_t resolved = scene_host_color(color);
    fill_circle(host->canvas, cx, cy, radius, resolved, (double)opacity / 300.0);
    draw_circle_ring(host->canvas, cx, cy, radius + 2, 1, resolved, (double)opacity / 640.0);
}

static void scene_host_draw_line(void *surface, int x0, int y0, int x1, int y1,
                                 px_backend_color_t color, uint8_t opacity) {
    scene_pixel_host_t *host = surface;
    draw_line(host->canvas, x0 + host->origin_x, y0 + host->origin_y, x1 + host->origin_x,
              y1 + host->origin_y, scene_host_color(color), (double)opacity / 255.0);
}

static void scene_host_fill_circle(void *surface, int cx, int cy, int radius,
                                   px_backend_color_t color, uint8_t opacity) {
    scene_pixel_host_t *host = surface;
    fill_circle(host->canvas, cx + host->origin_x, cy + host->origin_y, radius,
                scene_host_color(color), (double)opacity / 255.0);
}

static void scene_host_draw_custom(void *surface, const native_ui_node_t *node,
                                   const px_backend_palette_t *palette) {
    scene_pixel_host_t *host = surface;
    (void)palette;
    if (node && node->key == NATIVE_MASTHEAD_KEY_SOUL) {
        native_ui_rect_t rect = scene_host_rect(host, node->frame);
        int radius = (rect.width < rect.height ? rect.width : rect.height) / 2 - 2;
        if (radius < 10)
            radius = 10;
        draw_dsco_soul(host->canvas, rect.x + rect.width / 2, rect.y + rect.height / 2, radius,
                       host->state);
    } else if (node && node->key == NATIVE_COMPOSER_KEY_INPUT) {
        native_ui_rect_t rect = scene_host_rect(host, node->frame);
        draw_session_input(host->canvas, rect.x, rect.y, rect.width, rect.height);
    }
}

static void scene_pixel_backend_init(px_backend_t *backend, scene_pixel_host_t *host,
                                     px_canvas_t *canvas, int origin_x, int origin_y,
                                     pixel_tui_state_t state, px_color_t accent) {
    *host = (scene_pixel_host_t){
        .canvas = canvas,
        .fx = fx_surface(canvas),
        .origin_x = origin_x,
        .origin_y = origin_y,
        .state = state,
    };
    px_backend_palette_t palette = scene_backend_palette(accent);
    px_backend_ops_t ops = {
        .push_clip = scene_host_push_clip,
        .pop_clip = scene_host_pop_clip,
        .fill_rect = scene_host_fill_rect,
        .stroke_rect = scene_host_stroke_rect,
        .draw_text = scene_host_draw_text,
        .draw_icon = scene_host_draw_icon,
        .draw_line = scene_host_draw_line,
        .fill_circle = scene_host_fill_circle,
        .draw_custom = scene_host_draw_custom,
    };
    px_backend_init(backend, host, &palette, &ops);
}

static bool draw_session_masthead(px_canvas_t *canvas, native_ui_rect_t frame, const char *model,
                                  pixel_tui_state_t state, bool show_compact_metrics,
                                  px_color_t accent) {
    if (!canvas || frame.width < 160 || frame.height < 44)
        return false;
    int next = s_masthead_scene_valid ? 1 - s_masthead_scene_index : 0;
    native_ui_scene_t *scene = &s_masthead_scenes[next];
    const native_ui_scene_t *previous =
        s_masthead_scene_valid ? &s_masthead_scenes[s_masthead_scene_index] : NULL;
    const char *title = "DSCO";
    char signature_title[256];
    const char *signature = getenv("DSCO_KITTY_SIGNATURE");
    if (signature && signature[0]) {
        snprintf(signature_title, sizeof(signature_title), "DSCO  /  %s", signature);
        title = signature_title;
    }
    native_masthead_model_t masthead = {
        .title = title,
        .model = model,
        .slot = g_session.slot_name,
        .state_label = session_state_name(state),
        .state = masthead_agent_state(state),
        .turn = g_session.turn,
        .input_tokens = g_session.input_tokens,
        .output_tokens = g_session.output_tokens,
        .tools_used = g_session.tools_used,
        .queue_depth = g_session.queue_depth,
        .queue_capacity = g_session.queue_capacity,
        .context_percent = g_session.context_percent,
        .cost_usd = g_session.cost_usd,
        .unpriced_responses = g_session.unpriced_responses,
        .show_compact_metrics = show_compact_metrics,
    };
    if (!native_masthead_build(scene, frame.width, frame.height, &masthead))
        return false;

    scene_pixel_host_t host;
    px_backend_t backend;
    scene_pixel_backend_init(&backend, &host, canvas, frame.x, frame.y, state, accent);
    /* Preserve the established neutral masthead fill while retaining raised
     * surface semantics (shadow/elevation) for other backends. */
    backend.palette.colors[NATIVE_UI_COLOR_SURFACE_RAISED] = scene_backend_color(C_PANEL);
    native_ui_render(scene, previous, px_backend_native_vtable(), &backend);
    s_masthead_scene_index = next;
    s_masthead_scene_valid = true;
    return true;
}

static bool draw_session_composer(px_canvas_t *canvas, native_ui_rect_t frame,
                                  pixel_tui_state_t state, px_color_t accent,
                                  double accent_energy, int origin_x, int origin_y) {
    if (!canvas || frame.width < 160 || frame.height < 56)
        return false;
    int next = s_composer_scene_valid ? 1 - s_composer_scene_index : 0;
    native_ui_scene_t *scene = &s_composer_scenes[next];
    const native_ui_scene_t *previous =
        s_composer_scene_valid ? &s_composer_scenes[s_composer_scene_index] : NULL;
    char clock_label[16] = {0};
    if (g_session.show_clock) {
        time_t wall = time(NULL);
        struct tm tm_now;
        if (localtime_r(&wall, &tm_now))
            snprintf(clock_label, sizeof(clock_label), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    }
    if (accent_energy < 0.0)
        accent_energy = 0.0;
    if (accent_energy > 1.0)
        accent_energy = 1.0;
    native_trace_ui_view_t trace_view;
    native_trace_ui_snapshot(&trace_view);
    native_composer_model_t composer = {
        .text = g_session.input,
        .cursor = g_session.input_cursor,
        .active = g_session.input_active,
        .agent_state = masthead_agent_state(state),
        .columns = session_input_columns(frame.width - 58),
        .max_rows = NATIVE_UI_COMPOSER_MAX_ROWS,
        .queue_depth = g_session.queue_depth,
        .queue_capacity = g_session.queue_capacity,
        .clock = clock_label,
        .compact = canvas->width < 620,
        .accent_opacity = (uint8_t)(accent_energy * 255.0 + 0.5),
        .diagnostics_label=trace_view.label,
        .diagnostics_detail=trace_view.detail,
        .diagnostics_enabled=trace_view.enabled,
    };
    if (!native_composer_build(scene, frame.width, frame.height, &composer))
        return false;
    s_trace_ui_view=trace_view;s_trace_ui_rect=(native_ui_rect_t){0};
    for(int i=0;i<scene->count;i++) {
        const native_ui_node_t *node=&scene->nodes[i];
        if(node->key==NATIVE_COMPOSER_KEY_DIAGNOSTICS && (node->state & NATIVE_UI_STATE_VISIBLE)) {
            s_trace_ui_rect=node->frame;
            s_trace_ui_rect.x+=frame.x+origin_x;s_trace_ui_rect.y+=frame.y+origin_y;
        }
    }

    scene_pixel_host_t host;
    px_backend_t backend;
    scene_pixel_backend_init(&backend, &host, canvas, frame.x, frame.y, state, accent);
    native_ui_render(scene, previous, px_backend_native_vtable(), &backend);
    s_composer_scene_index = next;
    s_composer_scene_valid = true;
    return true;
}

static px_canvas_t *render_scene_frame(const char *scene_json, int width, int requested_height) {
    if (!scene_json || strnlen(scene_json, PIXEL_TUI_SCENE_JSON_MAX + 1U) >
                           PIXEL_TUI_SCENE_JSON_MAX || width < 160 ||
        width > PIXEL_TUI_SCENE_DIMENSION_MAX || requested_height < 0 ||
        requested_height > PIXEL_TUI_SCENE_DIMENSION_MAX)
        return NULL;
    if (scene_json_has_nul(scene_json))
        return NULL;
    native_ui_scene_t *scene = malloc(sizeof(*scene));
    if (!scene)
        return NULL;
    int probe_height = requested_height > 0 ? requested_height : 640;
    if (native_ui_scene_from_json(scene, scene_json, width, probe_height) < 0) {
        free(scene);
        return NULL;
    }
    int height = requested_height;
    if (height <= 0) {
        /* Fit the surface to the laid-out content. Grown containers fill the
         * probe viewport, so only leaves measure real content extent; their
         * ancestors' bottom padding rides on top. */
        int bottom = 0;
        for (int i = 0; i < scene->count; i++) {
            const native_ui_node_t *node = &scene->nodes[i];
            if (!(node->state & NATIVE_UI_STATE_VISIBLE) || node->first_child >= 0)
                continue;
            int edge = node->frame.y + node->frame.height;
            for (int at = node->parent; at >= 0; at = scene->nodes[at].parent)
                edge += scene->nodes[at].style.padding.bottom;
            if (edge > bottom)
                bottom = edge;
        }
        height = bottom + 12;
        if (height < 160)
            height = 160;
        if (height > 900)
            height = 900;
        if (height != probe_height &&
            native_ui_scene_from_json(scene, scene_json, width, height) < 0) {
            free(scene);
            return NULL;
        }
    }
    px_canvas_t *c = canvas_acquire(width, height);
    if (!c) {
        free(scene);
        return NULL;
    }
    canvas_background(c, 0x53434e45U);
    scene_pixel_host_t host;
    px_backend_t backend;
    scene_pixel_backend_init(&backend, &host, c, 0, 0, PIXEL_TUI_IDLE, C_CYAN);
    native_ui_render(scene, NULL, px_backend_native_vtable(), &backend);
    free(scene);
    return c;
}

bool pixel_tui_write_scene_ppm(const char *path, const char *scene_json, int width, int height) {
    if (!path || !*path)
        return false;
    px_canvas_t *c = render_scene_frame(scene_json, width > 0 ? width : 900, height);
    if (!c)
        return false;
    bool ok = canvas_write_ppm(path, c);
    free_canvas(c);
    return ok;
}

int pixel_tui_render_scene_json(FILE *out, const char *scene_json) {
    if (!out || !scene_json || strnlen(scene_json, PIXEL_TUI_SCENE_JSON_MAX + 1U) >
                                      PIXEL_TUI_SCENE_JSON_MAX)
        return 0;
    session_lock();
    bool in_session = g_session.active;
    if (in_session) {
        out = session_output(out);
        /* Do not write graphics into an interactive tool's borrowed terminal. */
        if (g_session.terminal_suspended) {
            session_unlock();
            return 0;
        }
        char *copy = strdup(scene_json);
        if (!copy) {
            session_unlock();
            return 0;
        }
        char *previous_json = g_session.scene_json;
        bool previous_dirty = g_session.scene_dirty;
        g_session.scene_json = copy;
        g_session.scene_dirty = true;
        if (!session_refresh_scene(out)) {
            g_session.scene_json = previous_json;
            g_session.scene_dirty = previous_dirty;
            free(copy);
            session_place_scene(out, false);
            (void)fflush(out);
            session_unlock();
            return 0;
        }
        free(previous_json);
        session_clear_overlay(out);
        int occupied = g_session.scene_rows;
        bool flushed = fflush(out) == 0;
        session_unlock();
        return flushed ? occupied : 0;
    } else if (!pixel_tui_available(out)) {
        session_unlock();
        return 0;
    }
    int cols = 80, rows = 24, pixel_width = 0, pixel_height = 0;
    terminal_geometry(out, &cols, &rows, &pixel_width, &pixel_height);
    double dpr = render_device_scale(cols, rows, pixel_width, pixel_height);
    int width = pixel_width > 0 ? pixel_width / dpr - 24 : cols * 10;
    if (width < 480)
        width = 480;
    if (width > 1280)
        width = 1280;
    px_canvas_t *c = render_scene_frame(scene_json, width, 0);
    if (!c) {
        session_unlock();
        return 0;
    }
    int cell_h = (pixel_height > 0 && rows > 0) ? pixel_height / dpr / rows : 20;
    if (cell_h < 8)
        cell_h = 20;
    int occupied_rows = (c->height + cell_h - 1) / cell_h;
    if (occupied_rows < 3)
        occupied_rows = 3;
    int row_cap = rows - 2;
    if (occupied_rows > row_cap)
        occupied_rows = row_cap;
    int placement_cols = cols > 2 ? cols - 2 : cols;

    uint32_t image_id =
        0x4453474eU ^ ((uint32_t)getpid() << 5) ^ (uint32_t)(g_session.generation + 1);
    if (image_id == 0)
        image_id = 1;
    char control[256];
    snprintf(control, sizeof(control), "a=T,t=d,f=24,s=%d,v=%d,i=%u,c=%d,r=%d,C=1,z=2,q=2,o=z",
             c->pixel_width, c->pixel_height, image_id, placement_cols, occupied_rows);
    bool sent = send_kitty_pixels(out, control, c, false, NULL);
    free_canvas(c);
    if (!sent) {
        session_unlock();
        return 0;
    }
    for (int i = 0; i < occupied_rows; i++)
        fputc('\n', out);
    fflush(out);
    int result = ferror(out) ? 0 : occupied_rows;
    session_unlock();
    return result;
}

bool pixel_tui_clear_scene(FILE *out) {
    if (!out)
        return false;
    session_lock();
    bool had_scene = g_session.active && g_session.scene_json != NULL;
    if (had_scene) {
        out = session_output(out);
        /* A suspended scene has no placement, but its image data is still
         * owned. Defer terminal cleanup until resume/shutdown if necessary. */
        if (g_session.terminal_suspended) {
            free(g_session.scene_json);
            g_session.scene_json = NULL;
            g_session.scene_dirty = false;
        } else {
            session_release_scene(out);
            (void)fflush(out);
        }
    }
    session_unlock();
    return had_scene;
}

bool tool_ui_render(const char *input_json, char *result, size_t result_len) {
    if (!result || result_len == 0)
        return false;
    result[0] = '\0';
    if (!input_json || !*input_json ||
        strnlen(input_json, PIXEL_TUI_SCENE_JSON_MAX + 65537U) >
            PIXEL_TUI_SCENE_JSON_MAX + 65536U || scene_json_has_nul(input_json)) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"expected bounded JSON without NUL strings\"}");
        return false;
    }
    yyjson_doc *doc = yyjson_read(input_json, strlen(input_json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"expected a JSON object\"}");
        return false;
    }
    unsigned actions = 0;
    size_t idx, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, idx, max, key, value) {
        (void)value;
        if (yyjson_equals_str(key, "action")) actions++;
    }
    yyjson_val *av = yyjson_obj_get(root, "action");
    if (actions > 1 || (actions && !yyjson_is_str(av))) {
        yyjson_doc_free(doc);
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"action must be one string field\"}");
        return false;
    }
    yyjson_val *wv = yyjson_obj_get(root, "width");
    if (wv && (!yyjson_is_int(wv) || yyjson_get_sint(wv) < 160 ||
               yyjson_get_sint(wv) > PIXEL_TUI_SCENE_DIMENSION_MAX)) {
        yyjson_doc_free(doc);
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"width must be an integer from 160 to 4096\"}");
        return false;
    }
    char *action = actions ? strdup(yyjson_get_str(av)) : NULL;
    yyjson_doc_free(doc);
    if (actions && !action) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"out of memory\"}");
        return false;
    }
    if (action && !strcmp(action, "close")) {
        if (result_len < sizeof("{\"ok\":true,\"closed\":false}")) {
            free(action);
            return false;
        }
        bool closed = pixel_tui_clear_scene(stdout);
        snprintf(result, result_len, "{\"ok\":true,\"closed\":%s}", closed ? "true" : "false");
        free(action);
        return true;
    }
    if (action && strcmp(action, "render")) {
        free(action);
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"action must be render or close\"}");
        return false;
    }
    free(action);
    char *spec = json_get_raw(input_json, "spec");
    const char *scene_json = spec;
    if (!scene_json) {
        /* Accept the spec object directly as the tool input. */
        char *element = json_get_str(input_json, "element");
        if (element) {
            scene_json = input_json;
            free(element);
        }
    }
    if (!scene_json) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"missing spec object\"}");
        return false;
    }
    if (strnlen(scene_json, PIXEL_TUI_SCENE_JSON_MAX + 1U) > PIXEL_TUI_SCENE_JSON_MAX) {
        free(spec);
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"scene JSON exceeds 1 MiB\"}");
        return false;
    }
    char *ppm_path = json_get_str(input_json, "ppm_path");
    int width = json_get_int(input_json, "width", 900);
    bool wrote_ppm = false;
    if (ppm_path && *ppm_path)
        wrote_ppm = pixel_tui_write_scene_ppm(ppm_path, scene_json, width, 0);
    int rows = pixel_tui_render_scene_json(stdout, scene_json);
    bool ok = rows > 0 || wrote_ppm;
    if (ok) {
        jbuf_t response;
        jbuf_init(&response, 256);
        jbuf_appendf(&response, "{\"ok\":true,\"rows\":%d,\"rendered\":%s,\"ppm\":",
                     rows, rows > 0 ? "true" : "false");
        if (wrote_ppm)
            jbuf_append_json_str(&response, ppm_path);
        else
            jbuf_append(&response, "null");
        jbuf_append(&response, "}");
        if (response.len >= result_len) {
            static const char too_small[] = "{\"ok\":false,\"error\":\"result_too_small\"}";
            result[0] = '\0';
            if (sizeof(too_small) <= result_len)
                memcpy(result, too_small, sizeof(too_small));
            ok = false;
        } else {
            memcpy(result, response.data, response.len + 1U);
        }
        jbuf_free(&response);
    } else
        snprintf(result, result_len,
                 "{\"ok\":false,\"error\":\"scene invalid or no kitty surface;"
                 " pass ppm_path for a headless artifact\"}");
    free(spec);
    free(ppm_path);
    return ok;
}
