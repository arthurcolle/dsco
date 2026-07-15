#define _POSIX_C_SOURCE 200809L

#include "pixel_tui.h"
#include "font_compat.h"
#include "json_util.h"
#include "kitty_graphics.h"
#include "native_ui.h"
#include "pixel_fx.h"
#include "plan.h"
#include "plan_dag.h"
#include "rich_text.h"
#include "ui_motion.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

typedef struct { uint8_t r, g, b; } px_color_t;
typedef struct { int width, height; px_color_t *pixels; } px_canvas_t;

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

/* DSCO quiet-console palette. Neutral surfaces do the structural work; color
 * is reserved for current state, completion, warning, and failure. */
static const px_color_t C_BG_TOP = {12, 14, 16};
static const px_color_t C_BG_BOTTOM = {8, 10, 12};
static const px_color_t C_PANEL = {17, 20, 23};
static const px_color_t C_PANEL_ALT = {21, 24, 28};
static const px_color_t C_TEXT = {205, 211, 216};
static const px_color_t C_DIM = {100, 108, 116};
static const px_color_t C_CYAN = {105, 139, 153};
static const px_color_t C_VIOLET = {111, 108, 122};
static const px_color_t C_GREEN = {108, 142, 122};
static const px_color_t C_AMBER = {174, 142, 94};
static const px_color_t C_RED = {166, 96, 99};

typedef struct {
    char role[20];
    char detail[160];
    char text[3072];
    int turn;
    uint64_t sequence;
    bool streaming;
} pixel_message_t;

#define PIXEL_MESSAGE_CAP 512
#define PIXEL_COMMAND_CAP 160
#define PIXEL_TOOL_VIS_CAP 8
#define PIXEL_SWARM_VIS_CAP 12
#define PIXEL_TURN_VIS_CAP 10

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
    bool active;
    uint32_t image_ids[4];
    uint32_t generation;
    int image_widths[4];
    int image_heights[4];
    int cols;
    int rows;
    int width;
    int height;
    pixel_tui_state_t state;
    char model[128];
    pixel_message_t messages[PIXEL_MESSAGE_CAP];
    int message_start;
    int message_count;
    int current_message;
    char input[4096];
    size_t input_cursor;
    bool input_active;
    int transcript_scroll;
    double last_paint_s;
    double started_s;
    double state_started_s;
    double turn_started_s;
    double cost_usd;
    double context_percent;
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
    int prev_frame_width;
    int prev_frame_height;
    uint32_t prev_frame_image;
    uint32_t patch_streak;
    bool patch_enabled;
} pixel_session_t;

static pixel_session_t g_session;
static pthread_mutex_t g_session_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_animation_cond = PTHREAD_COND_INITIALIZER;

static void *session_capture_thread_main(void *arg);
static void *session_animation_thread_main(void *arg);
static double monotonic_s(void);
static px_color_t session_animated_accent(pixel_tui_state_t state);
static void draw_meter(px_canvas_t *c, int x, int y, int w, double percent,
                       px_color_t color);

static void session_lock(void) {
    (void)pthread_mutex_lock(&g_session_mutex);
}

static void session_unlock(void) {
    (void)pthread_mutex_unlock(&g_session_mutex);
}

static FILE *session_output(FILE *fallback) {
    return g_session.active && g_session.tty_out ? g_session.tty_out : fallback;
}

static bool session_suppress_stdio(void) {
    if (!g_session.active || g_session.stdio_suppressed) return true;
    fflush(stdout);
    fflush(stderr);
    if (g_session.devnull_fd < 0)
        g_session.devnull_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (g_session.devnull_fd < 0) return false;
    int target = (!g_session.capture_muted && g_session.capture_write_fd >= 0)
                     ? g_session.capture_write_fd
                     : g_session.devnull_fd;
    if (dup2(target, STDOUT_FILENO) < 0 || dup2(target, STDERR_FILENO) < 0)
        return false;
    g_session.stdio_suppressed = true;
    return true;
}

static void session_capture_set_muted(bool muted) {
    if (!g_session.active || g_session.capture_muted == muted) return;
    fflush(stdout);
    fflush(stderr);
    g_session.capture_muted = muted;
    if (!g_session.stdio_suppressed) return;
    int target = (!muted && g_session.capture_write_fd >= 0)
                     ? g_session.capture_write_fd
                     : g_session.devnull_fd;
    if (target >= 0) {
        (void)dup2(target, STDOUT_FILENO);
        (void)dup2(target, STDERR_FILENO);
    }
}

static bool session_capture_start(void) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) return false;
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    g_session.capture_read_fd = fds[0];
    g_session.capture_write_fd = fds[1];
    g_session.capture_stop = false;
    if (pthread_create(&g_session.capture_thread, NULL,
                       session_capture_thread_main, NULL) != 0) {
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
    if (!g_session.active || !g_session.stdio_suppressed) return;
    fflush(stdout);
    fflush(stderr);
    if (g_session.saved_stdout_fd >= 0)
        (void)dup2(g_session.saved_stdout_fd, STDOUT_FILENO);
    if (g_session.saved_stderr_fd >= 0)
        (void)dup2(g_session.saved_stderr_fd, STDERR_FILENO);
    g_session.stdio_suppressed = false;
}

static double clamp01(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

static px_color_t color_mix(px_color_t a, px_color_t b, double t) {
    t = clamp01(t);
    return (px_color_t){
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t)
    };
}

static void put_pixel(px_canvas_t *c, int x, int y, px_color_t color, double alpha) {
    if (!c || !c->pixels || (unsigned)x >= (unsigned)c->width ||
        (unsigned)y >= (unsigned)c->height)
        return;
    px_color_t *p = &c->pixels[(size_t)y * (size_t)c->width + (size_t)x];
    *p = color_mix(*p, color, alpha);
}

static void fill_rect(px_canvas_t *c, int x, int y, int w, int h,
                      px_color_t color, double alpha) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->width ? c->width : x + w;
    int y1 = y + h > c->height ? c->height : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            put_pixel(c, xx, yy, color, alpha);
}

/* Bridge the local canvas/color types onto the shared effects core. The
 * layouts are identical (packed 24-bit RGB); pixel_fx adds AA rounded
 * geometry, shadows, gradients, and blur on the same memory. */
static pixel_fx_surface_t fx_surface(px_canvas_t *c) {
    pixel_fx_surface_t s;
    pixel_fx_surface_init(&s, (uint8_t *)c->pixels, c->width, c->height);
    return s;
}

static pixel_fx_rgb_t fx_color(px_color_t color) {
    return (pixel_fx_rgb_t){color.r, color.g, color.b};
}

static void fill_rounded(px_canvas_t *c, int x, int y, int w, int h,
                         int radius, px_color_t color, double alpha) {
    pixel_fx_surface_t s = fx_surface(c);
    pixel_fx_fill_rounded(&s, x, y, w, h, radius, fx_color(color), alpha);
}

static void stroke_rounded(px_canvas_t *c, int x, int y, int w, int h,
                           int radius, px_color_t color, double alpha) {
    pixel_fx_surface_t s = fx_surface(c);
    pixel_fx_stroke_rounded(&s, x, y, w, h, radius, 1, fx_color(color), alpha);
}

/* Elevated panel: soft shadow under an AA rounded surface with a hairline
 * border. This is the standard ground for every session card. */
static void draw_panel(px_canvas_t *c, int x, int y, int w, int h, int radius,
                       px_color_t fill, double fill_alpha) {
    pixel_fx_surface_t s = fx_surface(c);
    pixel_fx_shadow(&s, x, y, w, h, radius, 14, 3,
                    (pixel_fx_rgb_t){0, 0, 0}, 0.42);
    pixel_fx_fill_rounded(&s, x, y, w, h, radius, fx_color(fill), fill_alpha);
    pixel_fx_stroke_rounded(&s, x, y, w, h, radius, 1,
                            fx_color(C_DIM), 0.26);
}

/* ── timeline identities ─────────────────────────────────────────────────
 * Stable (key, prop) pairs for the retained motion timeline. Message and
 * tool tracks derive keys from their sequence numbers. */
enum {
    MOTION_PROP_VALUE = 0,     /* eased scalar (meters) */
    MOTION_PROP_ENTRANCE = 1,  /* 0 → 1 arrival of a new object */
    MOTION_PROP_FLASH = 2,     /* 1 → 0 attention decay */
};
#define MOTION_KEY_CONTEXT UINT64_C(0x110)
#define MOTION_KEY_QUEUE UINT64_C(0x111)
#define MOTION_KEY_MESSAGE(sequence) (UINT64_C(0x4d000000) + (sequence))
#define MOTION_KEY_TOOL(sequence) (UINT64_C(0x54000000) + (sequence))

static double session_motion_value(uint64_t key, uint16_t prop,
                                   double fallback) {
    return ui_motion_value(&g_session.motion, key, prop, monotonic_s(),
                           fallback);
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

static double motion_pulse(double period_s, double offset_s) {
    double phase = motion_phase(period_s, offset_s);
    return 0.5 - 0.5 * cos(phase * 6.28318530717958647692);
}

static void draw_motion_sweep(px_canvas_t *c, int x, int y, int w, int h,
                              px_color_t color, double period_s, double alpha) {
    if (!c || w < 12 || h < 1 || !g_session.animation_enabled)
        return;
    int radius = w / 7;
    if (radius < 12) radius = 12;
    if (radius > 90) radius = 90;
    double phase = motion_phase(period_s, 0.0);
    int center = x - radius + (int)((double)(w + radius * 2) * phase);
    for (int xx = center - radius; xx <= center + radius; xx += 2) {
        double distance = fabs((double)(xx - center)) / (double)radius;
        double strength = 1.0 - clamp01(distance);
        fill_rect(c, xx, y, 2, h, color, alpha * smoothstep(strength));
    }
}

static void draw_line(px_canvas_t *c, int x0, int y0, int x1, int y1,
                      px_color_t color, double alpha) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_pixel(c, x0, y0, color, alpha);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fill_circle(px_canvas_t *c, int cx, int cy, int radius,
                        px_color_t color, double alpha) {
    if (!c || radius < 1) return;
    int rr = radius * radius;
    for (int y = -radius; y <= radius; y++) {
        int span = (int)sqrt((double)(rr - y * y));
        fill_rect(c, cx - span, cy + y, span * 2 + 1, 1, color, alpha);
    }
}

static void draw_circle_ring(px_canvas_t *c, int cx, int cy, int radius, int thickness,
                             px_color_t color, double alpha) {
    if (!c || radius < 2 || thickness < 1) return;
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
static void draw_ring_arc(px_canvas_t *c, int cx, int cy, int radius,
                          double a0, double a1, px_color_t color, double alpha) {
    if (!c || radius < 2 || a1 <= a0) return;
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
static void draw_dsco_soul(px_canvas_t *c, int cx, int cy, int radius,
                           pixel_tui_state_t state) {
    if (!c || radius < 10) return;
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
            if (!tool->used) continue;
            double a0 = -1.5707963267948966 + slot_span * i + gap * 0.5;
            double a1 = a0 + slot_span - gap;
            px_color_t arc = C_AMBER;
            double arc_alpha = 0.0;
            if (tool->status == PIXEL_OP_RUNNING) {
                arc_alpha = 0.55 + motion_pulse(1.1, i * 0.13) * 0.35;
            } else {
                double age = now - (tool->started_s + tool->elapsed_ms / 1000.0);
                arc = tool->status == PIXEL_OP_DONE ? C_GREEN : C_RED;
                arc_alpha = (tool->status == PIXEL_OP_ERROR ? 0.85 : 0.70) -
                            (age > 0.0 ? age * 0.12 : 0.0);
            }
            if (arc_alpha <= 0.04) continue;
            draw_ring_arc(c, cx, cy, radius, a0, a1, arc, arc_alpha);
            segmented = true;
        }
    }
    if (!segmented)
        draw_circle_ring(c, cx, cy, radius, 1, C_DIM, 0.36);
    else
        draw_circle_ring(c, cx, cy, radius, 1, C_DIM, 0.14);

    /* Wings: two quiet vanes carrying the core into the upper field. */
    draw_line(c, cx - 3, cy - 2, cx - radius + 3, cy - radius / 2,
              C_CYAN, 0.54);
    draw_line(c, cx - 2, cy - 5, cx - radius / 2, cy - radius + 3,
              C_CYAN, 0.32);
    draw_line(c, cx + 3, cy - 2, cx + radius - 3, cy - radius / 2,
              C_CYAN, 0.54);
    draw_line(c, cx + 2, cy - 5, cx + radius / 2, cy - radius + 3,
              C_CYAN, 0.32);

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
    for (int y = 0; y < c->height; y++) {
        double v = (double)y / (double)(c->height > 1 ? c->height - 1 : 1);
        px_color_t row = color_mix(C_BG_TOP, C_BG_BOTTOM, v);
        for (int x = 0; x < c->width; x++)
            c->pixels[(size_t)y * (size_t)c->width + (size_t)x] = row;
    }
}

#define GLYPH(a,b,c,d,e,f,g) (((uint64_t)(a)<<30)|((uint64_t)(b)<<25)|\
                               ((uint64_t)(c)<<20)|((uint64_t)(d)<<15)|\
                               ((uint64_t)(e)<<10)|((uint64_t)(f)<<5)|(g))
static uint64_t glyph_bits(char ch) {
    switch ((unsigned char)toupper((unsigned char)ch)) {
    case 'A': return GLYPH(14,17,17,31,17,17,17); case 'B': return GLYPH(30,17,17,30,17,17,30);
    case 'C': return GLYPH(14,17,16,16,16,17,14); case 'D': return GLYPH(30,17,17,17,17,17,30);
    case 'E': return GLYPH(31,16,16,30,16,16,31); case 'F': return GLYPH(31,16,16,30,16,16,16);
    case 'G': return GLYPH(14,17,16,23,17,17,15); case 'H': return GLYPH(17,17,17,31,17,17,17);
    case 'I': return GLYPH(14,4,4,4,4,4,14); case 'J': return GLYPH(1,1,1,1,17,17,14);
    case 'K': return GLYPH(17,18,20,24,20,18,17); case 'L': return GLYPH(16,16,16,16,16,16,31);
    case 'M': return GLYPH(17,27,21,21,17,17,17); case 'N': return GLYPH(17,25,21,19,17,17,17);
    case 'O': return GLYPH(14,17,17,17,17,17,14); case 'P': return GLYPH(30,17,17,30,16,16,16);
    case 'Q': return GLYPH(14,17,17,17,21,18,13); case 'R': return GLYPH(30,17,17,30,20,18,17);
    case 'S': return GLYPH(15,16,16,14,1,1,30); case 'T': return GLYPH(31,4,4,4,4,4,4);
    case 'U': return GLYPH(17,17,17,17,17,17,14); case 'V': return GLYPH(17,17,17,17,17,10,4);
    case 'W': return GLYPH(17,17,17,21,21,21,10); case 'X': return GLYPH(17,17,10,4,10,17,17);
    case 'Y': return GLYPH(17,17,10,4,4,4,4); case 'Z': return GLYPH(31,1,2,4,8,16,31);
    case '0': return GLYPH(14,17,19,21,25,17,14); case '1': return GLYPH(4,12,4,4,4,4,14);
    case '2': return GLYPH(14,17,1,2,4,8,31); case '3': return GLYPH(30,1,1,14,1,1,30);
    case '4': return GLYPH(2,6,10,18,31,2,2); case '5': return GLYPH(31,16,16,30,1,1,30);
    case '6': return GLYPH(14,16,16,30,17,17,14); case '7': return GLYPH(31,1,2,4,8,8,8);
    case '8': return GLYPH(14,17,17,14,17,17,14); case '9': return GLYPH(14,17,17,15,1,1,14);
    case ':': return GLYPH(0,4,4,0,4,4,0); case '.': return GLYPH(0,0,0,0,0,4,4);
    case '/': return GLYPH(1,2,2,4,8,8,16); case '-': return GLYPH(0,0,0,31,0,0,0);
    case '+': return GLYPH(0,4,4,31,4,4,0); case '#': return GLYPH(10,31,10,10,31,10,0);
    case '[': return GLYPH(14,8,8,8,8,8,14); case ']': return GLYPH(14,2,2,2,2,2,14);
    case '(': return GLYPH(2,4,8,8,8,4,2); case ')': return GLYPH(8,4,2,2,2,4,8);
    case '_': return GLYPH(0,0,0,0,0,0,31); case '=': return GLYPH(0,31,0,31,0,0,0);
    case '?': return GLYPH(14,17,1,2,4,0,4); case '!': return GLYPH(4,4,4,4,4,0,4);
    case ',': return GLYPH(0,0,0,0,0,4,8); case ';': return GLYPH(0,4,4,0,4,4,8);
    case '|': return GLYPH(4,4,4,4,4,4,4); case '<': return GLYPH(2,4,8,16,8,4,2);
    case '>': return GLYPH(8,4,2,1,2,4,8); case '%': return GLYPH(17,2,4,8,16,17,0);
    case '$': return GLYPH(4,15,20,14,5,30,4); case '@': return GLYPH(14,17,23,21,23,16,14);
    case '*': return GLYPH(0,21,14,31,14,21,0); case '\\': return GLYPH(16,8,8,4,2,2,1);
    default: return 0;
    }
}

static float text_point_size(int scale) {
    return scale >= 3 ? 22.0f : (scale == 2 ? 15.0f : 10.5f);
}

static size_t utf8_char_bytes(const char *s) {
    if (!s || !*s) return 0;
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0 && ((unsigned char)s[1] & 0xc0) == 0x80) return 2;
    if ((c & 0xf0) == 0xe0 && ((unsigned char)s[1] & 0xc0) == 0x80 &&
        ((unsigned char)s[2] & 0xc0) == 0x80) return 3;
    if ((c & 0xf8) == 0xf0 && ((unsigned char)s[1] & 0xc0) == 0x80 &&
        ((unsigned char)s[2] & 0xc0) == 0x80 &&
        ((unsigned char)s[3] & 0xc0) == 0x80) return 4;
    return 1;
}

static int draw_text(px_canvas_t *c, int x, int y, int scale, const char *text,
                     px_color_t color, double alpha, int max_width) {
    int origin = x;
    if (!text || scale < 1) return 0;
    int native_advance = font_compat_draw_rgb(
        (uint8_t *)c->pixels, c->width, c->height,
        c->width * (int)sizeof(px_color_t), x, y,
        max_width > 0 ? max_width : c->width - x, text,
        text_point_size(scale), false,
        color.r, color.g, color.b, (float)clamp01(alpha));
    if (native_advance >= 0)
        return native_advance;
    for (; *text; text++) {
        if (max_width > 0 && x - origin + 5 * scale > max_width) break;
        uint64_t bits = glyph_bits(*text);
        for (int gy = 0; gy < 7; gy++) {
            for (int gx = 0; gx < 5; gx++) {
                if ((bits >> ((6 - gy) * 5 + (4 - gx))) & 1U)
                    fill_rect(c, x + gx*scale, y + gy*scale, scale, scale, color, alpha);
            }
        }
        x += 6 * scale;
    }
    return x - origin;
}

static void draw_text_ellipsis(px_canvas_t *c, int x, int y, int scale, const char *text,
                               px_color_t color, double alpha, int max_width) {
    if (!text || max_width < 6*scale) return;
    if (font_compat_available()) {
        /* CoreText clips at a glyph boundary and preserves shaped Unicode.
         * Byte-count truncation would split UTF-8 and destroy fallback runs. */
        draw_text(c, x, y, scale, text, color, alpha, max_width);
        return;
    }
    int max_chars = max_width / (6*scale);
    size_t len = strlen(text);
    if ((int)len <= max_chars) {
        draw_text(c, x, y, scale, text, color, alpha, max_width);
        return;
    }
    char buf[256];
    int keep = max_chars - 3;
    if (keep < 1) keep = 1;
    if (keep > (int)sizeof(buf) - 4) keep = (int)sizeof(buf) - 4;
    memcpy(buf, text, (size_t)keep);
    memcpy(buf + keep, "...", 4);
    draw_text(c, x, y, scale, buf, color, alpha, max_width);
}

static px_color_t status_color(plan_status_t status) {
    switch (status) {
    case PLAN_DONE: return C_GREEN;
    case PLAN_IN_PROGRESS: return C_CYAN;
    case PLAN_BLOCKED: return C_AMBER;
    case PLAN_FAILED: case PLAN_CANCELLED: return C_RED;
    case PLAN_SKIPPED: return C_DIM;
    default: return C_VIOLET;
    }
}

static void collect_step(plan_layout_t *layout, int step_id, int depth, int parent) {
    if (!layout || depth > 12) return;
    step_t *step = step_get(step_id);
    if (!step) return;
    if (layout->count >= (int)(sizeof(layout->nodes)/sizeof(layout->nodes[0]))) {
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
    if (atoms > 3) atoms = 3;
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
    draw_text_ellipsis(c, x + 62, y + 12, 2, step->title ? step->title : "UNTITLED",
                       C_TEXT, 0.96, w - 140);

    const char *type = step_type_name(step->type);
    const char *status = plan_status_name(step->status);
    int status_w = (int)strlen(status) * 6 + 12;
    draw_chip(c, x + w - status_w - 12, y + 10, status, accent);
    draw_chip(c, x + 62, y + 37, type, C_DIM);

    int atom_y = y + 58;
    int shown = step->atom_count < 3 ? step->atom_count : 3;
    for (int i = 0; i < shown; i++) {
        atom_t *atom = atom_get(step->atom_ids[i]);
        if (!atom) continue;
        px_color_t atom_color = status_color(atom->status);
        fill_rect(c, x + 62, atom_y + i*18, w - 78, 14, C_PANEL_ALT, 0.82);
        fill_rect(c, x + 67, atom_y + 4 + i*18, 5, 5, atom_color, 0.9);
        draw_text_ellipsis(c, x + 80, atom_y + 3 + i*18, 1,
                           atom->title ? atom->title : atom_type_name(atom->type),
                           C_DIM, 0.90, w - 102);
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
    size_t cap;   /* allocated bytes in canvas.pixels */
    bool in_use;
} canvas_slot_t;
static canvas_slot_t g_canvas_pool[CANVAS_POOL_SLOTS];
static pthread_mutex_t g_canvas_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

static px_canvas_t *canvas_acquire(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    size_t need = (size_t)width * (size_t)height * sizeof(px_color_t);
    (void)pthread_mutex_lock(&g_canvas_pool_mutex);
    for (int i = 0; i < CANVAS_POOL_SLOTS; i++) {
        canvas_slot_t *s = &g_canvas_pool[i];
        if (s->in_use) continue;
        if (s->cap < need) {
            px_color_t *grown = realloc(s->canvas.pixels, need);
            if (!grown) continue; /* try next slot or fall through */
            s->canvas.pixels = grown;
            s->cap = need;
        }
        s->in_use = true;
        s->canvas.width = width;
        s->canvas.height = height;
        (void)pthread_mutex_unlock(&g_canvas_pool_mutex);
        return &s->canvas;
    }
    (void)pthread_mutex_unlock(&g_canvas_pool_mutex);
    /* Pool exhausted: legacy per-frame heap path (released in free_canvas). */
    px_canvas_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->width = width;
    c->height = height;
    c->pixels = malloc(need);
    if (!c->pixels) { free(c); return NULL; }
    return c;
}

static px_canvas_t *render_plan_frame(int plan_id, int requested_width, plan_layout_t *out_layout) {
    plan_t *plan = plan_get(plan_id);
    if (!plan) return NULL;
    plan_layout_t layout;
    build_layout(plan, &layout);
    int width = requested_width;
    if (width < 640) width = 640;
    if (width > 1280) width = 1280;
    int y = 134;
    for (int i = 0; i < layout.count; i++) {
        layout.nodes[i].height = node_height(layout.nodes[i].step);
        layout.nodes[i].y = y;
        y += layout.nodes[i].height + 12;
    }
    int height = y + (layout.hidden ? 42 : 24);
    if (height < 300) height = 300;
    px_canvas_t *c = canvas_acquire(width, height);
    if (!c) return NULL;
    canvas_background(c, (uint32_t)plan_id * 2654435761U);

    /* Header: goal, mode, state, and a live topology rail. */
    fill_rect(c, 24, 20, width - 48, 94, C_PANEL, 0.94);
    fill_rect(c, 24, 20, width - 48, 1, C_DIM, 0.34);
    fill_rect(c, 24, 20, 2, 94, status_color(plan->status), 0.68);
    char plan_label[48];
    snprintf(plan_label, sizeof(plan_label), "PLAN #%d // %s", plan->id, plan_mode_name(plan->mode));
    draw_text(c, 44, 36, 1, plan_label, C_CYAN, 0.95, width - 280);
    draw_text_ellipsis(c, 44, 56, 3, plan->title ? plan->title : "EXECUTION PLAN",
                       C_TEXT, 0.98, width - 300);
    draw_text_ellipsis(c, 44, 88, 1, plan->goal ? plan->goal : "NO GOAL SET",
                       C_DIM, 0.96, width - 280);
    draw_chip(c, width - 180, 36, plan_status_name(plan->status), status_color(plan->status));
    draw_text(c, width - 180, 88, 1, "PLAN TREE", C_DIM, 0.82, 150);

    /* Connector topology goes behind cards. */
    for (int i = 0; i < layout.count; i++) {
        plan_node_t *node = &layout.nodes[i];
        if (node->parent < 0) continue;
        plan_node_t *parent = &layout.nodes[node->parent];
        int px = 48 + parent->depth*48 + 22;
        int py = parent->y + parent->height;
        int nx = 48 + node->depth*48 - 12;
        int ny = node->y + 18;
        px_color_t wire = status_color(node->step->status);
        draw_line(c, px, py, px, ny, wire, 0.28);
        draw_line(c, px, ny, nx, ny, wire, 0.48);
        fill_rect(c, nx - 1, ny - 1, 3, 3, wire, 0.62);
    }
    for (int i = 0; i < layout.count; i++) draw_node(c, &layout.nodes[i], i);
    if (layout.hidden) {
        char more[48];
        snprintf(more, sizeof(more), "+%d HIDDEN NODES", layout.hidden);
        draw_text(c, 48, height - 28, 1, more, C_AMBER, 0.95, width - 96);
    }
    draw_text(c, width - 166, height - 18, 1, "DSCO / PLAN", C_DIM, 0.72, 146);
    if (out_layout) *out_layout = layout;
    return c;
}

static void collect_plan_actions(plan_action_layout_t *layout, int step_id, int depth) {
    if (!layout || depth > 12) return;
    step_t *step = step_get(step_id);
    if (!step) return;
    for (int i = 0; i < step->atom_count; i++) {
        if (layout->count >= (int)(sizeof(layout->nodes) / sizeof(layout->nodes[0]))) {
            layout->hidden++;
            continue;
        }
        atom_t *atom = atom_get(step->atom_ids[i]);
        if (!atom) continue;
        layout->nodes[layout->count++] = (plan_action_node_t){
            .atom = atom, .step = step, .x = 0, .y = 0, .w = 0, .h = 0,
        };
        layout->wire_count += atom->input_from_count;
    }
    for (int i = 0; i < step->child_step_count; i++)
        collect_plan_actions(layout, step->child_step_ids[i], depth + 1);
}

static void build_action_layout(plan_t *plan, plan_action_layout_t *layout) {
    if (!plan || !layout) return;
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
    if (!layout) return false;
    for (int i = 0; i < layout->ready_count; i++)
        if (layout->ready_ids[i] == atom_id) return true;
    return false;
}

static int action_index_for_id(const plan_action_layout_t *layout, int atom_id) {
    if (!layout) return -1;
    int visible = layout->count > 12 ? 12 : layout->count;
    for (int i = 0; i < visible; i++)
        if (layout->nodes[i].atom && layout->nodes[i].atom->id == atom_id) return i;
    return -1;
}

static const char *plan_action_label(const atom_t *atom) {
    if (!atom) return "ACTION";
    if (atom->title && *atom->title) return atom->title;
    if (atom->tool_name && *atom->tool_name) return atom->tool_name;
    return atom_type_name(atom->type);
}

static void draw_plan_action_card(px_canvas_t *c, const plan_action_node_t *node,
                                  const plan_action_layout_t *layout) {
    if (!c || !node || !node->atom) return;
    const atom_t *atom = node->atom;
    px_color_t accent = status_color(atom->status);
    bool ready = action_id_ready(layout, atom->id);
    fill_rect(c, node->x, node->y, node->w, node->h, C_PANEL_ALT,
              ready ? 0.93 : 0.78);
    fill_rect(c, node->x, node->y, 3, node->h, accent, ready ? 0.92 : 0.60);
    fill_rect(c, node->x, node->y, node->w, 1, C_DIM, 0.22);
    fill_rect(c, node->x, node->y + node->h - 1, node->w, 1, C_DIM, 0.13);
    char id[24];
    snprintf(id, sizeof(id), "A-%02d", atom->id);
    draw_text(c, node->x + 10, node->y + 8, 1, id, accent, 0.92, 42);
    if (ready) fill_circle(c, node->x + node->w - 9, node->y + 10, 3, C_GREEN, 0.92);
    draw_text_ellipsis(c, node->x + 10, node->y + 25, 1,
                       plan_action_label(atom), C_TEXT, 0.84, node->w - 20);
    draw_chip(c, node->x + 10, node->y + node->h - 24,
              plan_status_name(atom->status), accent);
}

static int plan_action_lane_index(const plan_action_layout_t *layout, int step_id,
                                  int *step_ids, int *count, int max_count) {
    if (!layout || !step_ids || !count || max_count < 1) return -1;
    for (int i = 0; i < *count; i++)
        if (step_ids[i] == step_id) return i;
    if (*count >= max_count) return -1;
    step_ids[*count] = step_id;
    return (*count)++;
}

static px_canvas_t *render_plan_actions_frame(int plan_id, int requested_width) {
    plan_t *plan = plan_get(plan_id);
    if (!plan) return NULL;
    int width = requested_width;
    if (width < 640) width = 640;
    if (width > 1280) width = 1280;
    int height = 620;
    px_canvas_t *c = canvas_acquire(width, height);
    if (!c) return NULL;
    canvas_background(c, (uint32_t)plan_id * 2246822519U);

    plan_action_layout_t layout;
    build_action_layout(plan, &layout);
    int visible = layout.count > 12 ? 12 : layout.count;
    int margin = 24, gap = 18, top_y = 20;
    fill_rect(c, margin, top_y, width - margin * 2, 94, C_PANEL, 0.94);
    fill_rect(c, margin, top_y, width - margin * 2, 1, C_DIM, 0.34);
    fill_rect(c, margin, top_y, 2, 94, status_color(plan->status), 0.68);
    draw_text(c, margin + 20, top_y + 16, 1, "ACTION PLAN / DEPENDENCY MAP",
              C_CYAN, 0.94, width - 300);
    draw_text_ellipsis(c, margin + 20, top_y + 36, 2,
                       plan->title ? plan->title : "ACTION PLAN",
                       C_TEXT, 0.96, width - 300);
    draw_text_ellipsis(c, margin + 20, top_y + 67, 1,
                       plan->goal ? plan->goal : "NO GOAL SET",
                       C_DIM, 0.86, width - 300);
    draw_chip(c, width - 190, top_y + 18, plan_status_name(plan->status),
              status_color(plan->status));
    char count_label[64];
    snprintf(count_label, sizeof(count_label), "%d ACTIONS  /  %d READY",
             layout.count, layout.ready_count);
    draw_text(c, width - 190, top_y + 69, 1, count_label, C_DIM, 0.74, 170);

    int body_y = 132, bottom_h = 142;
    int body_h = height - body_y - bottom_h - gap - margin;
    int left_w = (width * 64) / 100;
    int right_w = width - margin * 2 - left_w - gap;
    int left_x = margin, right_x = left_x + left_w + gap;
    fill_rect(c, left_x, body_y, left_w, body_h, C_PANEL, 0.90);
    fill_rect(c, right_x, body_y, right_w, body_h, C_PANEL, 0.90);
    draw_text(c, left_x + 16, body_y + 12, 1,
              "ACTION DAG / WIRED INPUTS", C_TEXT, 0.76, left_w - 32);
    draw_text(c, right_x + 16, body_y + 12, 1,
              "READY FRONTIER / GATES", C_TEXT, 0.76, right_w - 32);
    draw_line(c, left_x + 16, body_y + 31, left_x + left_w - 16, body_y + 31,
              C_DIM, 0.20);
    draw_line(c, right_x + 16, body_y + 31, right_x + right_w - 16, body_y + 31,
              C_DIM, 0.20);

    int cols = width >= 900 ? 4 : 3;
    if (visible > 0 && visible < cols) cols = visible;
    if (cols < 1) cols = 1;
    int node_gap = 14;
    int node_w = (left_w - 32 - (cols - 1) * node_gap) / cols;
    if (node_w < 84) node_w = 84;
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
            if (src_i < 0) continue;
            plan_action_node_t *src = &layout.nodes[src_i];
            draw_line(c, src->x + src->w / 2, src->y + src->h / 2,
                      dst->x + dst->w / 2, dst->y + dst->h / 2,
                      status_color(dst->atom->status), 0.54);
            edge_count++;
        }
        if (edge_count == 0 && i > 0) {
            plan_action_node_t *src = &layout.nodes[i - 1];
            draw_line(c, src->x + src->w, src->y + src->h / 2,
                      dst->x, dst->y + dst->h / 2, C_DIM, 0.20);
        }
    }
    for (int i = 0; i < visible; i++)
        draw_plan_action_card(c, &layout.nodes[i], &layout);
    if (layout.hidden || layout.count > visible) {
        char hidden[48];
        snprintf(hidden, sizeof(hidden), "+%d ACTIONS HIDDEN",
                 layout.hidden + layout.count - visible);
        draw_text(c, left_x + 16, body_y + body_h - 16, 1, hidden, C_AMBER, 0.82,
                  left_w - 32);
    }

    int ry = body_y + 48, rw = right_w - 32;
    draw_text(c, right_x + 16, ry, 1, "READY NOW", C_GREEN, 0.86, rw);
    ry += 20;
    int shown_ready = layout.ready_count < 5 ? layout.ready_count : 5;
    for (int i = 0; i < shown_ready; i++) {
        atom_t *atom = atom_get(layout.ready_ids[i]);
        if (!atom) continue;
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
    snprintf(wires, sizeof(wires), "WIRES %d  /  PARALLEL %dX",
             layout.wire_count, layout.ready_count > 3 ? 3 : layout.ready_count);
    draw_text(c, right_x + 16, body_y + body_h - 36, 1, wires, C_CYAN, 0.74, rw);
    draw_meter(c, right_x + 16, body_y + body_h - 17, rw,
               layout.count > 0 ? (100.0 * (double)(layout.count - layout.ready_count) /
                                    (double)layout.count) : 0.0, C_CYAN);

    int timeline_y = body_y + body_h + gap;
    fill_rect(c, margin, timeline_y, width - margin * 2, bottom_h, C_PANEL, 0.90);
    draw_text(c, margin + 16, timeline_y + 12, 1,
              "EXECUTION LANES / NEXT ACTION", C_TEXT, 0.76, width - margin * 2 - 32);
    draw_line(c, margin + 16, timeline_y + 31, width - margin - 16, timeline_y + 31,
              C_DIM, 0.20);
    int step_ids[8], lane_count = 0;
    for (int i = 0; i < visible; i++)
        (void)plan_action_lane_index(&layout, layout.nodes[i].step->id,
                                     step_ids, &lane_count, 8);
    int lane_x = margin + 112;
    int lane_w = width - margin * 2 - 132;
    int lane_h = lane_count > 0 ? (bottom_h - 54) / lane_count : 18;
    if (lane_h < 15) lane_h = 15;
    for (int lane = 0; lane < lane_count; lane++) {
        int yy = timeline_y + 42 + lane * lane_h;
        step_t *step = step_get(step_ids[lane]);
        draw_text_ellipsis(c, margin + 16, yy, 1,
                           step && step->title ? step->title : "STEP",
                           C_DIM, 0.74, 88);
        fill_rect(c, lane_x, yy + 4, lane_w, 7, C_BG_BOTTOM, 0.92);
        for (int i = 0; i < visible; i++) {
            if (!layout.nodes[i].step || layout.nodes[i].step->id != step_ids[lane]) continue;
            int bx = lane_x + (i * lane_w) / (visible > 0 ? visible : 1);
            int bw = lane_w / (visible > 0 ? visible : 1) - 3;
            if (bw < 6) bw = 6;
            fill_rect(c, bx, yy + 4, bw, 7, status_color(layout.nodes[i].atom->status), 0.76);
        }
    }
    int marker = lane_x + (int)(lane_w * motion_phase(1.8, 0.0));
    fill_rect(c, marker, timeline_y + 34, 2, bottom_h - 42, C_TEXT, 0.56);
    char footer[128];
    snprintf(footer, sizeof(footer), "READY %d  ·  WIRED %d  ·  %s",
             layout.ready_count, layout.wire_count,
             layout.ready_count > 0 ? "NEXT ACTION HIGHLIGHTED" : "WAITING ON DEPENDENCIES");
    draw_text(c, margin + 16, timeline_y + bottom_h - 16, 1, footer, C_CYAN, 0.76,
              width - margin * 2 - 32);
    return c;
}

static const char *session_state_name(pixel_tui_state_t state) {
    switch (state) {
    case PIXEL_TUI_REASONING: return "REASONING";
    case PIXEL_TUI_EXECUTING: return "EXECUTING";
    case PIXEL_TUI_RESPONDING: return "RESPONDING";
    default: return "IDLE";
    }
}

static px_color_t session_state_color(pixel_tui_state_t state) {
    switch (state) {
    case PIXEL_TUI_REASONING: return C_CYAN;
    case PIXEL_TUI_EXECUTING: return C_AMBER;
    case PIXEL_TUI_RESPONDING: return C_GREEN;
    default: return C_VIOLET;
    }
}

static double session_transition_progress(void) {
    if (!g_session.active || !g_session.animation_enabled ||
        g_session.previous_state == g_session.state ||
        g_session.transition_started_s <= 0.0)
        return 1.0;
    return smoothstep((monotonic_s() - g_session.transition_started_s) / 0.24);
}

static px_color_t session_animated_accent(pixel_tui_state_t state) {
    if (!g_session.active || state != g_session.state)
        return session_state_color(state);
    double progress = session_transition_progress();
    return color_mix(session_state_color(g_session.previous_state),
                     session_state_color(state), progress);
}

static px_color_t message_role_color(const char *role) {
    if (!role) return C_DIM;
    if (!strcasecmp(role, "USER")) return C_CYAN;
    if (!strcasecmp(role, "ASSISTANT")) return C_VIOLET;
    if (!strncasecmp(role, "TOOL", 4)) return C_AMBER;
    if (!strcasecmp(role, "ERROR")) return C_RED;
    if (!strcasecmp(role, "THINKING")) return C_CYAN;
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
        if (!strcasecmp(role, "USER")) summary.users++;
        else if (!strcasecmp(role, "ASSISTANT")) summary.assistants++;
        else if (!strncasecmp(role, "TOOL", 4)) summary.tools++;
        else if (!strcasecmp(role, "THINKING")) summary.thinking++;
        else if (!strcasecmp(role, "ERROR")) summary.errors++;
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
        if (g_session.tool_visuals[i].used &&
            g_session.tool_visuals[i].status == PIXEL_OP_RUNNING)
            count++;
    return count;
}

static int session_swarm_counts(int *active, int *done, int *errors) {
    int total = 0;
    if (active) *active = 0;
    if (done) *done = 0;
    if (errors) *errors = 0;
    for (int i = 0; i < PIXEL_SWARM_VIS_CAP; i++) {
        const pixel_swarm_visual_t *agent = &g_session.swarm_visuals[i];
        if (!agent->used) continue;
        total++;
        if (!strcasecmp(agent->status, "done")) {
            if (done) (*done)++;
        } else if (!strcasecmp(agent->status, "error") ||
                   !strcasecmp(agent->status, "killed")) {
            if (errors) (*errors)++;
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
    if (!dst || cap == 0) return;
    if (seconds < 0.0) seconds = 0.0;
    int elapsed = (int)seconds;
    if (elapsed < 60)
        snprintf(dst, cap, "%02ds", elapsed);
    else if (elapsed < 3600)
        snprintf(dst, cap, "%02dm %02ds", elapsed / 60, elapsed % 60);
    else
        snprintf(dst, cap, "%02dh %02dm", elapsed / 3600, (elapsed / 60) % 60);
}

static void __attribute__((unused)) draw_state_track(px_canvas_t *c, int x, int y, int w,
                                                     pixel_tui_state_t state) {
    static const char *labels[] = {"READY", "THINK", "TOOLS", "REPLY"};
    int active = (int)state;
    int gap = 5;
    int segment_w = (w - gap * 3) / 4;
    if (!c || segment_w < 28) return;
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
        fill_rect(c, xx, y + 15, segment_w, 1, color,
                  i == active ? 0.58 + pulse * 0.25 : 0.14);
        if (i == active && state != PIXEL_TUI_IDLE)
            draw_motion_sweep(c, xx, y + 14, segment_w, 2, color, 1.20, 0.84);
        if (segment_w >= 58)
            draw_text(c, xx + 6, y + 4, 1, labels[i], color,
                      i == active ? 0.92 : 0.48, segment_w - 12);
    }
}

static void draw_key_value(px_canvas_t *c, int x, int y, int w,
                           const char *key, const char *value,
                           px_color_t value_color) {
    draw_text(c, x, y, 1, key, C_DIM, 0.62, w / 2);
    int value_w = font_compat_measure_utf8(value, text_point_size(1), false);
    if (value_w < 0) value_w = (int)strlen(value) * 6;
    int value_x = x + w - value_w;
    if (value_x < x + w / 2) value_x = x + w / 2;
    draw_text_ellipsis(c, value_x, y, 1, value, value_color, 0.90, x + w - value_x);
}

static void draw_meter(px_canvas_t *c, int x, int y, int w, double percent,
                       px_color_t color) {
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;
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
    if (status == PIXEL_OP_DONE) return C_GREEN;
    if (status == PIXEL_OP_ERROR) return C_RED;
    return C_AMBER;
}

static px_color_t __attribute__((unused)) swarm_visual_color(const char *status) {
    if (status && !strcasecmp(status, "done")) return C_GREEN;
    if (status && (!strcasecmp(status, "error") || !strcasecmp(status, "killed")))
        return C_RED;
    if (status && !strcasecmp(status, "streaming")) return C_CYAN;
    return C_VIOLET;
}

static void draw_turn_trace(px_canvas_t *c, int x, int y, int w, int h) {
    int count = g_session.turn_visual_count;
    if (!c || count < 1 || w < 30 || h < 8) return;
    int shown = count > 8 ? 8 : count;
    int gap = 4;
    int cell_w = (w - gap * (shown - 1)) / shown;
    if (cell_w < 5) cell_w = 5;
    int start = count - shown;
    for (int i = 0; i < shown; i++) {
        const pixel_turn_visual_t *turn = &g_session.turn_visuals[start + i];
        int load = turn->tool_count + turn->swarm_count * 2;
        int bar_h = 4 + (load > 6 ? 6 : load) * 2;
        if (bar_h > h) bar_h = h;
        bool current = i == shown - 1;
        px_color_t color = current ? session_animated_accent(g_session.state) : C_DIM;
        fill_rect(c, x + i * (cell_w + gap), y + h - bar_h,
                  cell_w, bar_h, color, current ? 0.72 : 0.24);
        if (current && g_session.animation_enabled)
            draw_motion_sweep(c, x + i * (cell_w + gap), y + h - 2,
                              cell_w, 2, color, 0.90, 0.82);
    }
}

static void __attribute__((unused)) draw_phase_field(px_canvas_t *c, int x, int y, int w, int h,
                                                     pixel_tui_state_t state) {
    if (!c || w < 30 || h < 12) return;
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
        if (slot_w < 12) slot_w = 12;
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
            fill_rect(c, xx, y + (h - bar_h) / 2, w / 10, bar_h,
                      accent, 0.28 + energy * 0.52);
        }
    } else {
        draw_turn_trace(c, x, y, w, h);
    }
}

static void draw_live_operations(px_canvas_t *c, int x, int y, int w, int h,
                                 pixel_tui_state_t state) {
    if (!c || w < 120 || h < 38) return;
    (void)state;
    draw_text(c, x, y, 1, "ACTIVE OPERATION", C_TEXT, 0.70, w);
    int yy = y + 22;
    const pixel_tool_visual_t *tool = session_latest_tool();
    if (tool) {
        px_color_t color = tool_visual_color(tool->status);
        /* Completion answer: the finished row exhales once — a wash that
         * decays over ~a second — instead of silently flipping color. */
        double flash = session_motion_value(MOTION_KEY_TOOL(tool->sequence),
                                            MOTION_PROP_FLASH, 0.0);
        if (flash > 0.01)
            fill_rounded(c, x - 4, yy - 3, w + 4, 17, 5, color, 0.16 * flash);
        fill_circle(c, x + 3, yy + 5, 3, color, 0.78);
        draw_text_ellipsis(c, x + 13, yy, 1, tool->name, color, 0.90, w - 13);
        yy += 18;
        if (tool->preview[0] && yy + 14 < y + h) {
            draw_text_ellipsis(c, x + 13, yy, 1, tool->preview,
                               C_DIM, 0.72, w - 13);
            yy += 18;
        }
    }
    int active = 0, done = 0, errors = 0;
    int swarm_total = session_swarm_counts(&active, &done, &errors);
    if (swarm_total > 0 && yy + 14 < y + h) {
        char label[80];
        snprintf(label, sizeof(label), "SWARM  %d LIVE  /  %d DONE", active, done);
        draw_text_ellipsis(c, x, yy, 1, label,
                           errors ? C_RED : C_CYAN, 0.76, w);
    }
}

static void draw_session_rail(px_canvas_t *c, int x, int y, int w, int h,
                              const char *model, pixel_tui_state_t state,
                              const session_summary_t *summary) {
    if (!c || !summary || w < 150 || h < 120) return;
    px_color_t accent = session_animated_accent(state);
    int inner_x = x + 14;
    int inner_w = w - 28;
    fill_rect(c, x, y, w, h, C_PANEL, 0.58);
    fill_rect(c, x, y, 1, h, C_DIM, 0.22);
    fill_rect(c, x + w - 1, y, 1, h, C_DIM, 0.12);
    int soul_radius = w >= 220 ? 20 : 16;
    int soul_x = inner_x + soul_radius;
    int soul_y = y + soul_radius + 12;
    draw_dsco_soul(c, soul_x, soul_y, soul_radius, state);
    int label_x = soul_x + soul_radius + 14;
    int label_w = x + w - 16 - label_x;
    draw_text(c, label_x, y + 17, 1, "OVERMIND SOUL", C_TEXT, 0.78, label_w);
    draw_text_ellipsis(c, label_x, y + 35, 1,
                       model && *model ? model : "UNSET MODEL",
                       C_CYAN, 0.88, label_w);
    draw_text(c, label_x, y + 53, 1, session_state_name(state),
              accent, 0.76, label_w);
    draw_line(c, inner_x, y + 68, x + w - 14, y + 68, C_DIM, 0.16);

    char value[64];
    int yy = y + 82;
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
    if (state != PIXEL_TUI_IDLE || session_running_tool_count() > 0 ||
        swarm_total > 0) {
        yy += 28;
        draw_live_operations(c, inner_x, yy, inner_w, y + h - yy - 12, state);
        return;
    }

    if (h < 255) return;
    yy += 27;
    draw_text(c, inner_x, yy, 1, "RESOURCE ENVELOPE", C_TEXT, 0.72, inner_w);
    yy += 20;
    snprintf(value, sizeof(value), "%.0f%%", g_session.context_percent);
    draw_key_value(c, inner_x, yy, inner_w, "CONTEXT", value,
                   g_session.context_percent >= 85.0 ? C_RED :
                   (g_session.context_percent >= 60.0 ? C_AMBER : C_GREEN));
    yy += 15;
    draw_meter(c, inner_x, yy, inner_w,
               session_motion_value(MOTION_KEY_CONTEXT, MOTION_PROP_VALUE,
                                    g_session.context_percent),
               g_session.context_percent >= 85.0 ? C_RED :
               (g_session.context_percent >= 60.0 ? C_AMBER : C_GREEN));
    yy += 17;
    snprintf(value, sizeof(value), "$%.4f", g_session.cost_usd);
    draw_key_value(c, inner_x, yy, inner_w, "COST", value, C_TEXT);
    yy += 19;
    snprintf(value, sizeof(value), "~%zu tok", g_session.thinking_bytes / 4);
    draw_key_value(c, inner_x, yy, inner_w, "REASONING", value, C_TEXT);

    if (h < 385) return;
    yy += 27;
    draw_text(c, inner_x, yy, 1, "TURN TRACE", C_TEXT, 0.72, inner_w);
    draw_turn_trace(c, inner_x, yy + 17, inner_w, 28);
    yy += 54;
    draw_text(c, inner_x, yy, 1, "ACTIVITY MIX", C_TEXT, 0.72, inner_w);
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
    draw_key_value(c, inner_x, yy, inner_w, "ERRORS", value,
                   summary->errors ? C_RED : C_DIM);
}

/* Convert arbitrary provider/tool text into the compact bitmap font's safe
 * subset. ANSI controls and Markdown punctuation must never leak back into
 * Kitty as terminal commands because the image is the primary UI surface. */
static size_t plain_text_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return 0;
    size_t n = 0;
    bool space = false;
    for (size_t i = 0; src && src[i] && n + 1 < cap; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == 0x1b) {
            i++;
            if (src[i] == '[') {
                i++;
                while (src[i] && !((src[i] >= '@' && src[i] <= '~'))) i++;
            } else if (src[i] == ']') {
                while (src[i] && src[i] != '\a' &&
                       !(src[i] == 0x1b && src[i + 1] == '\\')) i++;
                if (src[i] == 0x1b && src[i + 1] == '\\') i++;
            }
            continue;
        }
        if (ch == '\r') continue;
        if (ch == '\n') {
            while (n > 0 && dst[n - 1] == ' ') n--;
            if (n > 0 && dst[n - 1] != '\n') dst[n++] = '\n';
            space = false;
            continue;
        }
        if (ch == '\t' || ch < 0x20) {
            if (!space && n > 0) dst[n++] = ' ';
            space = true;
            continue;
        }
        if (ch >= 0x80) {
            size_t seq = (ch & 0xe0) == 0xc0 ? 2 :
                         (ch & 0xf0) == 0xe0 ? 3 :
                         (ch & 0xf8) == 0xf0 ? 4 : 1;
            size_t valid = 1;
            while (valid < seq && src[i + valid] &&
                   ((unsigned char)src[i + valid] & 0xc0) == 0x80) valid++;
            if (valid != seq || n + seq >= cap) continue;
            memcpy(dst + n, src + i, seq);
            n += seq;
            i += seq - 1;
            space = false;
            continue;
        }
        dst[n++] = (char)ch;
        space = ch == ' ';
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\n')) n--;
    dst[n] = '\0';
    return n;
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
    int turn;
    int indent;
    uint64_t sequence;
    rich_style_t block_style;
} pixel_visual_line_t;

static int utf8_glyph_count(const char *text, size_t bytes) {
    int count = 0;
    for (size_t i = 0; text && i < bytes && text[i]; count++) {
        size_t step = utf8_char_bytes(text + i);
        if (step < 1 || i + step > bytes) step = 1;
        i += step;
    }
    return count;
}

static void visual_line_init(pixel_visual_line_t *line,
                             const pixel_message_t *message, bool first) {
    if (!line || !message) return;
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

static void visual_run_append(pixel_visual_line_t *line, rich_style_t style,
                              int level, const char *text, size_t bytes) {
    if (!line || !text || bytes == 0) return;
    while (bytes > 0) {
        pixel_visual_run_t *run = NULL;
        if (line->run_count > 0) {
            pixel_visual_run_t *last = &line->runs[line->run_count - 1];
            size_t room = sizeof(last->text) - strlen(last->text) - 1;
            if (last->style == style && last->level == level &&
                room >= utf8_char_bytes(text))
                run = last;
        }
        if (!run) {
            if (line->run_count >= PIXEL_VISUAL_RUN_CAP) return;
            run = &line->runs[line->run_count++];
            memset(run, 0, sizeof(*run));
            run->style = style;
            run->level = (uint8_t)level;
        }
        size_t used = strlen(run->text);
        size_t room = sizeof(run->text) - used - 1;
        size_t take = bytes < room ? bytes : room;
        while (take > 0 && ((unsigned char)text[take] & 0xc0) == 0x80) take--;
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

static pixel_visual_line_t *visual_next_line(pixel_visual_line_t *lines,
                                              int *count, int cap,
                                              const pixel_message_t *message,
                                              bool *first) {
    if (!lines || !count || *count >= cap) return NULL;
    pixel_visual_line_t *line = &lines[(*count)++];
    visual_line_init(line, message, first ? *first : false);
    if (first) *first = false;
    return line;
}

static int wrap_rich_message(const pixel_message_t *message, int chars,
                             pixel_visual_line_t *lines, int count, int cap) {
    if (!message || !lines || count >= cap || chars < 8) return count;
    if (!message->text[0] && !message->detail[0] &&
        !strcasecmp(message->role, "THINKING"))
        return count;
    rich_token_t tokens[512];
    size_t token_count = rich_text_parse(message->text, tokens,
                                         sizeof(tokens) / sizeof(tokens[0]));
    bool first = true;
    int message_start = count;
    pixel_visual_line_t *line = visual_next_line(lines, &count, cap, message, &first);
    if (!line) return count;

    if (message->detail[0]) {
        char detail[sizeof(message->detail)];
        plain_text_copy(detail, sizeof(detail), message->detail);
        visual_run_append(line, RICH_STYLE_CODE, 0, detail, strlen(detail));
        line->char_count += utf8_glyph_count(detail, strlen(detail));
        if (message->text[0]) {
            visual_run_append(line, RICH_STYLE_MUTED, 0, "  /  ", 5);
            line->char_count += 5;
        }
    }

    for (size_t ti = 0; ti < token_count && line; ti++) {
        const rich_token_t *token = &tokens[ti];
        if (token->type == RICH_TOKEN_BREAK) {
            if (!visual_line_empty(line) ||
                (ti + 1 < token_count && tokens[ti + 1].type == RICH_TOKEN_BREAK))
                line = visual_next_line(lines, &count, cap, message, &first);
            continue;
        }
        if (token->type == RICH_TOKEN_RULE) {
            if (!visual_line_empty(line))
                line = visual_next_line(lines, &count, cap, message, &first);
            if (!line) break;
            line->rule = true;
            line->block_style = RICH_STYLE_MUTED;
            continue;
        }
        if (token->block_start && !visual_line_empty(line))
            line = visual_next_line(lines, &count, cap, message, &first);
        if (!line) break;
        line->indent = token->indent;
        if (token->style == RICH_STYLE_CODE ||
            token->style == RICH_STYLE_MATH_DISPLAY ||
            token->style == RICH_STYLE_HEADING ||
            token->style == RICH_STYLE_QUOTE ||
            token->style == RICH_STYLE_LIST_MARKER)
            line->block_style = token->style;

        const char *p = token->text;
        while (*p && line) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *word = p;
            while (*p && *p != ' ') p += utf8_char_bytes(p);
            size_t bytes = (size_t)(p - word);
            int glyphs = utf8_glyph_count(word, bytes);
            int needed = glyphs + (line->char_count > 0 ? 1 : 0);
            if (line->char_count > 0 && line->char_count + needed > chars) {
                line = visual_next_line(lines, &count, cap, message, &first);
                if (!line) break;
                line->indent = token->indent;
                line->block_style = token->style;
            }
            if (line->char_count > 0) {
                visual_run_append(line, token->style, token->level, " ", 1);
                line->char_count++;
            }
            while (bytes > 0 && line) {
                int remaining = chars - line->char_count;
                if (remaining < 1) {
                    line = visual_next_line(lines, &count, cap, message, &first);
                    if (!line) break;
                    line->indent = token->indent;
                    line->block_style = token->style;
                    remaining = chars;
                }
                const char *q = word;
                size_t take = 0;
                int taken = 0;
                while (take < bytes && taken < remaining) {
                    size_t step = utf8_char_bytes(q + take);
                    take += step; taken++;
                }
                visual_run_append(line, token->style, token->level, q, take);
                line->char_count += taken;
                word += take; bytes -= take;
                if (bytes > 0)
                    line = visual_next_line(lines, &count, cap, message, &first);
            }
        }
    }
    while (count > message_start + 1 && visual_line_empty(&lines[count - 1])) count--;
    if (count > message_start)
        lines[count - 1].streaming = message->streaming;
    return count;
}

static const char *message_role_icon(const char *role) {
    if (!role) return "";
    if (!strcasecmp(role, "USER")) return "";
    if (!strcasecmp(role, "ASSISTANT")) return "";
    if (!strcasecmp(role, "THINKING")) return "";
    if (!strncasecmp(role, "TOOL", 4)) return "";
    if (!strcasecmp(role, "ERROR")) return "";
    return "";
}

static float rich_style_size(rich_style_t style, int level) {
    if (style == RICH_STYLE_HEADING) return level <= 2 ? 14.0f : 13.0f;
    if (style == RICH_STYLE_MATH_DISPLAY) return 13.0f;
    return 12.0f;
}

static void rich_style_traits(rich_style_t style, bool *bold, bool *italic) {
    if (bold) *bold = style == RICH_STYLE_STRONG ||
                      style == RICH_STYLE_HEADING ||
                      style == RICH_STYLE_MATH_DISPLAY;
    if (italic) *italic = style == RICH_STYLE_EMPHASIS ||
                          style == RICH_STYLE_QUOTE;
}

static int measure_rich_run(const pixel_visual_run_t *run) {
    if (!run || !run->text[0]) return 0;
    bool bold = false, italic = false;
    rich_style_traits(run->style, &bold, &italic);
    int width = font_compat_measure_utf8_styled(
        run->text, rich_style_size(run->style, run->level), bold, italic);
    return width > 0 ? width : (int)strlen(run->text) * 7;
}

static int draw_rich_run(px_canvas_t *c, int x, int y, int max_width,
                         const pixel_visual_run_t *run, int line_h) {
    if (!run || !run->text[0] || max_width < 1) return 0;
    bool bold = false, italic = false;
    rich_style_traits(run->style, &bold, &italic);
    px_color_t color = C_TEXT;
    double alpha = 0.91;
    if (run->style == RICH_STYLE_CODE) { color = C_AMBER; alpha = 0.92; }
    else if (run->style == RICH_STYLE_LINK) { color = C_CYAN; alpha = 0.96; }
    else if (run->style == RICH_STYLE_MATH ||
             run->style == RICH_STYLE_MATH_DISPLAY) { color = C_CYAN; alpha = 0.95; }
    else if (run->style == RICH_STYLE_QUOTE ||
             run->style == RICH_STYLE_MUTED ||
             run->style == RICH_STYLE_STRIKE) { color = C_DIM; alpha = 0.80; }
    else if (run->style == RICH_STYLE_LIST_MARKER) { color = C_VIOLET; alpha = 0.94; }
    int advance = font_compat_draw_rgb_styled(
        (uint8_t *)c->pixels, c->width, c->height,
        c->width * (int)sizeof(px_color_t), x, y, max_width, run->text,
        rich_style_size(run->style, run->level), bold, italic,
        color.r, color.g, color.b, (float)alpha);
    if (advance < 0)
        advance = draw_text(c, x, y, 1, run->text, color, alpha, max_width);
    if (run->style == RICH_STYLE_LINK)
        draw_line(c, x, y + line_h - 3, x + advance, y + line_h - 3,
                  C_CYAN, 0.38);
    else if (run->style == RICH_STYLE_STRIKE)
        draw_line(c, x, y + line_h / 2, x + advance, y + line_h / 2,
                  C_DIM, 0.58);
    return advance;
}

static void draw_session_transcript(px_canvas_t *c, int x, int y, int w, int h) {
    if (!c || w < 80 || h < 20) return;
    const float body_size = 13.0f;
    int measured_h = font_compat_line_height(body_size, false);
    int line_h = measured_h > 0 ? measured_h + 4 : 17;
    int role_w = 88;
    int full_role_w = font_compat_measure_utf8("  ASSIST",
                                               body_size, false);
    if (full_role_w > 0 && full_role_w + 12 > role_w)
        role_w = full_role_w + 12;
    if (role_w > 96) role_w = 96;
    if (role_w > w / 4) role_w = w / 4;
    int alphabet_w = font_compat_measure_utf8("abcdefghijklmnopqrstuvwxyz",
                                              body_size, false);
    int avg_advance = alphabet_w > 0 ? alphabet_w / 26 : 6;
    int chars = (w - role_w - 16) / (avg_advance > 0 ? avg_advance : 1);
    if (chars < 12) chars = 12;
    int line_cap = g_session.message_count * 32;
    if (line_cap < 256) line_cap = 256;
    if (line_cap > 4096) line_cap = 4096;
    pixel_visual_line_t *lines = calloc((size_t)line_cap, sizeof(*lines));
    if (!lines) return;
    int line_count = 0;
    for (int i = 0; i < g_session.message_count; i++) {
        int at = (g_session.message_start + i) % PIXEL_MESSAGE_CAP;
        line_count = wrap_rich_message(&g_session.messages[at], chars, lines,
                                       line_count, line_cap);
    }
    int visible = (h - 8) / line_h;
    if (visible < 1) {
        free(lines);
        return;
    }
    int max_scroll = line_count > visible ? line_count - visible : 0;
    if (g_session.transcript_scroll > max_scroll)
        g_session.transcript_scroll = max_scroll;
    if (g_session.transcript_scroll < 0)
        g_session.transcript_scroll = 0;
    int first = line_count > visible
                    ? line_count - visible - g_session.transcript_scroll
                    : 0;
    if (first < 0) first = 0;
    int last = first + visible;
    if (last > line_count) last = line_count;
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
            fill_rounded(c, x + 4, yy, w - 8, line_h - 2, 4, role_color,
                         0.10 * (1.0 - entrance));
        if (lines[i].first) {
            fill_rect(c, x + slide, yy + 2, 2, line_h - 5, role_color, 0.74);
            char role_label[64];
            const char *short_role = lines[i].role;
            if (!strcasecmp(short_role, "USER")) short_role = "YOU";
            else if (!strcasecmp(short_role, "ASSISTANT")) short_role = "DSCO";
            else if (!strcasecmp(short_role, "THINKING")) short_role = "THINK";
            else if (!strncasecmp(short_role, "TOOL", 4)) short_role = "TOOL";
            snprintf(role_label, sizeof(role_label), "%s  %s",
                     message_role_icon(lines[i].role), short_role);
            font_compat_draw_rgb_styled(
                (uint8_t *)c->pixels, c->width, c->height,
                c->width * (int)sizeof(px_color_t), x + 7 + slide, yy,
                role_w - 9, role_label, body_size, true, false,
                role_color.r, role_color.g, role_color.b, 0.88f);
        }
        int text_base = x + role_w + lines[i].indent * 10 + slide;
        int text_avail = w - (text_base - x);
        if (text_avail < 8) text_avail = 8;
        int line_width = 0;
        for (int ri = 0; ri < lines[i].run_count; ri++)
            line_width += measure_rich_run(&lines[i].runs[ri]);
        int card_w = line_width + 22;
        if (card_w < 160) card_w = 160;
        if (lines[i].block_style == RICH_STYLE_MATH_DISPLAY && card_w < 360)
            card_w = 360;
        if (card_w > text_avail) card_w = text_avail;
        if (lines[i].block_style == RICH_STYLE_CODE) {
            fill_rounded(c, text_base - 4, yy - 2, card_w, line_h, 4,
                         C_PANEL_ALT, 0.54);
            fill_rect(c, text_base - 3, yy - 1, 1, line_h - 2, C_AMBER, 0.55);
        } else if (lines[i].block_style == RICH_STYLE_MATH_DISPLAY) {
            fill_rounded(c, text_base - 4, yy - 2, card_w, line_h, 4,
                         C_PANEL_ALT, 0.58);
            fill_rect(c, text_base - 3, yy - 1, 1, line_h - 2, C_CYAN, 0.60);
        } else if (lines[i].block_style == RICH_STYLE_QUOTE) {
            fill_rounded(c, text_base - 5, yy - 1, 1, line_h - 2, 1,
                         C_VIOLET, 0.48);
        }
        if (lines[i].rule) {
            draw_line(c, text_base, yy + line_h / 2,
                      text_base + text_avail, yy + line_h / 2, C_DIM, 0.30);
        }
        int text_x = text_base;
        if (lines[i].block_style == RICH_STYLE_MATH_DISPLAY &&
            line_width > 0 && line_width + 20 < card_w)
            text_x += (card_w - line_width) / 2 - 4;
        int advance = 0;
        for (int ri = 0; ri < lines[i].run_count; ri++) {
            int drawn = draw_rich_run(c, text_x + advance, yy,
                                      text_avail - advance,
                                      &lines[i].runs[ri], line_h);
            if (drawn > 0) advance += drawn;
            if (advance >= text_avail) break;
        }
        if (lines[i].streaming) {
            double pulse = 0.48 + motion_pulse(0.72, 0.0) * 0.46;
            fill_rect(c, text_x + advance + 3, yy + 2, 2, line_h - 5,
                      role_color, pulse);
        }
    }
    free(lines);
}

static void draw_session_command_help(px_canvas_t *c, int x, int y, int w, int h) {
    if (!c || w < 80 || h < 40 || g_session.command_count < 1) return;
    int line_h = font_compat_line_height(text_point_size(1), false);
    if (line_h < 11) line_h = 11;
    line_h += 2;
    int heading_h = line_h + 8;
    int footer_h = line_h + 4;
    int rows = (h - heading_h - footer_h) / line_h;
    if (rows < 1) return;
    int max_columns = w / 145;
    if (max_columns < 1) max_columns = 1;
    if (max_columns > 5) max_columns = 5;
    int columns = (g_session.command_count + rows - 1) / rows;
    if (columns > max_columns) columns = max_columns;
    if (columns < 1) columns = 1;
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
        if (xx >= x + w) break;
        int available = x + w - xx;
        if (available > cell_w) available = cell_w;
        if (available < 20) continue;
        int max_command_w = available - 4;
        if (available >= 250) max_command_w = available * 2 / 5;
        draw_text_ellipsis(c, xx, yy, 1, g_session.commands[i].command,
                           C_CYAN, 0.94, max_command_w);
        if (available >= 250) {
            int desc_x = xx + max_command_w + 8;
            draw_text_ellipsis(c, desc_x, yy, 1,
                               g_session.commands[i].description,
                               C_DIM, 0.76, available - max_command_w - 8);
        }
    }
    if (visible_count < g_session.command_count) {
        char more[96];
        snprintf(more, sizeof(more), "+%d MORE / START TYPING TO CLOSE REGISTRY",
                 g_session.command_count - visible_count);
        draw_text(c, x, y + heading_h + rows * line_h + 2, 1,
                  more, C_AMBER, 0.72, w);
    }
}

static void draw_session_input(px_canvas_t *c, int x, int y, int w, int h) {
    int scale = c->width >= 640 ? 2 : 1;
    if (w < 40) return;
    char plain[sizeof(g_session.input)];
    size_t plain_len = plain_text_copy(plain, sizeof(plain), g_session.input);
    size_t cursor = g_session.input_cursor;
    if (cursor > plain_len) cursor = plain_len;
    size_t start = 0;
    char prefix[sizeof(g_session.input)];
    while (start < cursor) {
        size_t prefix_len = cursor - start;
        if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1;
        memcpy(prefix, plain + start, prefix_len);
        prefix[prefix_len] = '\0';
        int measured = font_compat_measure_utf8(prefix, text_point_size(scale), false);
        if (measured < 0 || measured <= w - 12) break;
        start += utf8_char_bytes(plain + start);
    }
    int line_h = font_compat_line_height(text_point_size(scale), false);
    if (line_h < 1) line_h = 7 * scale;
    int baseline = y + (h - line_h) / 2;
    draw_text(c, x, baseline, scale, plain + start, C_TEXT, 0.96, w - 8);
    if (g_session.input_active &&
        (!g_session.animation_enabled || motion_phase(1.0, 0.0) < 0.62)) {
        size_t prefix_len = cursor > start ? cursor - start : 0;
        if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1;
        memcpy(prefix, plain + start, prefix_len);
        prefix[prefix_len] = '\0';
        int advance = font_compat_measure_utf8(prefix, text_point_size(scale), false);
        if (advance < 0) advance = (int)prefix_len * 6 * scale;
        int cursor_x = x + advance;
        if (cursor_x > x + w - 3 * scale) cursor_x = x + w - 3 * scale;
        fill_rect(c, cursor_x, baseline, 2, line_h,
                  session_animated_accent(g_session.state), 0.88);
    }
}

static px_canvas_t *render_session_frame(int width, int height, const char *model,
                                         pixel_tui_state_t state) {
    px_canvas_t *c = canvas_acquire(width, height);
    if (!c) return NULL;
    canvas_background(c, 0x4453434fU + (uint32_t)state * 101U);
    px_color_t accent = session_animated_accent(state);
    double active_pulse = state == PIXEL_TUI_IDLE ? 0.0 : motion_pulse(0.90, 0.0);

    /* Ambient field: a barely-there state-tinted glow behind the header keeps
     * the whole surface answering "what is the agent doing" without reading
     * any text. Idle stays neutral. */
    if (state != PIXEL_TUI_IDLE) {
        pixel_fx_surface_t fx = fx_surface(c);
        pixel_fx_stop_t glow[2] = {
            {0.0f, fx_color(accent)},
            {1.0f, fx_color(C_BG_TOP)},
        };
        pixel_fx_gradient_radial(&fx, width / 2, 0, width / 2, glow, 2,
                                 0.05 + active_pulse * 0.03);
    }

    /* The Kitty surface is the first backend of the shared agent shell, not a
     * second layout system. Breakpoints and regions come from native_ui. */
    native_ui_agent_shell_layout_t shell = native_ui_agent_shell_layout(width, height);
    bool wide = shell.shows_inspector;
    int outer = shell.outer_margin;
    int inner = shell.inner_padding;
    int header_h = shell.header.height + shell.header.y;
    int deck_h = shell.composer.height + 6;
    int transcript_y = shell.transcript.y;
    int transcript_h = shell.transcript.height;
    session_summary_t summary = session_summarize();
    int swarm_active = 0, swarm_errors = 0;
    int swarm_total = session_swarm_counts(&swarm_active, NULL, &swarm_errors);
    int running_tools = session_running_tool_count();
    const pixel_tool_visual_t *latest_tool = session_latest_running_tool();

    draw_panel(c, outer, 4, width - outer * 2, header_h - 4, 9, C_PANEL, 0.94);
    fill_rounded(c, outer + 3, 8, 2, header_h - 12, 1, accent,
                 0.62 + active_pulse * 0.24);
    if (state != PIXEL_TUI_IDLE)
        draw_motion_sweep(c, outer + 6, 5, width - outer * 2 - 12, 2, accent,
                          1.60, 0.52);
    const char *header_title = width < 700 ? "DSCO / WORKSPACE" :
                                             "DSCO / AGENT WORKSPACE";
    int soul_radius = header_h < 56 ? 12 : 14;
    int soul_x = outer + inner + soul_radius;
    int soul_y = header_h / 2;
    draw_dsco_soul(c, soul_x, soul_y, soul_radius, state);
    int header_text_x = soul_x + soul_radius + 10;
    draw_text(c, header_text_x, 12, 2, header_title, C_TEXT, 0.92,
              width - header_text_x - 180);
    int state_chip_w = (int)strlen(session_state_name(state)) * 6 + 12;
    draw_chip(c, width - state_chip_w - outer - inner, 10,
              session_state_name(state), accent);
    if (g_session.turn > 0) {
        char turn_label[32];
        snprintf(turn_label, sizeof(turn_label), "TURN %d", g_session.turn);
        draw_text(c, width - state_chip_w - 98, 14, 1, turn_label, C_DIM, 0.72, 70);
    }
    if (header_h >= 58 && model && *model)
        draw_text_ellipsis(c, header_text_x, 31, 1, model, C_CYAN, 0.82,
                           wide ? 360 : width / 2);
    if (!wide && header_h >= 58) {
        char compact_metrics[128];
        snprintf(compact_metrics, sizeof(compact_metrics),
                 "CTX %.0f%%  /  $%.4f  /  Q %d/%d  /  %d EVENTS",
                 g_session.context_percent, g_session.cost_usd,
                 g_session.queue_depth,
                 g_session.queue_capacity > 0 ? g_session.queue_capacity : 8,
                 summary.total);
        draw_text_ellipsis(c, width / 2, 31, 1, compact_metrics,
                           C_DIM, 0.70, width / 2 - outer - inner);
    }
    draw_line(c, header_text_x, header_h - 8,
              width - outer - inner, header_h - 8, C_DIM, 0.12);
    fill_rect(c, header_text_x, header_h - 8,
              state == PIXEL_TUI_IDLE ? 42 : 92, 1, accent, 0.54);

    int content_x = outer;
    int rail_w = shell.inspector.width;
    int transcript_w = shell.transcript.width;
    int rail_x = shell.inspector.x;
    fill_rounded(c, content_x, transcript_y, transcript_w, transcript_h, 8,
                 C_PANEL, 0.38);
    stroke_rounded(c, content_x, transcript_y, transcript_w, transcript_h, 8,
                   C_DIM, 0.22);
    fill_rounded(c, content_x + 2, transcript_y + 6, 2, transcript_h - 12, 1,
                 accent, 0.42);
    draw_text(c, content_x + inner, transcript_y + 7, 1,
              g_session.command_help_active ? "COMMAND REGISTRY" : "TRANSCRIPT",
              C_DIM, 0.66, transcript_w - 180);
    if (!g_session.command_help_active && g_session.transcript_scroll > 0) {
        char scroll_label[48];
        snprintf(scroll_label, sizeof(scroll_label), "SCROLL +%d", g_session.transcript_scroll);
        draw_text(c, content_x + transcript_w - 114, transcript_y + 7, 1,
                  scroll_label, C_AMBER, 0.78, 100);
    } else if (!g_session.command_help_active && latest_tool && running_tools > 0) {
        char tool_label[96];
        snprintf(tool_label, sizeof(tool_label), "TOOL  %s", latest_tool->name);
        draw_text_ellipsis(c, content_x + transcript_w - 172, transcript_y + 7, 1,
                           tool_label, C_AMBER, 0.76, 158);
    } else if (!g_session.command_help_active && swarm_total > 0) {
        char swarm_label[80];
        snprintf(swarm_label, sizeof(swarm_label), "SWARM %d/%d", swarm_active, swarm_total);
        draw_text(c, content_x + transcript_w - 92, transcript_y + 7, 1,
                  swarm_label, swarm_errors ? C_RED : C_CYAN, 0.72, 78);
    } else if (!g_session.command_help_active) {
        char event_label[48];
        snprintf(event_label, sizeof(event_label), "%d EVENTS", summary.total);
        draw_text(c, content_x + transcript_w - 92, transcript_y + 7, 1,
                  event_label, C_DIM, 0.58, 78);
    }
    int transcript_content_y = transcript_y + 21;
    int transcript_content_h = transcript_h - 27;
    /* A live signal strip makes the active phase legible without exposing
     * private reasoning text. Idle workspaces stay quiet; active turns get a
     * phase track and compact activity field above the durable transcript. */
    if (!g_session.command_help_active && state != PIXEL_TUI_IDLE &&
        transcript_content_h >= 110 && transcript_w >= 320) {
        const int telemetry_h = 44;
        int signal_x = content_x + inner;
        int signal_w = transcript_w - inner * 2;
        int signal_y = transcript_content_y;
        fill_rounded(c, signal_x, signal_y, signal_w, telemetry_h, 6,
                     C_PANEL_ALT, 0.62);
        stroke_rounded(c, signal_x, signal_y, signal_w, telemetry_h, 6,
                       C_DIM, 0.18);
        fill_rounded(c, signal_x + 2, signal_y + 4, 2, telemetry_h - 8, 1,
                     accent, 0.58);
        draw_text(c, signal_x + 8, signal_y + 5, 1, "LIVE SIGNAL",
                  C_TEXT, 0.68, 76);
        draw_state_track(c, signal_x + 82, signal_y + 3,
                         signal_w - 90, state);
        draw_phase_field(c, signal_x + 8, signal_y + 25,
                         signal_w - 16, 14, state);
        transcript_content_y += telemetry_h + 6;
        transcript_content_h -= telemetry_h + 6;
    }
    if (g_session.command_help_active)
        draw_session_command_help(c, content_x + inner, transcript_y + 21,
                                  transcript_w - inner * 2, transcript_h - 27);
    else
        draw_session_transcript(c, content_x + inner, transcript_content_y,
                                transcript_w - inner * 2, transcript_content_h);
    if (wide)
        draw_session_rail(c, rail_x, transcript_y, rail_w, transcript_h,
                          model, state, &summary);

    int deck_y = height - deck_h;
    draw_panel(c, outer, deck_y, width - outer * 2, deck_h - 6, 9,
               C_PANEL_ALT, 0.90);
    /* The composer's accent spine breathes only while work is queued: a
     * quiet "input is waiting on the agent" signal. */
    double queue_breath = g_session.queue_depth > 0 ? motion_pulse(1.35, 0.2) : 0.0;
    fill_rounded(c, outer + 3, deck_y + 4, 2, deck_h - 14, 1, accent,
                 0.58 + active_pulse * 0.14 + queue_breath * 0.22);
    draw_text(c, outer + inner, deck_y + 4, 1, "COMPOSER", C_TEXT, 0.72,
              width - 280);
    char live_label[64];
    snprintf(live_label, sizeof(live_label), "LIVE  /  QUEUE %d/%d",
             g_session.queue_depth,
             g_session.queue_capacity > 0 ? g_session.queue_capacity : 8);
    int live_w = font_compat_measure_utf8(live_label, text_point_size(1), false);
    if (live_w < 0) live_w = (int)strlen(live_label) * 6;
    draw_text(c, width - live_w - outer - inner, deck_y + 4, 1, live_label,
              g_session.queue_depth > 0 ? C_AMBER : C_GREEN, 0.72, live_w);
    draw_line(c, outer + inner, deck_y + 16, width - outer - inner,
              deck_y + 16, C_DIM, 0.12);
    draw_text(c, outer + inner, deck_y + 22, 2, "›", accent, 0.92, 16);
    draw_session_input(c, outer + inner + 18, deck_y + 17,
                       width - (outer + inner) * 2 - 18, deck_h - 38);
    if (width >= 620)
        draw_text(c, outer + inner, deck_y + deck_h - 19, 1,
                  "ENTER SEND  /  OPTION+ENTER NEWLINE  /  PGUP PGDN HISTORY  /  CTRL+C INTERRUPT",
                  C_DIM, 0.62, width - (outer + inner) * 2);
    else
        draw_text(c, outer + inner, deck_y + deck_h - 19, 1,
                  "ENTER SEND  /  PGUP PGDN HISTORY", C_DIM, 0.62,
                  width - (outer + inner) * 2);
    return c;
}

static void free_canvas(px_canvas_t *c) {
    if (!c) return;
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

static bool canvas_write_ppm(const char *path, const px_canvas_t *c) {
    if (!path || !*path || !c || !c->pixels) return false;
    char tmp[4096];
    int wrote = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (wrote < 0 || (size_t)wrote >= sizeof(tmp)) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", c->width, c->height);
    size_t count = (size_t)c->width * (size_t)c->height;
    bool ok = fwrite(c->pixels, sizeof(px_color_t), count, f) == count;
    if (fclose(f) != 0) ok = false;
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        (void)unlink(tmp);
    return ok;
}

bool pixel_tui_write_plan_ppm(const char *path, int plan_id, int width) {
    return pixel_tui_write_plan_view_ppm(path, plan_id, width,
                                         PIXEL_PLAN_VIEW_TREE);
}

bool pixel_tui_write_plan_view_ppm(const char *path, int plan_id, int width,
                                   pixel_plan_view_t view) {
    if (!path || !*path) return false;
    px_canvas_t *c = view == PIXEL_PLAN_VIEW_ACTIONS
                   ? render_plan_actions_frame(plan_id, width)
                   : render_plan_frame(plan_id, width, NULL);
    if (!c) return false;
    FILE *f = fopen(path, "wb");
    if (!f) { free_canvas(c); return false; }
    fprintf(f, "P6\n%d %d\n255\n", c->width, c->height);
    bool ok = fwrite(c->pixels, sizeof(px_color_t),
                     (size_t)c->width * (size_t)c->height, f) ==
              (size_t)c->width * (size_t)c->height;
    if (fclose(f) != 0) ok = false;
    free_canvas(c);
    return ok;
}

bool pixel_tui_available(FILE *out) {
    return kitty_graphics_available(out);
}

static bool env_false(const char *name) {
    const char *v = getenv(name);
    return v && (!strcmp(v, "0") || !strcasecmp(v, "false") ||
                 !strcasecmp(v, "no") || !strcasecmp(v, "off"));
}

static bool env_true(const char *name) {
    const char *v = getenv(name);
    return v && (*v == '1' || !strcasecmp(v, "true") ||
                 !strcasecmp(v, "yes") || !strcasecmp(v, "on"));
}

static bool send_kitty_pixels(FILE *out, const char *control, const px_canvas_t *c,
                              bool animation_frame) {
    size_t raw_len = (size_t)c->width * (size_t)c->height * sizeof(px_color_t);
    kitty_graphics_send_options_t options;
    kitty_graphics_send_options_default(&options);
    options.continuation_control = animation_frame ? "a=f,q=2" : "q=2";
    return kitty_graphics_send_pixels(out, control, c->pixels, raw_len, &options);
}

static bool terminal_geometry(FILE *out, int *cols, int *rows,
                              int *pixel_width, int *pixel_height) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(fileno(out), TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0)
        return false;
    *cols = ws.ws_col; *rows = ws.ws_row;
    *pixel_width = ws.ws_xpixel; *pixel_height = ws.ws_ypixel;
    return true;
}

bool pixel_tui_session_active(void) {
    session_lock();
    bool active = g_session.active;
    session_unlock();
    return active;
}

bool pixel_tui_session_terminal_suspended(void) {
    session_lock();
    bool suspended = g_session.active && g_session.terminal_suspended;
    session_unlock();
    return suspended;
}

/* TIOCGWINSZ reports physical pixels. On HiDPI/Retina backing (2x-3x) the
 * compositor's absolute pixel metrics (glyphs, badges, meters, margins) are
 * tuned for ~1x density, so an unscaled canvas renders at half visual size
 * the moment the terminal lands on a Retina panel (e.g. primary-monitor
 * disconnect). Infer the backing ratio from cell pixel density and compose
 * in logical pixels; the Kitty placement already stretches the surface over
 * the full cell rectangle, restoring native visual size. DSCO_PIXEL_TUI_DPR
 * overrides the heuristic (1-4). */
static int requested_device_scale(void) {
    const char *env = getenv("DSCO_PIXEL_TUI_DPR");
    if (env && *env) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end != env && *end == '\0' && v >= 1 && v <= 4) return (int)v;
    }
    return 0;
}

static int render_device_scale(int cols, int rows, int pixel_width, int pixel_height) {
    return native_ui_terminal_viewport(cols, rows, pixel_width, pixel_height,
                                       requested_device_scale()).backing_scale;
}

static void session_render_geometry(int cols, int rows, int pixel_width, int pixel_height,
                                    int *width, int *height) {
    native_ui_viewport_metrics_t viewport = native_ui_terminal_viewport(
        cols, rows, pixel_width, pixel_height, requested_device_scale());
    int w = viewport.logical_width;
    int h = viewport.logical_height;
    if (w < 320) w = 320;
    if (w > 1920) w = 1920;
    if (h < 180) h = 180;
    if (h > 1200) h = 1200;
    *width = w;
    *height = h;
}

static uint32_t session_image_id(uint32_t generation, pixel_tui_state_t state) {
    uint32_t id = 0x44530000U ^ ((uint32_t)getpid() << 5) ^
                  (generation * 0x9e3779b9U) ^ ((uint32_t)state + 1U) * 0x101U;
    return id ? id : (uint32_t)state + 1U;
}

static bool session_upload_state(FILE *out, const char *model, int width, int height,
                                 uint32_t generation, pixel_tui_state_t state,
                                 uint32_t *out_image_id) {
    char control[256];
    uint32_t image_id = session_image_id(generation, state);
    if (out_image_id) *out_image_id = image_id;
    px_canvas_t *canvas = render_session_frame(width, height, model, state);
    if (!canvas) return false;
    /* The canvas is placed over the complete terminal grid below. Its raster
     * dimensions must therefore already match the terminal's logical surface:
     * never let Kitty perform a second resize of an undersized upload. */
    snprintf(control, sizeof(control),
             "a=t,t=d,f=24,s=%d,v=%d,i=%u,q=2,o=z",
             canvas->width, canvas->height, image_id);
    bool sent = send_kitty_pixels(out, control, canvas, false);
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SESSION_SNAPSHOT");
    if (snapshot && *snapshot &&
        ((!g_session.active && state == PIXEL_TUI_IDLE) ||
         (g_session.active && state == g_session.state)))
        (void)canvas_write_ppm(snapshot, canvas);
    free_canvas(canvas);
    return sent;
}

static void session_delete_image(FILE *out, uint32_t image_id, bool free_data) {
    if (!out || !image_id) return;
    fprintf(out, "\033_Ga=d,d=%c,i=%u,q=2\033\\", free_data ? 'I' : 'i', image_id);
}

static void session_clear_overlay(FILE *out) {
    if (!g_session.overlay_image_id) return;
    session_delete_image(out, g_session.overlay_image_id, true);
    g_session.overlay_image_id = 0;
}

static void session_place_current(FILE *out) {
    if (!g_session.active || !out) return;
    uint32_t image_id = g_session.image_ids[g_session.state];
    /* Re-anchor at screen origin after transcript scrolling. The placement is
     * image-only state; save/restore keeps the composer's cursor untouched. */
    fprintf(out, "\0337\033[H");
    session_delete_image(out, image_id, false);
    /* Positive z-index makes this framebuffer authoritative. Cell-oriented
     * fallback output can continue behind it without becoming a second UI. */
    fprintf(out, "\033_Ga=p,i=%u,p=1,c=%d,r=%d,C=1,z=1,q=2\033\\",
            image_id, g_session.cols, g_session.rows);
    fprintf(out, "\0338");
}

static double monotonic_s(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
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
#define SESSION_DAMAGE_MAX_COVERAGE 0.40
/* Periodic full refresh bounds drift from any lost/undelivered patch. */
#define SESSION_PATCH_STREAK_LIMIT 64

typedef struct { int x, y, w, h; } session_rect_t;

static bool session_rects_touch(session_rect_t a, session_rect_t b) {
    return a.x <= b.x + b.w && b.x <= a.x + a.w &&
           a.y <= b.y + b.h && b.y <= a.y + a.h;
}

static session_rect_t session_rect_union(session_rect_t a, session_rect_t b) {
    int left = a.x < b.x ? a.x : b.x;
    int top = a.y < b.y ? a.y : b.y;
    int right = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int bottom = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (session_rect_t){left, top, right - left, bottom - top};
}

static int session_damage_add(session_rect_t *rects, int count,
                              session_rect_t rect) {
    for (int i = 0; i < count;) {
        if (session_rects_touch(rects[i], rect)) {
            rect = session_rect_union(rects[i], rect);
            rects[i] = rects[--count];
            i = 0; /* the grown union may now touch earlier rects */
        } else {
            i++;
        }
    }
    if (count >= SESSION_DAMAGE_MAX_RECTS) return -1;
    rects[count++] = rect;
    return count;
}

/* Diff the rendered canvas against the resident frame in coarse tiles and
 * coalesce dirty tiles into a handful of rectangles. Returns the rect count,
 * 0 when nothing changed, or -1 when patching is not worthwhile. */
static int session_collect_damage(const px_canvas_t *canvas,
                                  session_rect_t *rects) {
    if (!g_session.prev_frame ||
        g_session.prev_frame_width != canvas->width ||
        g_session.prev_frame_height != canvas->height)
        return -1;
    const uint8_t *now_px = (const uint8_t *)canvas->pixels;
    const uint8_t *was_px = g_session.prev_frame;
    size_t stride = (size_t)canvas->width * 3;
    int count = 0;
    size_t dirty_area = 0;
    for (int ty = 0; ty < canvas->height; ty += SESSION_DAMAGE_TILE) {
        int th = canvas->height - ty < SESSION_DAMAGE_TILE
                     ? canvas->height - ty : SESSION_DAMAGE_TILE;
        for (int tx = 0; tx < canvas->width; tx += SESSION_DAMAGE_TILE) {
            int tw = canvas->width - tx < SESSION_DAMAGE_TILE
                         ? canvas->width - tx : SESSION_DAMAGE_TILE;
            bool dirty = false;
            for (int row = 0; row < th; row++) {
                size_t at = (size_t)(ty + row) * stride + (size_t)tx * 3;
                if (memcmp(now_px + at, was_px + at, (size_t)tw * 3) != 0) {
                    dirty = true;
                    break;
                }
            }
            if (!dirty) continue;
            dirty_area += (size_t)tw * (size_t)th;
            count = session_damage_add(rects, count,
                                       (session_rect_t){tx, ty, tw, th});
            if (count < 0) return -1;
        }
    }
    double coverage = (double)dirty_area /
                      ((double)canvas->width * (double)canvas->height);
    if (coverage > SESSION_DAMAGE_MAX_COVERAGE) return -1;
    return count;
}

static bool session_send_patch(FILE *out, const px_canvas_t *canvas,
                               uint32_t image_id, session_rect_t rect) {
    size_t bytes = (size_t)rect.w * (size_t)rect.h * 3;
    uint8_t *region = malloc(bytes);
    if (!region) return false;
    size_t stride = (size_t)canvas->width * 3;
    const uint8_t *src = (const uint8_t *)canvas->pixels;
    for (int row = 0; row < rect.h; row++)
        memcpy(region + (size_t)row * rect.w * 3,
               src + (size_t)(rect.y + row) * stride + (size_t)rect.x * 3,
               (size_t)rect.w * 3);
    char control[192];
    snprintf(control, sizeof(control),
             "a=f,i=%u,r=1,x=%d,y=%d,s=%d,v=%d,q=2,o=z",
             image_id, rect.x, rect.y, rect.w, rect.h);
    kitty_graphics_send_options_t options;
    kitty_graphics_send_options_default(&options);
    options.continuation_control = "a=f,q=2";
    bool sent = kitty_graphics_send_pixels(out, control, region, bytes, &options);
    free(region);
    return sent;
}

/* Retain the uploaded canvas as the diff baseline for `image_id`. */
static void session_store_frame(const px_canvas_t *canvas, uint32_t image_id) {
    size_t need = (size_t)canvas->width * (size_t)canvas->height * 3;
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
    g_session.prev_frame_width = canvas->width;
    g_session.prev_frame_height = canvas->height;
    g_session.prev_frame_image = image_id;
}

static double session_repaint_interval_s(void) {
    double interval = (double)g_session.animation_interval_ms / 1000.0;
    if (interval < 0.033) interval = 0.033;
    return interval * 0.9;
}

static bool session_repaint(FILE *out, bool force) {
    if (!g_session.active || !out) return false;
    if (g_session.terminal_suspended) return true;
    double now = monotonic_s();
    if (!force && g_session.last_paint_s > 0.0 &&
        now - g_session.last_paint_s < session_repaint_interval_s())
        return true;

    px_canvas_t *canvas = render_session_frame(g_session.width, g_session.height,
                                               g_session.model, g_session.state);
    if (!canvas) return false;
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SESSION_SNAPSHOT");
    if (snapshot && *snapshot)
        (void)canvas_write_ppm(snapshot, canvas);

    uint32_t current_id = g_session.image_ids[g_session.state];
    if (g_session.patch_enabled && current_id != 0 &&
        g_session.prev_frame_image == current_id &&
        g_session.patch_streak < SESSION_PATCH_STREAK_LIMIT) {
        session_rect_t rects[SESSION_DAMAGE_MAX_RECTS];
        int count = session_collect_damage(canvas, rects);
        if (count == 0) {
            /* Bit-identical frame: nothing to transmit. */
            g_session.last_paint_s = now;
            free_canvas(canvas);
            return true;
        }
        if (count > 0) {
            bool sent = true;
            for (int i = 0; i < count && sent; i++)
                sent = session_send_patch(out, canvas, current_id, rects[i]);
            if (sent) {
                session_store_frame(canvas, current_id);
                g_session.patch_streak++;
                g_session.last_paint_s = now;
                fflush(out);
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
    snprintf(control, sizeof(control), "a=t,t=d,f=24,s=%d,v=%d,i=%u,q=2,o=z",
             canvas->width, canvas->height, next_id);
    if (!send_kitty_pixels(out, control, canvas, false)) {
        session_delete_image(out, next_id, true);
        free_canvas(canvas);
        return false;
    }
    uint32_t old_id = g_session.image_ids[g_session.state];
    g_session.generation = generation;
    g_session.image_ids[g_session.state] = next_id;
    g_session.image_widths[g_session.state] = canvas->width;
    g_session.image_heights[g_session.state] = canvas->height;
    g_session.last_paint_s = now;
    session_store_frame(canvas, next_id);
    g_session.patch_streak = 0;
    session_place_current(out);
    session_delete_image(out, old_id, true);
    fflush(out);
    free_canvas(canvas);
    return true;
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
    session_render_geometry(cols, rows, pixel_width, pixel_height, &width, &height);
    if (cols == g_session.cols && rows == g_session.rows &&
        width == g_session.width && height == g_session.height) {
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
    session_clear_overlay(out);
    session_place_current(out);
    fflush(out);
    g_session.width = width;
    g_session.height = height;
    (void)session_repaint(out, true);
    return true;
}

static bool session_animation_fast(void) {
    /* Animate the discrete phase transition and any live timeline track,
     * then park. Streaming, tools and swarms already trigger semantic
     * repaints when their data changes; a perpetual full-screen pulse only
     * burns a core and adds no information. */
    if (session_transition_progress() < 1.0)
        return true;
    if (ui_motion_active(&g_session.motion, monotonic_s()))
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

static void session_animation_wake(void) {
    if (g_session.animation_thread_started)
        (void)pthread_cond_signal(&g_animation_cond);
}

/* Tool batches and swarm streams can emit telemetry from many workers at
 * once. Let the compositor coalesce those updates instead of serializing
 * agent work behind a full framebuffer encode/upload per event. Reduced-
 * motion sessions have no compositor thread, so they repaint synchronously. */
static void session_telemetry_repaint(FILE *out) {
    if (g_session.animation_enabled && g_session.animation_thread_started)
        session_animation_wake();
    else
        (void)session_repaint(out, true);
}

static void *session_animation_thread_main(void *arg) {
    (void)arg;
    session_lock();
    while (g_session.active && !g_session.animation_stop) {
        int delay_ms = session_animation_fast() ? g_session.animation_interval_ms : 500;
        struct timespec deadline;
        animation_deadline(&deadline, delay_ms);
        (void)pthread_cond_timedwait(&g_animation_cond, &g_session_mutex, &deadline);
        if (!g_session.active || g_session.animation_stop)
            break;
        if (g_session.terminal_suspended)
            continue;
        FILE *out = session_output(stderr);
        bool animate = session_animation_fast();
        bool resized = session_refresh_geometry_locked(out, false);
        if (animate) g_session.animation_frame++;
        if (!resized && animate)
            (void)session_repaint(out, false);
        ui_motion_prune(&g_session.motion, monotonic_s(), 2.0);
    }
    session_unlock();
    return NULL;
}

static void session_animation_start(void) {
    if (!g_session.animation_enabled || g_session.animation_thread_started)
        return;
    g_session.animation_stop = false;
    if (pthread_create(&g_session.animation_thread, NULL,
                       session_animation_thread_main, NULL) == 0)
        g_session.animation_thread_started = true;
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
    int cols = 80, rows = 24, pixel_width = 0, pixel_height = 0;
    terminal_geometry(out, &cols, &rows, &pixel_width, &pixel_height);
    int width = 0, height = 0;
    session_render_geometry(cols, rows, pixel_width, pixel_height, &width, &height);
    uint32_t generation = 1;
    uint32_t image_ids[4] = {0};

    /* SGR mouse mode gives the composer wheel events while the framebuffer
     * owns the alternate screen. Limit tracking to button events: reporting
     * every motion event only adds input pressure and has no UI consumer. */
    fprintf(out, "\033[?1049h\033[2J\033[H\033[?25l\033[?1000h\033[?1006h");
    /* Upload only the visible state. The previous four-frame eager upload made
     * startup encode and transmit four full screens before input became live;
     * other state images are produced lazily on transition. */
    bool ok = session_upload_state(out, model, width, height, generation,
                                   PIXEL_TUI_IDLE, &image_ids[PIXEL_TUI_IDLE]);
    if (!ok) {
        for (int i = 0; i < 4; i++) session_delete_image(out, image_ids[i], true);
        fprintf(out, "\033[?25h\033[?1049l");
        fflush(out);
        session_unlock();
        return false;
    }
    double started_s = monotonic_s();
    bool animations = !env_false("DSCO_PIXEL_TUI_ANIMATIONS") &&
                      !env_true("DSCO_REDUCED_MOTION") &&
                      !env_true("NO_MOTION") &&
                      !env_true("ACCESSIBILITY_REDUCE_MOTION");
    /* Damage patches make animation frames cheap (dirty rects, not full
     * re-encodes), so the default cadence is 15 fps while motion is live;
     * the loop still parks completely when the timeline settles. */
    int animation_interval_ms = 66;
    const char *fps_env = getenv("DSCO_PIXEL_TUI_FPS");
    if (fps_env && *fps_env) {
        char *end = NULL;
        long fps = strtol(fps_env, &end, 10);
        if (end != fps_env && *end == '\0') {
            if (fps < 2) fps = 2;
            if (fps > 60) fps = 60;
            animation_interval_ms = (int)(1000 / fps);
        }
    }
    g_session = (pixel_session_t){
        .active = true, .generation = generation, .cols = cols, .rows = rows,
        .width = width, .height = height, .state = PIXEL_TUI_IDLE,
        .started_s = started_s, .state_started_s = started_s,
        .turn_started_s = started_s, .queue_capacity = 8,
        .saved_stdout_fd = -1, .saved_stderr_fd = -1, .devnull_fd = -1,
        .capture_read_fd = -1, .capture_write_fd = -1,
        .capture_message = -1, .animation_enabled = animations,
        .animation_interval_ms = animation_interval_ms,
        .previous_state = PIXEL_TUI_IDLE,
        .transition_started_s = started_s,
        .patch_enabled = !env_false("DSCO_PIXEL_TUI_PATCH")
    };
    ui_motion_init(&g_session.motion, !animations);
    memcpy(g_session.image_ids, image_ids, sizeof(image_ids));
    for (int i = 0; i < 4; i++) {
        g_session.image_widths[i] = width;
        g_session.image_heights[i] = height;
    }
    snprintf(g_session.model, sizeof(g_session.model), "%s", model ? model : "");
    g_session.current_message = -1;
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
    session_delete_image(out, g_session.image_ids[g_session.state], false);
    g_session.previous_state = g_session.state;
    g_session.state = state;
    g_session.state_started_s = monotonic_s();
    g_session.transition_started_s = g_session.state_started_s;
    pixel_turn_visual_t *turn_visual = session_current_turn_visual();
    if (turn_visual)
        turn_visual->phase_mask |= 1u << (unsigned)state;
    (void)session_repaint(out, true);
    session_animation_wake();
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
    ui_motion_snap(&g_session.motion, MOTION_KEY_MESSAGE(message->sequence),
                   MOTION_PROP_ENTRANCE, 0.0);
    ui_motion_set(&g_session.motion, MOTION_KEY_MESSAGE(message->sequence),
                  MOTION_PROP_ENTRANCE, 1.0, 0.30, UI_MOTION_EASE_OUT, now);
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
    if (!line || len < 3 || len > 96) return false;
    while (len > 0 && isspace((unsigned char)line[len - 1])) len--;
    if (len < 3 || line[len - 1] != 's' ||
        !isdigit((unsigned char)line[len - 2]))
        return false;
    bool decimal = false;
    for (size_t i = len > 10 ? len - 10 : 0; i + 2 < len; i++) {
        if (isdigit((unsigned char)line[i]) && line[i + 1] == '.' &&
            isdigit((unsigned char)line[i + 2])) {
            decimal = true;
            break;
        }
    }
    if (!decimal) return false;
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
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
    size_t start = 0;
    while (start < len && (line[start] == ' ' || line[start] == '\t')) start++;
    if (start == len) return false;

    /* The cell composer echoes its prompt before the command loop receives
     * the submitted line. User input is already inserted through the semantic
     * USER event path, so recapturing this echo creates a duplicate DSCO row
     * and can displace transient native surfaces such as /help. */
    size_t trimmed_len = len - start;
    const unsigned char *trimmed = (const unsigned char *)line + start;
    bool composer_chevron =
        trimmed_len >= 3 &&
        ((trimmed[0] == 0xe2 && trimmed[1] == 0x80 && trimmed[2] == 0xba) ||
         (trimmed[0] == 0xe2 && trimmed[1] == 0x9d && trimmed[2] == 0xaf));
    if (composer_chevron) return false;
    if (capture_is_progress_tick(line + start, trimmed_len)) return false;

    session_lock();
    if (!g_session.active || g_session.terminal_suspended || g_session.capture_muted) {
        session_unlock();
        return false;
    }
    size_t n = len - start;
    pixel_message_t *message = NULL;
    if (g_session.capture_message >= 0 &&
        g_session.capture_message < PIXEL_MESSAGE_CAP) {
        message = &g_session.messages[g_session.capture_message];
        size_t used = strlen(message->text);
        if (used + 1 + n < sizeof(message->text)) {
            message->text[used++] = '\n';
            memcpy(message->text + used, line + start, n);
            message->text[used + n] = '\0';
        } else {
            message = NULL;
        }
    }
    if (!message) {
        message = session_new_message("DSCO", NULL);
        g_session.capture_message = (int)(message - g_session.messages);
    }
    if (!message->text[0] && n >= sizeof(message->text)) {
        start += n - (sizeof(message->text) - 1);
        n = sizeof(message->text) - 1;
    }
    if (!message->text[0]) {
        memcpy(message->text, line + start, n);
        message->text[n] = '\0';
    }
    message->streaming = false;
    g_session.current_message = -1;
    session_unlock();
    return true;
}

static void *session_capture_thread_main(void *arg) {
    (void)arg;
    char input[2048];
    char line[4096];
    size_t line_len = 0;
    capture_parse_state_t state = CAPTURE_TEXT;
    bool dirty = false;
    double dirty_since = 0.0;

    for (;;) {
        struct pollfd pfd = {.fd = g_session.capture_read_fd, .events = POLLIN | POLLHUP};
        int ready = poll(&pfd, 1, 50);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            if (dirty) {
                session_lock();
                if (g_session.active && !g_session.terminal_suspended)
                    (void)session_repaint(session_output(stderr), true);
                session_unlock();
                dirty = false;
                dirty_since = 0.0;
            }
            if (g_session.capture_stop) break;
            continue;
        }
        if (!(pfd.revents & (POLLIN | POLLHUP))) continue;
        ssize_t got = read(g_session.capture_read_fd, input, sizeof(input));
        if (got <= 0) {
            if (g_session.capture_stop || (pfd.revents & POLLHUP)) break;
            continue;
        }

        for (ssize_t i = 0; i < got; i++) {
            unsigned char ch = (unsigned char)input[i];
            if (state == CAPTURE_ESC) {
                if (ch == '[') state = CAPTURE_CSI;
                else if (ch == ']' || ch == '_' || ch == 'P' || ch == '^')
                    state = CAPTURE_STRING;
                else state = CAPTURE_TEXT;
                continue;
            }
            if (state == CAPTURE_CSI) {
                if (ch >= 0x40 && ch <= 0x7e) state = CAPTURE_TEXT;
                continue;
            }
            if (state == CAPTURE_STRING) {
                if (ch == '\a') state = CAPTURE_TEXT;
                else if (ch == 0x1b) state = CAPTURE_STRING_ESC;
                continue;
            }
            if (state == CAPTURE_STRING_ESC) {
                state = (ch == '\\') ? CAPTURE_TEXT : CAPTURE_STRING;
                continue;
            }
            if (ch == 0x1b) {
                state = CAPTURE_ESC;
            } else if (ch == '\n' || ch == '\r') {
                if (line_len > 0) {
                    dirty |= session_capture_publish(line, line_len);
                    line_len = 0;
                }
            } else if (ch == '\b' || ch == 0x7f) {
                if (line_len > 0) line_len--;
            } else if (ch == '\t') {
                do {
                    if (line_len + 1 < sizeof(line)) line[line_len++] = ' ';
                } while (line_len % 4 != 0);
            } else if (ch >= 0x20) {
                if (line_len + 1 >= sizeof(line)) {
                    dirty |= session_capture_publish(line, line_len);
                    line_len = 0;
                }
                line[line_len++] = (char)ch;
            }
        }

        double now = monotonic_s();
        if (dirty && dirty_since <= 0.0)
            dirty_since = now;
        /* Prefer one frame after a short quiet boundary. For truly continuous
         * output, cap capture-driven repaints at 4 fps so rendering cannot
         * consume the producer or pin a core during startup/tool bursts. */
        if (dirty && now - dirty_since >= 0.25) {
            session_lock();
            if (g_session.active && !g_session.terminal_suspended)
                (void)session_repaint(session_output(stderr), true);
            session_unlock();
            dirty = false;
            dirty_since = 0.0;
        }
    }

    if (line_len > 0) dirty |= session_capture_publish(line, line_len);
    if (dirty) {
        session_lock();
        if (g_session.active && !g_session.terminal_suspended)
            (void)session_repaint(session_output(stderr), true);
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
    session_clear_overlay(out);
    g_session.transcript_scroll = 0;
    (void)session_new_message(role, detail);
    session_capture_set_muted(true);
    (void)session_repaint(out, true);
    session_animation_wake();
    session_unlock();
}

void pixel_tui_session_append_text(FILE *out, const char *text) {
    session_lock();
    if (!g_session.active || !out || !text || !*text) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (g_session.current_message < 0) {
        (void)session_new_message("ASSISTANT", NULL);
        session_capture_set_muted(true);
    }
    pixel_message_t *message = &g_session.messages[g_session.current_message];
    size_t used = strlen(message->text);
    size_t add = strlen(text);
    size_t room = sizeof(message->text) - 1;
    if (add >= room) {
        text += add - room;
        add = room;
        used = 0;
    } else if (used + add > room) {
        size_t drop = used + add - room;
        memmove(message->text, message->text + drop, used - drop + 1);
        used -= drop;
    }
    memcpy(message->text + used, text, add);
    message->text[used + add] = '\0';
    (void)session_repaint(out, false);
    session_unlock();
}

void pixel_tui_session_end_message(FILE *out) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (g_session.current_message >= 0)
        g_session.messages[g_session.current_message].streaming = false;
    g_session.current_message = -1;
    session_capture_set_muted(false);
    (void)session_repaint(out, true);
    session_animation_wake();
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
    if (text && *text) plain_text_copy(message->text, sizeof(message->text), text);
    message->streaming = false;
    g_session.current_message = -1;
    (void)session_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_show_commands(FILE *out, const pixel_tui_command_t *commands,
                                     int count) {
    session_lock();
    if (!g_session.active || !out || !commands || count < 1) {
        session_unlock();
        return;
    }
    out = session_output(out);
    session_clear_overlay(out);
    if (count > PIXEL_COMMAND_CAP) count = PIXEL_COMMAND_CAP;
    g_session.command_count = count;
    for (int i = 0; i < count; i++) {
        snprintf(g_session.commands[i].command,
                 sizeof(g_session.commands[i].command), "%s",
                 commands[i].command ? commands[i].command : "");
        snprintf(g_session.commands[i].description,
                 sizeof(g_session.commands[i].description), "%s",
                 commands[i].description ? commands[i].description : "");
    }
    g_session.command_help_active = true;
    (void)session_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_input(FILE *out, const char *text, size_t cursor, bool active) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    const char *next = text ? text : "";
    size_t next_len = strlen(next);
    size_t next_cursor = cursor > next_len ? next_len : cursor;
    bool close_help = g_session.command_help_active && *next && strcmp(next, "/help") != 0;
    if (!close_help && g_session.input_active == active &&
        g_session.input_cursor == next_cursor && strcmp(g_session.input, next) == 0) {
        session_unlock();
        return;
    }
    if (*next && strcmp(g_session.input, next) != 0)
        session_clear_overlay(out);
    if (*next && strcmp(g_session.input, next) != 0)
        g_session.transcript_scroll = 0;
    snprintf(g_session.input, sizeof(g_session.input), "%s", next);
    g_session.input_cursor = next_cursor;
    g_session.input_active = active;
    if (close_help)
        g_session.command_help_active = false;
    (void)session_repaint(out, true);
    session_animation_wake();
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
    if (next < 0) next = 0;
    if (next > 100000) next = 100000;
    g_session.transcript_scroll = (int)next;
    (void)session_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_turn(FILE *out, int turn) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (turn < 0) turn = 0;
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
        pixel_turn_visual_t *next =
            &g_session.turn_visuals[g_session.turn_visual_count++];
        memset(next, 0, sizeof(*next));
        next->turn = turn;
        next->started_s = now;
        next->phase_mask = 1u << (unsigned)g_session.state;
    }
    g_session.turn = turn;
    g_session.turn_started_s = now;
    g_session.thinking_bytes = 0;
    (void)session_repaint(out, true);
    session_unlock();
}

void pixel_tui_session_set_runtime_metrics(FILE *out, double cost_usd,
                                           double context_percent) {
    session_lock();
    if (!g_session.active || !out) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (cost_usd < 0.0) cost_usd = 0.0;
    if (context_percent < 0.0) context_percent = 0.0;
    if (context_percent > 100.0) context_percent = 100.0;
    bool changed = g_session.cost_usd != cost_usd ||
                   g_session.context_percent != context_percent;
    g_session.cost_usd = cost_usd;
    if (changed)
        ui_motion_set(&g_session.motion, MOTION_KEY_CONTEXT, MOTION_PROP_VALUE,
                      context_percent, 0.6, UI_MOTION_SPRING, monotonic_s());
    g_session.context_percent = context_percent;
    if (changed) {
        (void)session_repaint(out, true);
        session_animation_wake();
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
    if (capacity < 1) capacity = 1;
    if (depth < 0) depth = 0;
    if (depth > capacity) depth = capacity;
    bool changed = g_session.queue_depth != depth ||
                   g_session.queue_capacity != capacity;
    g_session.queue_depth = depth;
    g_session.queue_capacity = capacity;
    if (changed) (void)session_repaint(out, true);
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
    for (int i = 0; i < PIXEL_TOOL_VIS_CAP; i++) {
        if (!g_session.tool_visuals[i].used) {
            slot = i;
            break;
        }
        if (g_session.tool_visuals[i].sequence < oldest) {
            oldest = g_session.tool_visuals[i].sequence;
            slot = i;
        }
    }
    pixel_tool_visual_t *tool = &g_session.tool_visuals[slot];
    memset(tool, 0, sizeof(*tool));
    tool->used = true;
    tool->sequence = ++g_session.next_tool_sequence;
    tool->status = PIXEL_OP_RUNNING;
    tool->started_s = monotonic_s();
    plain_text_copy(tool->name, sizeof(tool->name), name);
    if (input_json && *input_json)
        plain_text_copy(tool->preview, sizeof(tool->preview), input_json);
    pixel_turn_visual_t *turn_visual = session_current_turn_visual();
    if (turn_visual) {
        turn_visual->tool_count++;
        turn_visual->phase_mask |= 1u << PIXEL_TUI_EXECUTING;
    }
    session_clear_overlay(out);
    uint64_t operation_id = tool->sequence;
    session_telemetry_repaint(out);
    session_unlock();
    return operation_id;
}

void pixel_tui_session_tool_end(FILE *out, uint64_t operation_id, const char *name,
                                bool ok, double elapsed_ms) {
    session_lock();
    if (!g_session.active || !out || !name || !*name) {
        session_unlock();
        return;
    }
    out = session_output(out);
    pixel_tool_visual_t *match = NULL;
    for (int i = 0; i < PIXEL_TOOL_VIS_CAP; i++) {
        pixel_tool_visual_t *tool = &g_session.tool_visuals[i];
        if (!tool->used || tool->status != PIXEL_OP_RUNNING || strcmp(tool->name, name) != 0)
            continue;
        if (operation_id != 0 && tool->sequence == operation_id) {
            match = tool;
            break;
        }
        if (operation_id == 0 && (!match || tool->sequence > match->sequence))
            match = tool;
    }
    if (match) {
        match->status = ok ? PIXEL_OP_DONE : PIXEL_OP_ERROR;
        match->elapsed_ms = elapsed_ms >= 0.0 ? elapsed_ms : 0.0;
        double now = monotonic_s();
        ui_motion_snap(&g_session.motion, MOTION_KEY_TOOL(match->sequence),
                       MOTION_PROP_FLASH, 1.0);
        ui_motion_set(&g_session.motion, MOTION_KEY_TOOL(match->sequence),
                      MOTION_PROP_FLASH, 0.0, ok ? 0.9 : 1.6,
                      UI_MOTION_EASE_OUT, now);
    }
    session_telemetry_repaint(out);
    session_unlock();
}

void pixel_tui_session_swarm_update(FILE *out, int child_id, const char *status,
                                    const char *task, const char *model,
                                    size_t output_bytes, double cost_usd) {
    session_lock();
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
    if (slot < 0) slot = oldest_slot;
    pixel_swarm_visual_t *agent = &g_session.swarm_visuals[slot];
    bool is_new = !agent->used || agent->child_id != child_id;
    char previous_status[sizeof(agent->status)];
    snprintf(previous_status, sizeof(previous_status), "%s", agent->status);
    double now = monotonic_s();
    bool new_lifecycle = is_new ||
        (swarm_visual_terminal(previous_status) && !swarm_visual_terminal(status));
    if (new_lifecycle) memset(agent, 0, sizeof(*agent));
    agent->used = true;
    agent->child_id = child_id;
    plain_text_copy(agent->status, sizeof(agent->status), status ? status : "running");
    if (task && *task) plain_text_copy(agent->task, sizeof(agent->task), task);
    if (model && *model) plain_text_copy(agent->model, sizeof(agent->model), model);
    agent->output_bytes = output_bytes;
    agent->cost_usd = cost_usd > 0.0 ? cost_usd : 0.0;
    bool status_changed = strcmp(previous_status, agent->status) != 0;
    bool repaint = new_lifecycle || status_changed || now - agent->updated_s >= 0.16;
    agent->updated_s = now;
    if (new_lifecycle) {
        pixel_turn_visual_t *turn_visual = session_current_turn_visual();
        if (turn_visual) turn_visual->swarm_count++;
    }
    if (repaint) session_telemetry_repaint(out);
    session_unlock();
}

void pixel_tui_session_note_thinking(FILE *out, const char *delta) {
    session_lock();
    if (!g_session.active || !out || !delta) {
        session_unlock();
        return;
    }
    out = session_output(out);
    if (g_session.current_message < 0) {
        (void)session_new_message("THINKING", NULL);
        session_capture_set_muted(true);
    }
    pixel_message_t *message = &g_session.messages[g_session.current_message];
    g_session.thinking_bytes += strlen(delta);
    snprintf(message->detail, sizeof(message->detail),
             "reasoning stream / ~%zu tokens", g_session.thinking_bytes / 4);
    (void)session_repaint(out, false);
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

void pixel_tui_session_suspend_terminal(FILE *out) {
    session_lock();
    if (!g_session.active || g_session.terminal_suspended) {
        session_unlock();
        return;
    }
    out = session_output(out);
    session_delete_image(out, g_session.image_ids[g_session.state], false);
    fputs("\033[?25h\033[0m\033[?1049l", out);
    fflush(out);
    session_restore_stdio();
    g_session.terminal_suspended = true;
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
    fputs("\033[?1049h\033[2J\033[H\033[?25l", out);
    g_session.terminal_suspended = false;
    (void)session_suppress_stdio();
    (void)session_repaint(out, true);
    session_animation_wake();
    session_unlock();
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
    out = session_output(out);
    session_clear_overlay(out);
    for (int i = 0; i < 4; i++) session_delete_image(out, g_session.image_ids[i], true);
    fprintf(out, "\033[?1000l\033[?1006l\033[?25h\033[0m\033[?1049l");
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
    if (capture_write_fd >= 0) close(capture_write_fd);
    session_unlock();

    if (join_animation)
        (void)pthread_join(animation_thread, NULL);
    if (join_capture)
        (void)pthread_join(capture_thread, NULL);

    session_lock();
    g_session.animation_thread_started = false;
    g_session.capture_thread_started = false;
    if (g_session.capture_read_fd >= 0) close(g_session.capture_read_fd);
    if (g_session.tty_out) fclose(g_session.tty_out);
    if (g_session.saved_stdout_fd >= 0) close(g_session.saved_stdout_fd);
    if (g_session.saved_stderr_fd >= 0) close(g_session.saved_stderr_fd);
    if (g_session.devnull_fd >= 0) close(g_session.devnull_fd);
    free(g_session.prev_frame);
    memset(&g_session, 0, sizeof(g_session));
    session_unlock();
}

int pixel_tui_render_plan(FILE *out, int plan_id) {
    pixel_plan_view_t view = PIXEL_PLAN_VIEW_TREE;
    const char *requested = getenv("DSCO_PLAN_VIEW");
    if (requested && (!strcasecmp(requested, "actions") ||
                      !strcasecmp(requested, "action") ||
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
    int dpr = render_device_scale(cols, rows, pixel_width, pixel_height);
    int width = pixel_width > 0 ? pixel_width / dpr - 24 : cols * 10;
    if (width < 640) width = 640;
    if (width > 1280) width = 1280;
    px_canvas_t *c = view == PIXEL_PLAN_VIEW_ACTIONS
                   ? render_plan_actions_frame(plan_id, width)
                   : render_plan_frame(plan_id, width, NULL);
    if (!c) {
        session_unlock();
        return 0;
    }
    const char *snapshot = getenv("DSCO_PIXEL_TUI_SNAPSHOT");
    if (snapshot && *snapshot)
        (void)pixel_tui_write_plan_view_ppm(snapshot, plan_id, width, view);

    int cell_h = (pixel_height > 0 && rows > 0) ? pixel_height / dpr / rows : 20;
    if (cell_h < 8) cell_h = 20;
    int occupied_rows = (c->height + cell_h - 1) / cell_h;
    if (occupied_rows < 4) occupied_rows = 4;
    int row_cap = in_session ? rows - 7 : rows - 2;
    if (occupied_rows > row_cap) occupied_rows = row_cap;
    int placement_cols = cols > 2 ? cols - 2 : cols;

    uint32_t image_id = 0x44534300U ^ ((uint32_t)getpid() << 5) ^ (uint32_t)plan_id;
    if (image_id == 0) image_id = 1;
    if (in_session) session_clear_overlay(out);
    char control[256];
    snprintf(control, sizeof(control),
             "a=T,t=d,f=24,s=%d,v=%d,i=%u,c=%d,r=%d,C=1,z=2,q=2,o=z",
             c->width, c->height, image_id, placement_cols, occupied_rows);
    bool sent = send_kitty_pixels(out, control, c, false);
    free_canvas(c);
    if (!sent) {
        session_unlock();
        return 0;
    }
    if (in_session)
        g_session.overlay_image_id = image_id;
    else
        for (int i = 0; i < occupied_rows; i++) fputc('\n', out);
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
} scene_backend_ctx_t;

static px_color_t scene_token_color(native_ui_color_token_t token) {
    switch (token) {
    case NATIVE_UI_COLOR_CANVAS: return C_BG_TOP;
    case NATIVE_UI_COLOR_SURFACE: return C_PANEL;
    case NATIVE_UI_COLOR_SURFACE_RAISED: return C_PANEL_ALT;
    case NATIVE_UI_COLOR_TEXT_MUTED: return C_DIM;
    case NATIVE_UI_COLOR_ACCENT: return C_CYAN;
    case NATIVE_UI_COLOR_SUCCESS: return C_GREEN;
    case NATIVE_UI_COLOR_WARNING: return C_AMBER;
    case NATIVE_UI_COLOR_DANGER: return C_RED;
    case NATIVE_UI_COLOR_BORDER: return C_DIM;
    case NATIVE_UI_COLOR_FOCUS: return C_CYAN;
    case NATIVE_UI_COLOR_TEXT:
    default: return C_TEXT;
    }
}

static void scene_backend_push_clip(void *context, native_ui_rect_t rect) {
    scene_backend_ctx_t *ctx = context;
    (void)pixel_fx_clip_push(&ctx->fx, rect.x, rect.y, rect.width, rect.height);
}

static void scene_backend_pop_clip(void *context) {
    scene_backend_ctx_t *ctx = context;
    pixel_fx_clip_pop(&ctx->fx);
}

static void scene_backend_fill_rect(void *context, native_ui_rect_t rect,
                                    native_ui_color_token_t color,
                                    uint8_t opacity, uint8_t radius) {
    scene_backend_ctx_t *ctx = context;
    double alpha = (double)opacity / 255.0;
    /* Raised surfaces earn elevation automatically; flat fills stay flat. */
    if (color == NATIVE_UI_COLOR_SURFACE_RAISED && radius > 0)
        pixel_fx_shadow(&ctx->fx, rect.x, rect.y, rect.width, rect.height,
                        radius, 12, 3, (pixel_fx_rgb_t){0, 0, 0}, 0.38 * alpha);
    pixel_fx_fill_rounded(&ctx->fx, rect.x, rect.y, rect.width, rect.height,
                          radius, fx_color(scene_token_color(color)), alpha);
}

static void scene_backend_stroke_rect(void *context, native_ui_rect_t rect,
                                      native_ui_color_token_t color,
                                      uint8_t width, uint8_t radius) {
    scene_backend_ctx_t *ctx = context;
    pixel_fx_stroke_rounded(&ctx->fx, rect.x, rect.y, rect.width, rect.height,
                            radius, width > 0 ? width : 1,
                            fx_color(scene_token_color(color)), 0.55);
}

static int scene_type_scale(native_ui_type_token_t type) {
    return type == NATIVE_UI_TYPE_TITLE || type == NATIVE_UI_TYPE_METRIC ? 2 : 1;
}

static void scene_backend_draw_text(void *context, native_ui_rect_t rect,
                                    const char *text,
                                    native_ui_type_token_t type,
                                    native_ui_color_token_t color,
                                    uint8_t opacity) {
    scene_backend_ctx_t *ctx = context;
    if (!text || !*text || rect.width < 4) return;
    int scale = scene_type_scale(type);
    int line_h = font_compat_line_height(text_point_size(scale), false);
    if (line_h < 1) line_h = 7 * scale + 4;
    int y = rect.y + (rect.height - line_h) / 2;
    if (y < rect.y) y = rect.y;
    px_color_t fg = scene_token_color(color);
    if (type == NATIVE_UI_TYPE_CODE) fg = C_AMBER;
    draw_text_ellipsis(ctx->canvas, rect.x, y, scale, text, fg,
                       (double)opacity / 255.0, rect.width);
}

static void scene_backend_draw_icon(void *context, native_ui_rect_t rect,
                                    const char *name,
                                    native_ui_color_token_t color,
                                    uint8_t opacity) {
    scene_backend_ctx_t *ctx = context;
    (void)name;
    double alpha = (double)opacity / 255.0;
    int cx = rect.x + rect.width / 2;
    int cy = rect.y + rect.height / 2;
    int radius = (rect.width < rect.height ? rect.width : rect.height) / 3;
    if (radius < 2) radius = 2;
    px_color_t fg = scene_token_color(color);
    fill_circle(ctx->canvas, cx, cy, radius, fg, alpha * 0.85);
    draw_circle_ring(ctx->canvas, cx, cy, radius + 2, 1, fg, alpha * 0.4);
}

static void scene_backend_draw_custom(void *context,
                                      const native_ui_node_t *node) {
    scene_backend_ctx_t *ctx = context;
    native_ui_rect_t rect = node->frame;
    px_color_t fg = scene_token_color(node->style.foreground);
    double alpha = (double)node->style.opacity / 255.0;
    if (node->element == NATIVE_UI_ELEMENT_METER) {
        double value = clamp01((double)node->value);
        int track_y = rect.y + rect.height / 2 - 2;
        pixel_fx_fill_rounded(&ctx->fx, rect.x, track_y, rect.width, 4, 2,
                              fx_color(C_DIM), 0.18 * alpha);
        int fill_w = (int)((double)rect.width * value);
        if (fill_w > 0)
            pixel_fx_fill_rounded(&ctx->fx, rect.x, track_y, fill_w, 4, 2,
                                  fx_color(fg), 0.75 * alpha);
        return;
    }
    if (node->element == NATIVE_UI_ELEMENT_SPARKLINE) {
        /* Series ride in node->text as separated numbers; normalized into
         * the frame. A generative panel can stream telemetry as data. */
        double series[48];
        int count = 0;
        const char *p = node->text;
        while (*p && count < 48) {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p) { p++; continue; }
            series[count++] = v;
            p = end;
        }
        if (count < 2 || rect.width < 8 || rect.height < 4) return;
        double lo = series[0], hi = series[0];
        for (int i = 1; i < count; i++) {
            if (series[i] < lo) lo = series[i];
            if (series[i] > hi) hi = series[i];
        }
        double span = hi - lo > 1e-9 ? hi - lo : 1.0;
        int last_x = rect.x, last_y = 0;
        for (int i = 0; i < count; i++) {
            int x = rect.x + (int)((double)i / (double)(count - 1) *
                                   (double)(rect.width - 1));
            int y = rect.y + rect.height - 2 -
                    (int)((series[i] - lo) / span * (double)(rect.height - 4));
            if (i > 0)
                draw_line(ctx->canvas, last_x, last_y, x, y, fg, 0.78 * alpha);
            last_x = x;
            last_y = y;
        }
        /* Latest point gets the emphasis dot. */
        fill_circle(ctx->canvas, last_x, last_y, 2, fg, alpha);
        return;
    }
    if (node->element == NATIVE_UI_ELEMENT_IMAGE) {
        pixel_fx_fill_rounded(&ctx->fx, rect.x, rect.y, rect.width,
                              rect.height, 6, fx_color(C_PANEL_ALT),
                              0.8 * alpha);
        pixel_fx_stroke_rounded(&ctx->fx, rect.x, rect.y, rect.width,
                                rect.height, 6, 1, fx_color(C_DIM), 0.4);
        draw_text_ellipsis(ctx->canvas, rect.x + 8, rect.y + rect.height / 2 - 5,
                           1, node->text[0] ? node->text : "ARTIFACT",
                           C_DIM, 0.7 * alpha, rect.width - 16);
    }
}

static px_canvas_t *render_scene_frame(const char *scene_json, int width,
                                       int requested_height) {
    if (!scene_json || width < 160) return NULL;
    native_ui_scene_t *scene = malloc(sizeof(*scene));
    if (!scene) return NULL;
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
            if (!(node->state & NATIVE_UI_STATE_VISIBLE) ||
                node->first_child >= 0)
                continue;
            int edge = node->frame.y + node->frame.height;
            for (int at = node->parent; at >= 0; at = scene->nodes[at].parent)
                edge += scene->nodes[at].style.padding.bottom;
            if (edge > bottom) bottom = edge;
        }
        height = bottom + 12;
        if (height < 160) height = 160;
        if (height > 900) height = 900;
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
    scene_backend_ctx_t ctx = {.canvas = c, .fx = fx_surface(c)};
    native_ui_backend_t backend = {
        .push_clip = scene_backend_push_clip,
        .pop_clip = scene_backend_pop_clip,
        .fill_rect = scene_backend_fill_rect,
        .stroke_rect = scene_backend_stroke_rect,
        .draw_text = scene_backend_draw_text,
        .draw_icon = scene_backend_draw_icon,
        .draw_custom = scene_backend_draw_custom,
    };
    native_ui_render(scene, NULL, &backend, &ctx);
    free(scene);
    return c;
}

bool pixel_tui_write_scene_ppm(const char *path, const char *scene_json,
                               int width, int height) {
    if (!path || !*path) return false;
    px_canvas_t *c = render_scene_frame(scene_json, width > 0 ? width : 900,
                                        height);
    if (!c) return false;
    bool ok = canvas_write_ppm(path, c);
    free_canvas(c);
    return ok;
}

int pixel_tui_render_scene_json(FILE *out, const char *scene_json) {
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
    int dpr = render_device_scale(cols, rows, pixel_width, pixel_height);
    int width = pixel_width > 0 ? pixel_width / dpr - 24 : cols * 10;
    if (width < 480) width = 480;
    if (width > 1280) width = 1280;
    px_canvas_t *c = render_scene_frame(scene_json, width, 0);
    if (!c) {
        session_unlock();
        return 0;
    }
    int cell_h = (pixel_height > 0 && rows > 0) ? pixel_height / dpr / rows : 20;
    if (cell_h < 8) cell_h = 20;
    int occupied_rows = (c->height + cell_h - 1) / cell_h;
    if (occupied_rows < 3) occupied_rows = 3;
    int row_cap = in_session ? rows - 7 : rows - 2;
    if (occupied_rows > row_cap) occupied_rows = row_cap;
    int placement_cols = cols > 2 ? cols - 2 : cols;

    uint32_t image_id = 0x4453474eU ^ ((uint32_t)getpid() << 5) ^
                        (uint32_t)(g_session.generation + 1);
    if (image_id == 0) image_id = 1;
    if (in_session) session_clear_overlay(out);
    char control[256];
    snprintf(control, sizeof(control),
             "a=T,t=d,f=24,s=%d,v=%d,i=%u,c=%d,r=%d,C=1,z=2,q=2,o=z",
             c->width, c->height, image_id, placement_cols, occupied_rows);
    bool sent = send_kitty_pixels(out, control, c, false);
    free_canvas(c);
    if (!sent) {
        session_unlock();
        return 0;
    }
    if (in_session)
        g_session.overlay_image_id = image_id;
    else
        for (int i = 0; i < occupied_rows; i++) fputc('\n', out);
    fflush(out);
    int result = ferror(out) ? 0 : occupied_rows;
    session_unlock();
    return result;
}

bool tool_ui_render(const char *input_json, char *result, size_t result_len) {
    if (!result || result_len == 0) return false;
    result[0] = '\0';
    if (!input_json || !*input_json) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"empty input\"}");
        return false;
    }
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
        snprintf(result, result_len,
                 "{\"ok\":false,\"error\":\"missing spec object\"}");
        return false;
    }
    char *ppm_path = json_get_str(input_json, "ppm_path");
    int width = json_get_int(input_json, "width", 900);
    bool wrote_ppm = false;
    if (ppm_path && *ppm_path)
        wrote_ppm = pixel_tui_write_scene_ppm(ppm_path, scene_json, width, 0);
    int rows = pixel_tui_render_scene_json(stdout, scene_json);
    bool ok = rows > 0 || wrote_ppm;
    if (ok)
        snprintf(result, result_len,
                 "{\"ok\":true,\"rows\":%d,\"rendered\":%s,\"ppm\":%s%s%s}",
                 rows, rows > 0 ? "true" : "false",
                 wrote_ppm ? "\"" : "null",
                 wrote_ppm ? ppm_path : "",
                 wrote_ppm ? "\"" : "");
    else
        snprintf(result, result_len,
                 "{\"ok\":false,\"error\":\"scene invalid or no kitty surface;"
                 " pass ppm_path for a headless artifact\"}");
    free(spec);
    free(ppm_path);
    return ok;
}
