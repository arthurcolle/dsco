#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "agent.h"
#include "llm.h"
#include "tools.h"
#include "error.h"
#include "config.h"
#include "json_util.h"
#include "ipc.h"
#include "tui.h"
#include "md.h"
#include "baseline.h"
#include "plugin.h"
#include "setup.h"
#include "mcp.h"
#include "provider.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <pthread.h>
#include <sys/time.h>
#include <termios.h>
#include "crypto.h"
#include "output_guard.h"

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

volatile int g_interrupted = 0;

/* Stream heartbeat global — shared with llm.c write callback */
extern tui_stream_heartbeat_t *g_stream_heartbeat;

/* ── MCP integration ───────────────────────────────────────────────────── */

static mcp_registry_t g_mcp = {0};

/* MCP tool execution callback for external tool system */
static char *mcp_tool_execute_cb(const char *name, const char *input_json, void *ctx) {
    mcp_registry_t *reg = (mcp_registry_t *)ctx;
    return mcp_call_tool(reg, name, input_json);
}

static void mcp_register_discovered_tools(mcp_registry_t *reg) {
    int mcp_count;
    const mcp_tool_t *mcp_tools = mcp_get_tools(reg, &mcp_count);
    for (int i = 0; i < mcp_count; i++) {
        tools_register_external(mcp_tools[i].name, mcp_tools[i].description,
                                 mcp_tools[i].input_schema,
                                 mcp_tool_execute_cb, reg);
    }
    if (mcp_count > 0) {
        fprintf(stderr, "  %smcp: %d tools registered%s\n", TUI_DIM, mcp_count, TUI_RESET);
    }
}

/* ── Rate limiter (token bucket) ───────────────────────────────────────── */

typedef struct {
    double tokens;       /* current token count */
    double max_tokens;   /* bucket capacity */
    double refill_rate;  /* tokens per second */
    double last_refill;  /* timestamp */
} rate_limiter_t;

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void rate_limiter_init(rate_limiter_t *rl, double max_tokens, double refill_rate) {
    rl->tokens = max_tokens;
    rl->max_tokens = max_tokens;
    rl->refill_rate = refill_rate;
    rl->last_refill = now_ms();
}

static bool rate_limiter_acquire(rate_limiter_t *rl) {
    if (g_interrupted) return false;
    double now = now_ms();
    double elapsed = now - rl->last_refill;
    rl->tokens += elapsed * rl->refill_rate;
    if (rl->tokens > rl->max_tokens) rl->tokens = rl->max_tokens;
    rl->last_refill = now;

    if (rl->tokens >= 1.0) {
        rl->tokens -= 1.0;
        return true;
    }
    /* Wait for token to become available */
    double wait = (1.0 - rl->tokens) / rl->refill_rate;
    if (wait > 0 && wait < 30.0) {
        fprintf(stderr, "  %srate limit: waiting %.1fs%s\n", TUI_DIM, wait, TUI_RESET);
        if (usleep((useconds_t)(wait * 1e6)) != 0 && errno == EINTR && g_interrupted) {
            return false;
        }
        if (g_interrupted) return false;
        rl->tokens = 0;
        rl->last_refill = now_ms();
    }
    return true;
}

/* ── Cost budget ───────────────────────────────────────────────────────── */

static double g_cost_budget = 0.0;  /* 0 = unlimited */

static double session_cost(session_state_t *session) {
    const model_info_t *mi = model_lookup(session->model);
    if (!mi) return 0;
    return session->total_input_tokens  * mi->input_price / 1e6
         + session->total_output_tokens  * mi->output_price / 1e6
         + session->total_cache_read_tokens * mi->cache_read_price / 1e6
         + session->total_cache_write_tokens * mi->cache_write_price / 1e6;
}

static bool check_cost_budget(session_state_t *session) {
    if (g_cost_budget <= 0) return true;
    double cost = session_cost(session);
    if (cost >= g_cost_budget) {
        fprintf(stderr, "  %s%scost budget exceeded: $%.4f / $%.4f%s\n",
                TUI_BOLD, TUI_RED, cost, g_cost_budget, TUI_RESET);
        return false;
    }
    if (cost >= g_cost_budget * 0.9) {
        fprintf(stderr, "  %scost warning: $%.4f / $%.4f (%.0f%%)%s\n",
                TUI_YELLOW, cost, g_cost_budget, 100.0 * cost / g_cost_budget, TUI_RESET);
    }
    return true;
}

/* ── Provider management ───────────────────────────────────────────────── */

static provider_t *g_provider = NULL;

static void ensure_provider(session_state_t *session, const char *api_key) {
    const char *pname = provider_detect(session->model, api_key);
    if (g_provider && strcmp(g_provider->name, pname) == 0) return;
    provider_free(g_provider);
    g_provider = provider_create(pname);
}

/* ── Image drag-and-drop support ─────────────────────────────────────── */

/* Max image size before downscaling (5MB raw, ~20MP) */
#define IMG_MAX_FILE_SIZE   (20 * 1024 * 1024)
#define IMG_MAX_DIMENSION   1568  /* Anthropic recommended max (avoids resize latency) */
#define IMG_MAX_B64_SIZE    (10 * 1024 * 1024)

static const char *img_media_type_for_ext(const char *ext) {
    if (strcasecmp(ext, ".png") == 0)  return "image/png";
    if (strcasecmp(ext, ".jpg") == 0)  return "image/jpeg";
    if (strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0)  return "image/gif";
    if (strcasecmp(ext, ".webp") == 0) return "image/webp";
    if (strcasecmp(ext, ".bmp") == 0)  return "image/bmp";
    if (strcasecmp(ext, ".tif") == 0)  return "image/tiff";
    if (strcasecmp(ext, ".tiff") == 0) return "image/tiff";
    if (strcasecmp(ext, ".heic") == 0) return "image/heic";
    if (strcasecmp(ext, ".heif") == 0) return "image/heif";
    if (strcasecmp(ext, ".avif") == 0) return "image/avif";
    return NULL;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_inplace(char *s) {
    char *r = s;
    char *w = s;
    while (*r) {
        if (r[0] == '%' && r[1] && r[2]) {
            int hi = hex_val(r[1]);
            int lo = hex_val(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static bool shell_quote_single(const char *src, char *dst, size_t dst_sz) {
    if (!src || !dst || dst_sz < 3) return false;
    size_t d = 0;
    dst[d++] = '\'';
    for (const char *p = src; *p; p++) {
        if (*p == '\'') {
            if (d + 4 >= dst_sz) return false;
            dst[d++] = '\'';
            dst[d++] = '\\';
            dst[d++] = '\'';
            dst[d++] = '\'';
        } else {
            if (d + 1 >= dst_sz) return false;
            dst[d++] = *p;
        }
    }
    if (d + 2 > dst_sz) return false;
    dst[d++] = '\'';
    dst[d] = '\0';
    return true;
}

static const char *extract_image_path(const char *token, char *out_path, size_t out_sz) {
    if (!token || !*token) return NULL;

    size_t tlen = strlen(token);
    const char *start = token;
    if ((token[0] == '\'' || token[0] == '"') && tlen > 2 && token[tlen-1] == token[0]) {
        start = token + 1;
        tlen -= 2;
    }
    if (tlen > 2 && token[0] == '<' && token[tlen-1] == '>') {
        start = token + 1;
        tlen -= 2;
    }

    if (tlen >= out_sz) tlen = out_sz - 1;
    memcpy(out_path, start, tlen);
    out_path[tlen] = '\0';
    while (tlen > 0 && (out_path[tlen-1] == ' ' || out_path[tlen-1] == '\t'))
        out_path[--tlen] = '\0';

    {
        char *r = out_path, *w = out_path;
        while (*r) {
            if (*r == '\\' && *(r+1) == ' ') {
                *w++ = ' ';
                r += 2;
            } else {
                *w++ = *r++;
            }
        }
        *w = '\0';
        tlen = (size_t)(w - out_path);
    }

    if (strncasecmp(out_path, "file://", 7) == 0) {
        char *uri = out_path + 7;
        if (strncasecmp(uri, "localhost/", 10) == 0) uri += 9;
        if (*uri) {
            memmove(out_path, uri, strlen(uri) + 1);
            url_decode_inplace(out_path);
            tlen = strlen(out_path);
        }
    }

    if (out_path[0] == '~' && (out_path[1] == '/' || out_path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            char tmp[4096];
            snprintf(tmp, sizeof(tmp), "%s%s", home, out_path + 1);
            snprintf(out_path, out_sz, "%s", tmp);
        }
    }

    const char *dot = strrchr(out_path, '.');
    if (!dot) return NULL;
    return img_media_type_for_ext(dot);
}

static char *load_and_encode_image(const char *path, const char *media_type,
                                    tui_spinner_t *spinner) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    if (st.st_size > IMG_MAX_FILE_SIZE) return NULL;

    const char *read_path = path;
    char downscaled[512] = "";

    if (st.st_size > 1024 * 1024 &&
        strcmp(media_type, "image/gif") != 0) {
        if (spinner) {
            spinner->label = "downsizing image...";
            tui_spinner_tick(spinner);
        }
        snprintf(downscaled, sizeof(downscaled), "/tmp/dsco_drag_%d.jpg", getpid());
        char q_path[8192];
        char q_out[1024];
        int rc = -1;
        if (shell_quote_single(path, q_path, sizeof(q_path)) &&
            shell_quote_single(downscaled, q_out, sizeof(q_out))) {
            char cmd[12288];
            snprintf(cmd, sizeof(cmd),
                     "sips --resampleHeightWidthMax %d %s --setProperty format jpeg --out %s 2>/dev/null",
                     IMG_MAX_DIMENSION, q_path, q_out);
            rc = system(cmd);
        }
        if (rc == 0 && stat(downscaled, &st) == 0) {
            read_path = downscaled;
            media_type = "image/jpeg";
        } else {
            downscaled[0] = '\0';
        }
    }

    if (spinner) {
        spinner->label = "encoding image...";
        tui_spinner_tick(spinner);
    }

    FILE *f = fopen(read_path, "rb");
    if (!f) {
        if (downscaled[0]) unlink(downscaled);
        return NULL;
    }

    size_t file_sz = (size_t)st.st_size;
    uint8_t *raw = safe_malloc(file_sz);
    size_t nread = fread(raw, 1, file_sz, f);
    fclose(f);

    if (downscaled[0]) unlink(downscaled);

    size_t b64_sz = ((nread + 2) / 3) * 4 + 1;
    if (b64_sz > (size_t)IMG_MAX_B64_SIZE) { free(raw); return NULL; }
    char *b64 = safe_malloc(b64_sz);
    size_t olen = base64_encode(raw, nread, b64, b64_sz);
    b64[olen] = '\0';
    free(raw);

    return b64;
}

static int process_dragged_images(char *input_buf, conversation_t *conv) {
    bool has_img_ext = false;
    const char *exts[] = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp",
                          ".tif", ".tiff", ".heic", ".heif", ".avif",
                          ".PNG", ".JPG", ".JPEG", ".GIF", ".WEBP", ".BMP",
                          ".TIF", ".TIFF", ".HEIC", ".HEIF", ".AVIF"};
    for (int i = 0; i < 22; i++) {
        if (strstr(input_buf, exts[i])) { has_img_ext = true; break; }
    }
    if (!has_img_ext && !strstr(input_buf, "file://")) return 0;

    char clean_path[4096];
    int img_count = 0;

    char *images_b64[16] = {0};
    const char *images_mt[16] = {0};
    char remaining_text[MAX_INPUT_LINE];
    remaining_text[0] = '\0';
    int rem_pos = 0;

    char *buf = safe_strdup(input_buf);
    char *p = buf;

    while (*p && img_count < 16) {
        char *ext_pos = NULL;
        const char *matched_ext = NULL;
        for (int i = 0; i < 22; i++) {
            char *found = strstr(p, exts[i]);
            if (found && (!ext_pos || found < ext_pos)) {
                ext_pos = found;
                matched_ext = exts[i];
            }
        }

        if (!ext_pos) {
            size_t left = strlen(p);
            if (left > 0 && rem_pos + (int)left < MAX_INPUT_LINE - 1) {
                memcpy(remaining_text + rem_pos, p, left);
                rem_pos += (int)left;
            }
            break;
        }

        char *path_end = ext_pos + strlen(matched_ext);

        char *path_start = ext_pos;
        bool in_quotes = false;
        while (path_start > p) {
            char prev = *(path_start - 1);
            if (prev == '\'' || prev == '"') {
                if (path_start - 1 == p || *(path_start - 2) == ' ') {
                    path_start--;
                    in_quotes = true;
                    if (*path_end == prev) path_end++;
                    break;
                }
            }
            if (prev == ' ' && !in_quotes) {
                if (path_start >= p + 2 && *(path_start - 2) == '\\') {
                    path_start -= 2;
                    continue;
                }
                break;
            }
            path_start--;
        }

        size_t prefix_len = (size_t)(path_start - p);
        if (prefix_len > 0 && rem_pos + (int)prefix_len < MAX_INPUT_LINE - 1) {
            memcpy(remaining_text + rem_pos, p, prefix_len);
            rem_pos += (int)prefix_len;
        }

        size_t plen = (size_t)(path_end - path_start);
        char token[4096];
        if (plen < sizeof(token)) {
            memcpy(token, path_start, plen);
            token[plen] = '\0';

            const char *mt = extract_image_path(token, clean_path, sizeof(clean_path));
            if (mt && access(clean_path, R_OK) == 0) {
                tui_spinner_t spin;
                tui_spinner_init(&spin, SPINNER_DOTS, "dragging image...", TUI_CYAN);
                tui_spinner_tick(&spin);

                char *b64 = load_and_encode_image(clean_path, mt, &spin);
                if (b64) {
                    images_b64[img_count] = b64;
                    images_mt[img_count] = mt;
                    img_count++;

                    const char *fname = strrchr(clean_path, '/');
                    fname = fname ? fname + 1 : clean_path;
                    char done_msg[256];
                    snprintf(done_msg, sizeof(done_msg), "image loaded: %s", fname);
                    tui_spinner_done(&spin, done_msg);
                } else {
                    tui_spinner_done(&spin, "failed to load image");
                    if (rem_pos + (int)plen < MAX_INPUT_LINE - 1) {
                        memcpy(remaining_text + rem_pos, path_start, plen);
                        rem_pos += (int)plen;
                    }
                }
            } else {
                if (rem_pos + (int)plen < MAX_INPUT_LINE - 1) {
                    memcpy(remaining_text + rem_pos, path_start, plen);
                    rem_pos += (int)plen;
                }
            }
        }

        p = path_end;
    }

    remaining_text[rem_pos] = '\0';
    free(buf);

    if (img_count == 0) return 0;

    char *text = remaining_text;
    while (*text == ' ') text++;
    size_t tlen = strlen(text);
    while (tlen > 0 && text[tlen-1] == ' ') text[--tlen] = '\0';

    if (img_count == 1) {
        conv_add_user_image_base64(conv, images_mt[0], images_b64[0],
                                    tlen > 0 ? text : "Describe this image.");
    } else {
        if (tlen > 0) {
            conv_add_user_text(conv, text);
        }
        for (int i = 0; i < img_count; i++) {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "Image %d of %d", i + 1, img_count);
            conv_add_user_image_base64(conv, images_mt[i], images_b64[i],
                                        i == 0 && tlen == 0 ? "Describe these images." : prompt);
        }
    }

    for (int i = 0; i < img_count; i++) free(images_b64[i]);

    if (tlen > 0) {
        snprintf(input_buf, MAX_INPUT_LINE, "%s", text);
    } else {
        snprintf(input_buf, MAX_INPUT_LINE, "[%d image(s)]", img_count);
    }

    return img_count;
}

static void sigint_handler(int sig) {
    (void)sig;
    /* First Ctrl+C interrupts current stream/tool round.
       Second Ctrl+C is a hard exit if we're already interrupted/stuck. */
    if (g_interrupted) {
        /* Reset terminal state before hard exit:
           - Reset scroll region to full terminal
           - Disable bracketed paste mode
           - Reset all SGR attributes
           - Show cursor */
        const char reset[] = "\033[r\033[?2004l\033[0m\033[?25h\n";
        (void)write(STDERR_FILENO, reset, sizeof(reset) - 1);
        _exit(130);
    }
    g_interrupted = 1;
}

static void sigtstp_handler(int sig) {
    (void)sig;
    /* Reset terminal state before suspend so the parent shell isn't corrupted */
    const char reset[] = "\033[r\033[?2004l\033[0m\033[?25h";
    (void)write(STDERR_FILENO, reset, sizeof(reset) - 1);
    /* Re-raise default SIGTSTP to actually suspend */
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
}

/* ── Terminal cleanup atexit handler ───────────────────────────────────── */
static void terminal_reset_atexit(void) {
    /* Safety net: always restore terminal to sane state on exit.
       This catches all exit() paths including readline EOF, quit command, etc.
       Reset: scroll region, bracketed paste, SGR attributes, cursor visibility. */
    fprintf(stderr, "\033[r\033[?2004l\033[0m\033[?25h");
    fflush(stderr);
}

/* ── SIGWINCH handler (terminal resize) ────────────────────────────────── */
static tui_status_bar_t *g_winch_sb = NULL;  /* set in agent_run */
static volatile sig_atomic_t g_winch_pending = 0;

static void sigwinch_handler(int sig) {
    (void)sig;
    /* Only set flag — actual resize handling happens in main loop
       (fprintf/mutex are not async-signal-safe) */
    g_winch_pending = 1;
}

/* Called from main loop to handle deferred SIGWINCH resize */
static void handle_pending_winch(void) {
    if (!g_winch_pending) return;
    g_winch_pending = 0;

    if (!g_winch_sb || !g_winch_sb->visible) return;

    int rows = tui_term_height();
    int panel = g_winch_sb->panel_rows > 0 ? g_winch_sb->panel_rows : 3;
    int scroll_bottom = rows - panel;

    tui_term_lock();
    /* Reset scroll region to new dimensions */
    fprintf(stderr, "\033[1;%dr", scroll_bottom);

    /* Clear old panel rows (may have shifted) then redraw */
    for (int i = 0; i < panel; i++) {
        fprintf(stderr, "\033[%d;1H\033[2K", rows - i);
    }
    fflush(stderr);
    tui_term_unlock();

    tui_status_bar_render(g_winch_sb);
    tui_input_panel_render(g_winch_sb, NULL);
}

/* ── Auto-save ─────────────────────────────────────────────────────────── */

static void autosave(conversation_t *conv, session_state_t *session) {
    if (conv->count == 0) return;
    char dir_path[512], save_path[560];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(dir_path, sizeof(dir_path), "%s/.dsco/sessions", home);
    mkdir(dir_path, 0755);
    snprintf(save_path, sizeof(save_path), "%s/_autosave.json", dir_path);
    conv_save_ex(conv, session, save_path);
}

/* Global for signal-handler auto-save */
static conversation_t *g_autosave_conv = NULL;
static session_state_t *g_autosave_session = NULL;

static void exit_autosave_handler(void) {
    if (g_autosave_conv) autosave(g_autosave_conv, g_autosave_session);
}

static void sigterm_autosave(int sig) {
    (void)sig;
    if (g_autosave_conv) autosave(g_autosave_conv, g_autosave_session);
    /* Reset terminal before exit — scroll region, bracketed paste, SGR */
    const char reset[] = "\033[r\033[?2004l\033[0m\033[?25h\n";
    (void)write(STDERR_FILENO, reset, sizeof(reset) - 1);
    _exit(0);
}

/* ── Cost tracking ─────────────────────────────────────────────────────── */

static void print_cost(session_state_t *session) {
    const model_info_t *mi = model_lookup(session->model);
    if (!mi) {
        fprintf(stderr, "  %sunknown model for pricing%s\n", TUI_DIM, TUI_RESET);
        return;
    }
    double in_cost  = session->total_input_tokens  * mi->input_price / 1e6;
    double out_cost = session->total_output_tokens  * mi->output_price / 1e6;
    double cr_cost  = session->total_cache_read_tokens * mi->cache_read_price / 1e6;
    double cw_cost  = session->total_cache_write_tokens * mi->cache_write_price / 1e6;
    double total = in_cost + out_cost + cr_cost + cw_cost;

    fprintf(stderr, "\n");
    tui_header("Session Cost", TUI_BCYAN);
    fprintf(stderr, "  %sModel:%s       %s\n", TUI_DIM, TUI_RESET, session->model);
    fprintf(stderr, "  %sTurns:%s       %d\n", TUI_DIM, TUI_RESET, session->turn_count);
    fprintf(stderr, "  %sInput:%s       %d tokens  ($%.4f)\n", TUI_DIM, TUI_RESET,
            session->total_input_tokens, in_cost);
    fprintf(stderr, "  %sOutput:%s      %d tokens  ($%.4f)\n", TUI_DIM, TUI_RESET,
            session->total_output_tokens, out_cost);
    if (session->total_cache_read_tokens > 0)
        fprintf(stderr, "  %sCache read:%s  %d tokens  ($%.4f)\n", TUI_DIM, TUI_RESET,
                session->total_cache_read_tokens, cr_cost);
    if (session->total_cache_write_tokens > 0)
        fprintf(stderr, "  %sCache write:%s %d tokens  ($%.4f)\n", TUI_DIM, TUI_RESET,
                session->total_cache_write_tokens, cw_cost);
    fprintf(stderr, "  %s%s──────────────────────%s\n", TUI_BOLD, TUI_CYAN, TUI_RESET);
    fprintf(stderr, "  %sTotal:%s       %s$%.4f%s\n\n", TUI_BOLD, TUI_RESET,
            TUI_BGREEN, total, TUI_RESET);
}

/* ── Context display ───────────────────────────────────────────────────── */

static void print_context(session_state_t *session, int last_input_tokens) {
    int ctx = session->context_window;
    if (ctx <= 0) ctx = CONTEXT_WINDOW_TOKENS;
    double pct = 100.0 * last_input_tokens / ctx;
    int bar_width = 40;
    int filled = (int)(bar_width * pct / 100.0);
    if (filled > bar_width) filled = bar_width;

    fprintf(stderr, "\n  %sContext:%s %d / %d tokens (%.1f%%)\n  [", TUI_DIM, TUI_RESET,
            last_input_tokens, ctx, pct);
    const char *color = pct < 50 ? TUI_GREEN : pct < 80 ? TUI_YELLOW : TUI_RED;
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) fprintf(stderr, "%s\xe2\x96\x88%s", color, TUI_RESET);
        else fprintf(stderr, "%s\xe2\x96\x91%s", TUI_DIM, TUI_RESET);
    }
    fprintf(stderr, "]\n\n");
}

/* ── Readline tab completion ───────────────────────────────────────────── */

#ifdef HAVE_READLINE
static const char *s_commands[] = {
    "/clear", "/save", "/load", "/sessions", "/setup", "/tools",
    "/plugins", "/plugins validate", "/help", "/model", "/cost", "/context", "/effort",
    "/compact", "/version", "/force", "/budget", "/trust", "/web", "/code",
    "/mcp", "/provider", "/status", "/temp", "/thinking", "/fallback",
    "/metrics", "/telemetry", "/cache", "/trace",
    "/features", "/perf", "/minimap",
    "quit", "exit", NULL
};

static char *command_generator(const char *text, int state) {
    static int idx, len;
    if (!state) { idx = 0; len = (int)strlen(text); }
    while (s_commands[idx]) {
        const char *cmd = s_commands[idx++];
        if (strncmp(cmd, text, (size_t)len) == 0)
            return strdup(cmd);
    }
    /* Also complete model aliases */
    static int model_idx;
    if (idx == 0) model_idx = 0; /* won't reach here but guard */
    if (strncmp(text, "/model ", 7) == 0) {
        const char *partial = text + 7;
        int plen = (int)strlen(partial);
        for (int i = model_idx; MODEL_REGISTRY[i].alias; i++) {
            model_idx = i + 1;
            if (strncmp(MODEL_REGISTRY[i].alias, partial, (size_t)plen) == 0) {
                char *r = malloc(8 + strlen(MODEL_REGISTRY[i].alias));
                sprintf(r, "/model %s", MODEL_REGISTRY[i].alias);
                return r;
            }
        }
    }
    return NULL;
}

static char **command_completion(const char *text, int start, int end) {
    (void)start; (void)end;
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, command_generator);
}
#endif

/* ── Parallel tool execution ───────────────────────────────────────────── */

/* ── Streaming callbacks ───────────────────────────────────────────────── */

static bool s_in_text_block = false;
static bool s_in_thinking_block = false;
static md_renderer_t s_md;

/* ── 40 Features: static state ──────────────────────────────────────────── */
static tui_features_t       g_features;
static tui_cadence_t        s_cadence;
static tui_word_counter_t   s_word_counter;
static tui_thinking_state_t s_thinking;
static tui_flame_t          s_flame;
static tui_dag_t            s_dag;
static tui_branch_t         s_branch;
static tui_citation_t       s_citations;
static tui_throughput_t     s_throughput;
static tui_ghost_t          s_ghost;
static tui_latency_breakdown_t s_last_latency;
static tui_stream_heartbeat_t  s_heartbeat;

static void on_stream_text(const char *text, void *ctx) {
    (void)ctx;
    tui_stream_heartbeat_poke(&s_heartbeat, NULL);
    /* End thinking block if transitioning thinking → text */
    if (s_in_thinking_block) {
        tui_term_lock();
        if (g_features.collapsible_thinking) {
            tui_thinking_end(&s_thinking);
        } else {
            fprintf(stderr, "\033[0m\n");
            fflush(stderr);
        }
        tui_term_unlock();
        s_in_thinking_block = false;
    }
    if (!s_in_text_block) {
        s_in_text_block = true;
        tui_word_counter_init(&s_word_counter);
    }
    /* F2: Typing cadence — buffer and flush at steady rate */
    tui_term_lock();
    if (g_features.typing_cadence) {
        tui_cadence_feed(&s_cadence, text);
        /* Flush cadence buffer into markdown renderer */
        if (s_cadence.len > 0) {
            s_cadence.buf[s_cadence.len] = '\0';
            md_feed_str(&s_md, s_cadence.buf);
            s_cadence.len = 0;
        }
    } else {
        md_feed_str(&s_md, text);
    }
    fflush(stderr);
    tui_term_unlock();
    /* F5: Live word count */
    tui_word_counter_feed(&s_word_counter, text);
    tui_word_counter_render(&s_word_counter);
    /* F39: Throughput tracking */
    tui_throughput_tick(&s_throughput, tui_estimate_tokens(text));
}

static void on_stream_thinking(const char *text, void *ctx) {
    (void)ctx;
    tui_stream_heartbeat_poke(&s_heartbeat, "thinking...");
    if (!s_in_thinking_block) {
        s_in_thinking_block = true;
        tui_thinking_init(&s_thinking);
        /* Print thinking header (since llm.c defers to us when callback is set) */
        if (!g_features.collapsible_thinking) {
            tui_term_lock();
            fprintf(stderr, "  \033[2m\033[3m[thinking]\n");
            fflush(stderr);
            tui_term_unlock();
        }
    }
    /* F4: Collapsible thinking — count silently instead of printing */
    tui_term_lock();
    if (g_features.collapsible_thinking) {
        tui_thinking_feed(&s_thinking, text);
    } else {
        fprintf(stderr, " %s", text);
        fflush(stderr);
    }
    tui_term_unlock();
}

static void on_stream_tool_start(const char *name, const char *id, void *ctx) {
    (void)ctx;
    tui_stream_heartbeat_poke(&s_heartbeat, NULL);
    if (s_in_thinking_block) {
        /* F4: End collapsible thinking */
        if (g_features.collapsible_thinking) {
            tui_thinking_end(&s_thinking);
        } else {
            fprintf(stderr, "\n");
        }
        s_in_thinking_block = false;
    }
    if (s_in_text_block) {
        tui_term_lock();
        md_flush(&s_md);
        fprintf(stderr, "\n");
        fflush(stderr);
        tui_term_unlock();
        /* F5: End word counter */
        tui_word_counter_end(&s_word_counter);
        s_in_text_block = false;
        tui_transition_divider();
    }
    /* F7: Citation footnotes — assign footnote number */
    if (g_features.citation_footnotes) {
        int fn = tui_citation_add(&s_citations, name, id, NULL, 0);
        fprintf(stderr, "  %s[%d]%s ", TUI_DIM, fn, TUI_RESET);
    }
    /* Defer printing ⚡ name — will be merged with args at execution time */
    baseline_log("tool", name, "tool_use started", NULL);
}

/* Extract tool args into a single-line preview string */
static void extract_tool_preview(const char *name, const char *input_json,
                                  char *out, size_t out_sz) {
    (void)name;
    out[0] = '\0';
    if (!input_json || strcmp(input_json, "{}") == 0) return;

    /* Try to extract key values from JSON for common tools */
    char *cmd = json_get_str(input_json, "command");
    char *code = json_get_str(input_json, "code");
    char *query = json_get_str(input_json, "query");
    char *path = json_get_str(input_json, "file_path");
    char *url = json_get_str(input_json, "url");
    char *pattern = json_get_str(input_json, "pattern");
    char *expr = json_get_str(input_json, "expression");

    int max = (int)out_sz - 1;
    if (max > 200) max = 200;

    if (cmd) {
        snprintf(out, out_sz, "$ %.*s", max - 2, cmd);
    } else if (code) {
        const char *nl = strchr(code, '\n');
        int len = nl ? (int)(nl - code) : (int)strlen(code);
        if (len > max) len = max;
        snprintf(out, out_sz, "%.*s%s", len, code, nl ? " ..." : "");
    } else if (path && pattern) {
        snprintf(out, out_sz, "%s ~ /%s/", path, pattern);
    } else if (path) {
        snprintf(out, out_sz, "%s", path);
    } else if (query) {
        snprintf(out, out_sz, "%.*s", max, query);
    } else if (url) {
        snprintf(out, out_sz, "%.*s", max, url);
    } else if (pattern) {
        snprintf(out, out_sz, "/%.*s/", max - 2, pattern);
    } else if (expr) {
        snprintf(out, out_sz, "%.*s", max, expr);
    } else {
        const char *start = input_json;
        if (*start == '{') start++;
        int len = (int)strlen(start);
        if (len > 0 && start[len-1] == '}') len--;
        if (len > max) len = max;
        if (len > 0) snprintf(out, out_sz, "%.*s", len, start);
    }

    free(cmd); free(code); free(query); free(path);
    free(url); free(pattern); free(expr);

    /* Replace newlines with spaces for single-line display */
    for (char *c = out; *c; c++) {
        if (*c == '\n' || *c == '\r') *c = ' ';
    }
}

/* Print combined tool indicator — powerline pill in truecolor, fallback ⚡ */
static void print_tool_start_line(const char *name, const char *input_json) {
    tui_tool_type_t tt = tui_classify_tool(name);
    const char *tc = tui_tool_color(tt);
    tui_rgb_t rgb = tui_tool_rgb(tt);
    char preview[256];
    extract_tool_preview(name, input_json, preview, sizeof(preview));

    bool use_powerline = tui_detect_color_level() >= TUI_COLOR_256;
    const tui_glyphs_t *gl = tui_glyph();

    if (use_powerline && gl->pl_right[0]) {
        /* Powerline pill:  ⚡ name ▶ args */
        int r = (int)(rgb.r * 0.65), g2 = (int)(rgb.g * 0.65), b = (int)(rgb.b * 0.65);
        fprintf(stderr, "  \033[48;2;%d;%d;%dm\033[38;2;255;255;255m %s %s \033[0m",
                r, g2, b, gl->icon_lightning, name);
        fprintf(stderr, "\033[38;2;%d;%d;%dm%s%s",
                r, g2, b, gl->pl_right, TUI_RESET);
        if (preview[0])
            fprintf(stderr, " %s%s%s", TUI_DIM, preview, TUI_RESET);
    } else {
        fprintf(stderr, "  %s%s⚡%s %s%s%s", TUI_BOLD, tc, TUI_RESET,
                TUI_DIM, name, TUI_RESET);
        if (preview[0])
            fprintf(stderr, "  %s%s%s", TUI_DIM, preview, TUI_RESET);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void print_tool_result_ex(const char *name, bool ok, const char *result, double elapsed_ms) {
    const char *nl = strchr(result, '\n');
    int len = nl ? (int)(nl - result) : (int)strlen(result);
    if (len > 100) len = 100;

    tui_tool_type_t tt = tui_classify_tool(name);
    const char *tc = tui_tool_color(tt);

    char elapsed_str[32];
    if (elapsed_ms < 1000.0)
        snprintf(elapsed_str, sizeof(elapsed_str), "%.0fms", elapsed_ms);
    else
        snprintf(elapsed_str, sizeof(elapsed_str), "%.1fs", elapsed_ms / 1000.0);

    /* Size info for large results */
    char size_str[32] = "";
    size_t total = strlen(result);
    if (total > 1024)
        snprintf(size_str, sizeof(size_str), " [%.1fKB]", total / 1024.0);

    fprintf(stderr, "    %s%s%s %s%s%s%s %s(%s)%s %s%.*s%s\n",
            ok ? TUI_GREEN : TUI_RED,
            ok ? tui_glyph()->ok : tui_glyph()->fail,
            TUI_RESET,
            tc, name, TUI_RESET,
            size_str,
            TUI_DIM, elapsed_str, TUI_RESET,
            TUI_DIM, len, result, TUI_RESET);
    baseline_log(ok ? "tool_result" : "tool_error", name, result, NULL);
}

/* Legacy wrapper for any callers that don't have elapsed info */
static void print_tool_result(const char *name, bool ok, const char *result) {
    print_tool_result_ex(name, ok, result, 0.0);
}

static void print_usage_ex(usage_t *u, const char *model, session_state_t *session) {
    bool truecolor = tui_supports_truecolor();
    const tui_glyphs_t *gl = tui_glyph();

    if (truecolor) {
        tui_rgb_t dim_c = tui_hsv_to_rgb(220.0f, 0.10f, 0.45f);
        tui_rgb_t in_c  = tui_hsv_to_rgb(210.0f, 0.30f, 0.60f);
        tui_rgb_t out_c = tui_hsv_to_rgb(160.0f, 0.30f, 0.60f);

        fprintf(stderr, "  \033[38;2;%d;%d;%dm[", dim_c.r, dim_c.g, dim_c.b);
        fprintf(stderr, "\033[38;2;%d;%d;%dmin:%d", in_c.r, in_c.g, in_c.b, u->input_tokens);
        fprintf(stderr, " \033[38;2;%d;%d;%dmout:%d", out_c.r, out_c.g, out_c.b, u->output_tokens);

        if (u->cache_read_input_tokens > 0) {
            tui_rgb_t cr = tui_hsv_to_rgb(120.0f, 0.25f, 0.55f);
            fprintf(stderr, " \033[38;2;%d;%d;%dm%scache-read:%d",
                    cr.r, cr.g, cr.b, gl->icon_lightning, u->cache_read_input_tokens);
        }
        if (u->cache_creation_input_tokens > 0) {
            tui_rgb_t cw = tui_hsv_to_rgb(40.0f, 0.30f, 0.60f);
            fprintf(stderr, " \033[38;2;%d;%d;%dmcache-write:%d",
                    cw.r, cw.g, cw.b, u->cache_creation_input_tokens);
        }

        const model_info_t *mi = model_lookup(model);
        if (mi) {
            double turn_cost = u->input_tokens * mi->input_price / 1e6
                             + u->output_tokens * mi->output_price / 1e6
                             + u->cache_read_input_tokens * mi->cache_read_price / 1e6
                             + u->cache_creation_input_tokens * mi->cache_write_price / 1e6;
            /* Cost color: green cheap → yellow → red expensive */
            float cost_hue = turn_cost < 0.01 ? 120.0f
                           : turn_cost < 0.10 ? 120.0f - (float)((turn_cost - 0.01) / 0.09) * 60.0f
                           : turn_cost < 1.00 ? 60.0f - (float)((turn_cost - 0.10) / 0.90) * 60.0f
                           : 0.0f;
            tui_rgb_t cost_c = tui_hsv_to_rgb(cost_hue, 0.45f, 0.75f);
            fprintf(stderr, " \033[38;2;%d;%d;%dm%s$%.4f",
                    cost_c.r, cost_c.g, cost_c.b, gl->icon_money, turn_cost);
            if (session) {
                double total = session_cost(session);
                tui_rgb_t tot_c = tui_hsv_to_rgb(220.0f, 0.10f, 0.50f);
                fprintf(stderr, " \033[38;2;%d;%d;%dm(total: $%.4f)",
                        tot_c.r, tot_c.g, tot_c.b, total);
            }
        }
        fprintf(stderr, "\033[38;2;%d;%d;%dm]\033[0m\n", dim_c.r, dim_c.g, dim_c.b);
    } else {
        fprintf(stderr, "%s  [in:%d out:%d", TUI_DIM, u->input_tokens, u->output_tokens);
        if (u->cache_read_input_tokens > 0)
            fprintf(stderr, " cache-read:%d", u->cache_read_input_tokens);
        if (u->cache_creation_input_tokens > 0)
            fprintf(stderr, " cache-write:%d", u->cache_creation_input_tokens);
        const model_info_t *mi = model_lookup(model);
        if (mi) {
            double turn_cost = u->input_tokens * mi->input_price / 1e6
                             + u->output_tokens * mi->output_price / 1e6
                             + u->cache_read_input_tokens * mi->cache_read_price / 1e6
                             + u->cache_creation_input_tokens * mi->cache_write_price / 1e6;
            fprintf(stderr, " $%.4f", turn_cost);
            if (session) {
                double total = session_cost(session);
                fprintf(stderr, " (total: $%.4f)", total);
            }
        }
        fprintf(stderr, "]%s\n", TUI_RESET);
    }
}

/* ── Read input line (readline or fgets) ───────────────────────────────── */

static char *read_input_line_prompt(char *buf, size_t buf_sz, const char *prompt) {
    const char *p = prompt ? prompt : "\033[1m\033[95m\xe2\x9d\xaf\033[0m ";
#ifdef HAVE_READLINE
    char *line = readline(p);
    if (!line) return NULL;
    if (line[0]) add_history(line);
    snprintf(buf, buf_sz, "%s", line);
    free(line);
    return buf;
#else
    fprintf(stderr, "%s", p);
    fflush(stderr);
    if (!fgets(buf, (int)buf_sz, stdin)) {
        if (ferror(stdin) && errno == EINTR) {
            clearerr(stdin);
            return buf; /* will be empty, caller handles */
        }
        return NULL;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] != '\n') {
        int ch;
        while ((ch = fgetc(stdin)) != EOF && ch != '\n')
            ;
    }
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    return buf;
#endif
}

/* ── Main agent loop ───────────────────────────────────────────────────── */

void agent_run(const char *api_key, const char *model) {
    /* Register terminal reset FIRST — ensures scroll region, bracketed paste,
       and SGR attrs are cleaned up on ANY exit path through exit(). */
    atexit(terminal_reset_atexit);

    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, NULL);

    struct sigaction sa_tstp;
    memset(&sa_tstp, 0, sizeof(sa_tstp));
    sa_tstp.sa_handler = sigtstp_handler;
    sigemptyset(&sa_tstp.sa_mask);
    sigaction(SIGTSTP, &sa_tstp, NULL);

    /* Auto-save on SIGTERM/SIGHUP */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_autosave;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGHUP, &sa_term, NULL);

    tools_init();
    dsco_locks_init(&g_locks);
    md_init(&s_md, stderr);

    /* Initialize 40 UI features */
    tui_features_init(&g_features);
    g_tui_features = &g_features;
    tui_cadence_init(&s_cadence, &s_md);
    tui_word_counter_init(&s_word_counter);
    tui_thinking_init(&s_thinking);
    tui_flame_init(&s_flame);
    tui_dag_init(&s_dag);
    tui_branch_init(&s_branch);
    tui_citation_init(&s_citations);
    tui_throughput_init(&s_throughput);
    tui_ghost_init(&s_ghost);
    memset(&s_last_latency, 0, sizeof(s_last_latency));
    /* F29: Adaptive theme detection */
    tui_apply_theme(tui_detect_theme());

    /* Initialize MCP servers and register discovered tools */
    int mcp_tools = mcp_init(&g_mcp);
    if (mcp_tools > 0) {
        mcp_register_discovered_tools(&g_mcp);
    }

    /* Rate limiter: 5 requests/second burst, 1/second sustained */
    rate_limiter_t rate_limiter;
    rate_limiter_init(&rate_limiter, 5.0, 1.0);

    /* Per-tool metrics */
    tool_metrics_t tool_metrics;
    tool_metrics_init(&tool_metrics);

    /* Tool result cache */
    tool_cache_t tool_cache;
    tool_cache_init(&tool_cache);

#ifdef HAVE_READLINE
    rl_attempted_completion_function = command_completion;
    rl_basic_word_break_characters = " \t\n";
#endif

    conversation_t conv;
    conv_init(&conv);
    g_autosave_conv = &conv;
    atexit(exit_autosave_handler);

    /* Session state */
    session_state_t session;
    session_state_init(&session, model);
    g_autosave_session = &session;

    /* Initialize provider based on model */
    ensure_provider(&session, api_key);

    char input_buf[MAX_INPUT_LINE];
    int last_input_tokens = 0;

    int tool_count;
    tools_get_all(&tool_count);
    tool_count += g_external_tool_count; /* include MCP tools */

    /* Check for autosave to resume */
    {
        char autosave_path[560];
        const char *home = getenv("HOME");
        if (home) {
            snprintf(autosave_path, sizeof(autosave_path), "%s/.dsco/sessions/_autosave.json", home);
            if (access(autosave_path, R_OK) == 0) {
                fprintf(stderr, "  %sautosave found. /load _autosave to resume%s\n",
                        TUI_DIM, TUI_RESET);
            }
        }
    }

    /* Welcome banner */
    tui_welcome(session.model, tool_count, DSCO_VERSION);

    /* Enhanced startup info */
    {
        /* Active feature count */
        int active_features = 0;
        const bool *flags = (const bool *)&g_features;
        for (int fi = 0; fi < TUI_FEATURE_COUNT; fi++)
            if (flags[fi]) active_features++;

        fprintf(stderr, "  %s%d/%d features active%s · %strust: %s%s",
                TUI_DIM, active_features, TUI_FEATURE_COUNT, TUI_RESET,
                TUI_DIM, session_trust_tier_to_string(session.trust_tier), TUI_RESET);

        /* Git branch on startup (F19) */
        if (g_features.branch_indicator) {
            FILE *gf = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
            if (gf) {
                char branch[128] = "";
                if (fgets(branch, sizeof(branch), gf)) {
                    size_t bl = strlen(branch);
                    if (bl > 0 && branch[bl-1] == '\n') branch[bl-1] = '\0';
                    if (branch[0])
                        fprintf(stderr, " · %s%s %s%s", TUI_BMAGENTA,
                                tui_glyph()->icon_git ? tui_glyph()->icon_git : "",
                                branch, TUI_RESET);
                }
                pclose(gf);
            }
        }
        fprintf(stderr, "\n");
    }

    /* Initialize status bar */
    tui_status_bar_t status_bar;
    tui_status_bar_init(&status_bar, session.model);
    /* F31: Enable status bar clock */
    tui_status_bar_set_clock(&status_bar, g_features.status_clock);

    /* SIGWINCH handler for terminal resize */
    g_winch_sb = &status_bar;
    struct sigaction sa_winch;
    memset(&sa_winch, 0, sizeof(sa_winch));
    sa_winch.sa_handler = sigwinch_handler;
    sigemptyset(&sa_winch.sa_mask);
    sa_winch.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa_winch, NULL);

    fprintf(stderr, "  %stype your request, or 'quit' to exit%s %s\n\n", TUI_DIM, TUI_RESET, tui_glyph()->sparkle);

    while (1) {
        g_interrupted = 0;
        output_guard_reset();

        /* Build dynamic prompt: [turn N] model · $cost · context% ▸ */
        char dyn_prompt[256];
        {
            double cost = session_cost(&session);
            int ctx_used = session.total_input_tokens + session.total_output_tokens;
            int ctx_max = session.context_window > 0 ? session.context_window : CONTEXT_WINDOW_TOKENS;
            double ctx_pct = ctx_max > 0 ? 100.0 * ctx_used / ctx_max : 0;
            const char *ctx_color = ctx_pct < 60 ? TUI_GREEN : (ctx_pct < 85 ? TUI_YELLOW : TUI_RED);

            /* Shorten model name for prompt */
            const model_info_t *mi = model_lookup(session.model);
            const char *short_model = mi ? mi->alias : session.model;

            if (session.turn_count > 0) {
                snprintf(dyn_prompt, sizeof(dyn_prompt),
                    "\001" TUI_DIM "\002" "[" "\001" TUI_RESET "\002"
                    "\001" TUI_BCYAN "\002" "t%d" "\001" TUI_RESET "\002"
                    "\001" TUI_DIM "\002" "] " "\001" TUI_RESET "\002"
                    "\001" TUI_BWHITE "\002" "%s" "\001" TUI_RESET "\002"
                    "\001" TUI_DIM "\002" " · " "\001" TUI_RESET "\002"
                    "\001" TUI_GREEN "\002" "$%.2f" "\001" TUI_RESET "\002"
                    "\001" TUI_DIM "\002" " · " "\001" TUI_RESET "\002"
                    "\001%s\002" "%.0f%%" "\001" TUI_RESET "\002"
                    "\001" TUI_DIM "\002" " ▸" "\001" TUI_RESET "\002" " ",
                    session.turn_count, short_model, cost, ctx_color, ctx_pct);
            } else {
                snprintf(dyn_prompt, sizeof(dyn_prompt),
                    "\001" TUI_BOLD "\002" "\001" TUI_BMAGENTA "\002"
                    "❯" "\001" TUI_RESET "\002" " ");
            }
        }

        /* ── Synchronize terminal state before input ──────────────────
         * After streaming, cursor position may be undefined.
         * Reset scroll region, flush stderr, then re-establish the
         * bottom panel and place cursor on the input row.  This prevents
         * the input prompt from appearing in the middle of output. */
        fflush(stderr);
        if (!read_input_line_prompt(input_buf, sizeof(input_buf), dyn_prompt)) break;

        size_t len = strlen(input_buf);
        if (len == 0) continue;

        /* Strip bracketed paste markers if present */
        {
            char *ps = strstr(input_buf, "\033[200~");
            if (ps) memmove(ps, ps + 6, strlen(ps + 6) + 1);
            char *pe = strstr(input_buf, "\033[201~");
            if (pe) *pe = '\0';
        }

        /* Detect multi-line paste (newlines in input) */
        {
            int newlines = 0;
            for (const char *p = input_buf; *p; p++) if (*p == '\n') newlines++;
            if (newlines > 0) {
                fprintf(stderr, "  %s[%d lines pasted]%s\n", TUI_DIM, newlines + 1, TUI_RESET);
            }
        }

        if (strcmp(input_buf, "quit") == 0 || strcmp(input_buf, "exit") == 0) break;

        /* ── Slash commands ────────────────────────────────────────────── */

        if (strcmp(input_buf, "/clear") == 0) {
            conv_free(&conv);
            conv_init(&conv);
            session.total_input_tokens = 0;
            session.total_output_tokens = 0;
            session.total_cache_read_tokens = 0;
            session.total_cache_write_tokens = 0;
            session.turn_count = 0;
            tui_success("conversation cleared");
            baseline_log("command", "/clear", NULL, NULL);
            continue;
        }
        if (strncmp(input_buf, "/model", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                /* Show current model */
                fprintf(stderr, "  %smodel:%s %s\n", TUI_DIM, TUI_RESET, session.model);
                fprintf(stderr, "  %savailable:%s", TUI_DIM, TUI_RESET);
                for (int i = 0; MODEL_REGISTRY[i].alias; i++)
                    fprintf(stderr, " %s", MODEL_REGISTRY[i].alias);
                fprintf(stderr, "\n");
            } else {
                const char *resolved = model_resolve_alias(arg);
                snprintf(session.model, sizeof(session.model), "%s", resolved);
                session.context_window = model_context_window(resolved);
                const model_info_t *mi = model_lookup(resolved);
                char msg[256];
                snprintf(msg, sizeof(msg), "model switched to %s (ctx: %dk)",
                         resolved, session.context_window / 1000);
                tui_success(msg);
                /* Update thinking gate */
                if (mi && !mi->supports_thinking) {
                    fprintf(stderr, "  %snote: adaptive thinking not available for this model%s\n",
                            TUI_DIM, TUI_RESET);
                }
            }
            baseline_log("command", "/model", session.model, NULL);
            continue;
        }
        if (strncmp(input_buf, "/effort", 7) == 0) {
            const char *arg = input_buf + 7;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                fprintf(stderr, "  %seffort:%s %s  (options: low, medium, high)\n",
                        TUI_DIM, TUI_RESET, session.effort);
            } else if (strcmp(arg, "low") == 0 || strcmp(arg, "medium") == 0 ||
                       strcmp(arg, "high") == 0) {
                snprintf(session.effort, sizeof(session.effort), "%s", arg);
                char msg[64];
                snprintf(msg, sizeof(msg), "effort set to %s", arg);
                tui_success(msg);
            } else {
                tui_error("effort must be: low, medium, or high");
            }
            baseline_log("command", "/effort", session.effort, NULL);
            continue;
        }
        if (strcmp(input_buf, "/cost") == 0) {
            print_cost(&session);
            baseline_log("command", "/cost", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/context") == 0) {
            print_context(&session, last_input_tokens);
            /* F15: Context pressure gauge */
            tui_context_gauge(session.total_input_tokens + session.total_output_tokens,
                              session.context_window, 0);
            baseline_log("command", "/context", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/compact") == 0) {
            /* Manual compaction: trim aggressively */
            conv_trim_old_results(&conv, 4, 128);
            char msg[128];
            snprintf(msg, sizeof(msg), "conversation compacted (%d messages remain)", conv.count);
            tui_success(msg);
            baseline_log("command", "/compact", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/version") == 0) {
            fprintf(stderr, "  dsco v%s (built %s, %s)\n", DSCO_VERSION, BUILD_DATE, GIT_HASH);
            continue;
        }
        /* /force — tool_choice control */
        if (strncmp(input_buf, "/force", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                if (session.tool_choice[0]) {
                    fprintf(stderr, "  %stool_choice:%s %s\n", TUI_DIM, TUI_RESET, session.tool_choice);
                } else {
                    fprintf(stderr, "  %stool_choice:%s auto (default)\n", TUI_DIM, TUI_RESET);
                }
                fprintf(stderr, "  %susage: /force <tool_name> | /force any | /force none | /force auto%s\n",
                        TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "auto") == 0) {
                session.tool_choice[0] = '\0';
                tui_success("tool_choice reset to auto");
            } else if (strcmp(arg, "any") == 0 || strcmp(arg, "none") == 0) {
                snprintf(session.tool_choice, sizeof(session.tool_choice), "%s", arg);
                char msg[64];
                snprintf(msg, sizeof(msg), "tool_choice set to %s", arg);
                tui_success(msg);
            } else {
                snprintf(session.tool_choice, sizeof(session.tool_choice), "tool:%s", arg);
                char msg[160];
                snprintf(msg, sizeof(msg), "next call will force tool: %s (resets to auto after)", arg);
                tui_success(msg);
            }
            baseline_log("command", "/force", session.tool_choice, NULL);
            continue;
        }
        /* /prefill — seed assistant response */
        if (strncmp(input_buf, "/prefill", 8) == 0) {
            const char *arg = input_buf + 8;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                if (session.prefill[0]) {
                    fprintf(stderr, "  %sprefill:%s %s\n", TUI_DIM, TUI_RESET, session.prefill);
                } else {
                    fprintf(stderr, "  %sno prefill set%s\n", TUI_DIM, TUI_RESET);
                }
                fprintf(stderr, "  %susage: /prefill { (for JSON) or /prefill <text>%s\n",
                        TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "clear") == 0) {
                session.prefill[0] = '\0';
                tui_success("prefill cleared");
            } else {
                snprintf(session.prefill, sizeof(session.prefill), "%s", arg);
                char msg[128];
                snprintf(msg, sizeof(msg), "next response will start with: %s", session.prefill);
                tui_success(msg);
            }
            continue;
        }
        /* /json — shortcut to force JSON output via prefill */
        if (strcmp(input_buf, "/json") == 0) {
            snprintf(session.prefill, sizeof(session.prefill), "{");
            tui_success("JSON mode: next response will be JSON (prefill=\"{\")");
            continue;
        }
        /* /web — toggle server-side web search */
        if (strcmp(input_buf, "/web") == 0 || strcmp(input_buf, "/web on") == 0 ||
            strcmp(input_buf, "/web off") == 0) {
            if (strcmp(input_buf, "/web on") == 0) session.web_search = true;
            else if (strcmp(input_buf, "/web off") == 0) session.web_search = false;
            else session.web_search = !session.web_search;
            char msg[64];
            snprintf(msg, sizeof(msg), "web search %s", session.web_search ? "enabled" : "disabled");
            tui_success(msg);
            baseline_log("command", "/web", session.web_search ? "on" : "off", NULL);
            continue;
        }
        /* /code — toggle server-side code execution */
        if (strcmp(input_buf, "/code") == 0 || strcmp(input_buf, "/code on") == 0 ||
            strcmp(input_buf, "/code off") == 0) {
            if (strcmp(input_buf, "/code on") == 0) session.code_execution = true;
            else if (strcmp(input_buf, "/code off") == 0) session.code_execution = false;
            else session.code_execution = !session.code_execution;
            char msg[64];
            snprintf(msg, sizeof(msg), "code execution %s", session.code_execution ? "enabled" : "disabled");
            tui_success(msg);
            baseline_log("command", "/code", session.code_execution ? "on" : "off", NULL);
            continue;
        }
        if (strncmp(input_buf, "/save", 5) == 0) {
            const char *name = input_buf + 5;
            while (*name == ' ') name++;
            if (*name == '\0') name = "default";

            char dir_path[512];
            const char *home = getenv("HOME");
            if (!home) home = "/tmp";
            snprintf(dir_path, sizeof(dir_path), "%s/.dsco/sessions", home);
            mkdir(dir_path, 0755);

            char save_path[1024];
            snprintf(save_path, sizeof(save_path), "%s/%s.json", dir_path, name);

            if (conv_save_ex(&conv, &session, save_path)) {
                char msg[1100];
                snprintf(msg, sizeof(msg), "session saved to %s (%d messages)", save_path, conv.count);
                tui_success(msg);
                baseline_log("command", "/save", save_path, NULL);
            } else {
                tui_error("failed to save session");
                baseline_log("error", "/save", "save failed", NULL);
            }
            continue;
        }
        if (strncmp(input_buf, "/load", 5) == 0) {
            const char *name = input_buf + 5;
            while (*name == ' ') name++;
            if (*name == '\0') name = "default";

            char load_path[1024];
            const char *home = getenv("HOME");
            if (!home) home = "/tmp";
            snprintf(load_path, sizeof(load_path), "%s/.dsco/sessions/%s.json", home, name);

            if (conv_load_ex(&conv, &session, load_path)) {
                char msg[1100];
                snprintf(msg, sizeof(msg), "session loaded from %s (%d messages)", load_path, conv.count);
                tui_success(msg);
                /* F18: Session diff summary */
                {
                    int tc = 0, est = 0;
                    for (int i = 0; i < conv.count; i++) {
                        for (int j = 0; j < conv.msgs[i].content_count; j++) {
                            if (conv.msgs[i].content[j].tool_name) tc++;
                            if (conv.msgs[i].content[j].text)
                                est += tui_estimate_tokens(conv.msgs[i].content[j].text);
                        }
                    }
                    tui_session_diff(conv.count, tc, est, session.model);
                }
                baseline_log("command", "/load", load_path, NULL);
            } else {
                char msg[1100];
                snprintf(msg, sizeof(msg), "failed to load session '%s'", name);
                tui_error(msg);
                baseline_log("error", "/load", "load failed", NULL);
            }
            continue;
        }
        if (strcmp(input_buf, "/sessions") == 0) {
            char dir_path[512];
            const char *home = getenv("HOME");
            if (!home) home = "/tmp";
            snprintf(dir_path, sizeof(dir_path), "%s/.dsco/sessions", home);

            DIR *d = opendir(dir_path);
            if (!d) {
                tui_info("no saved sessions");
            } else {
                fprintf(stderr, "\n");
                tui_header("Saved Sessions", TUI_BCYAN);
                struct dirent *ent;
                int count = 0;
                while ((ent = readdir(d)) != NULL) {
                    size_t nlen = strlen(ent->d_name);
                    if (nlen > 5 && strcmp(ent->d_name + nlen - 5, ".json") == 0) {
                        char display[256];
                        snprintf(display, sizeof(display), "%.*s", (int)(nlen - 5), ent->d_name);
                        fprintf(stderr, "    %s%s%s\n", TUI_CYAN, display, TUI_RESET);
                        count++;
                    }
                }
                closedir(d);
                if (count == 0) tui_info("no saved sessions");
                else fprintf(stderr, "\n");
            }
            baseline_log("command", "/sessions", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/setup") == 0 || strcmp(input_buf, "/setup --force") == 0) {
            bool force = (strcmp(input_buf, "/setup --force") == 0);
            char summary[768];
            int discovered = dsco_setup_autopopulate(force, true, summary, sizeof(summary));
            if (discovered < 0) {
                tui_error(summary);
                baseline_log("setup", "setup_failed", summary, NULL);
            } else {
                tui_success(summary);
                baseline_log("setup", force ? "setup_force" : "setup", summary, NULL);
            }
            continue;
        }
        if (strcmp(input_buf, "/setup report") == 0) {
            char report[32768];
            if (dsco_setup_report(report, sizeof(report)) < 0) {
                tui_error("setup report failed");
                baseline_log("setup", "setup_report_failed", NULL, NULL);
            } else {
                fprintf(stderr, "\n%s\n", report);
                baseline_log("setup", "setup_report", dsco_setup_env_path(), NULL);
            }
            continue;
        }
        if (strcmp(input_buf, "/tools") == 0) {
            int count;
            const tool_def_t *tools = tools_get_all(&count);
            fprintf(stderr, "\n");
            tui_header("Tools", TUI_BCYAN);

            const char *last_prefix = "";
            for (int i = 0; i < count; i++) {
                const char *name = tools[i].name;
                const char *category = "misc";
                if (strstr(name, "file") || strstr(name, "read") || strstr(name, "write") ||
                    strstr(name, "edit") || strstr(name, "append") || strstr(name, "mkdir") ||
                    strstr(name, "tree") || strstr(name, "wc") || strstr(name, "head") ||
                    strstr(name, "tail") || strstr(name, "symlink") || strstr(name, "page") ||
                    strcmp(name, "list_directory") == 0 || strcmp(name, "find_files") == 0 ||
                    strcmp(name, "grep_files") == 0 || strcmp(name, "chmod") == 0 ||
                    strcmp(name, "move_file") == 0 || strcmp(name, "copy_file") == 0 ||
                    strcmp(name, "delete_file") == 0 || strcmp(name, "file_info") == 0)
                    category = "file";
                else if (strstr(name, "git"))   category = "git";
                else if (strstr(name, "agent") || strstr(name, "swarm") || strstr(name, "spawn"))
                    category = "swarm";
                else if (strstr(name, "self_") || strstr(name, "inspect") ||
                         strstr(name, "call_graph") || strstr(name, "dependency"))
                    category = "ast";
                else if (strstr(name, "http") || strstr(name, "curl") || strstr(name, "dns") ||
                         strstr(name, "ping") || strstr(name, "port") || strstr(name, "net") ||
                         strstr(name, "cert") || strstr(name, "whois") || strstr(name, "download") ||
                         strstr(name, "upload") || strstr(name, "websocket") || strstr(name, "traceroute") ||
                         strcmp(name, "market_quote") == 0)
                    category = "network";
                else if (strstr(name, "docker")) category = "docker";
                else if (strstr(name, "ssh") || strstr(name, "scp")) category = "remote";
                else if (strstr(name, "compile") || strstr(name, "run_") ||
                         strcmp(name, "bash") == 0) category = "exec";

                if (strcmp(category, last_prefix) != 0) {
                    last_prefix = category;
                    const char *cat_color = TUI_DIM;
                    if (strcmp(category, "swarm") == 0) cat_color = TUI_BYELLOW;
                    else if (strcmp(category, "ast") == 0) cat_color = TUI_BMAGENTA;
                    fprintf(stderr, "  %s%s%s\n", cat_color, category, TUI_RESET);
                }
                /* Show call count from metrics if any */
                const tool_metric_t *tm = tool_metrics_get(&tool_metrics, tools[i].name);
                if (tm && tm->calls > 0) {
                    fprintf(stderr, "    %s%-22s%s %s%3dx%s %s%s%s\n",
                            TUI_CYAN, tools[i].name, TUI_RESET,
                            TUI_BWHITE, tm->calls, TUI_RESET,
                            TUI_DIM, tools[i].description, TUI_RESET);
                } else {
                    fprintf(stderr, "    %s%-22s%s     %s%s%s\n",
                            TUI_CYAN, tools[i].name, TUI_RESET,
                            TUI_DIM, tools[i].description, TUI_RESET);
                }
            }
            /* List MCP/external tools grouped by server */
            if (g_external_tool_count > 0) {
                fprintf(stderr, "  %smcp%s\n", TUI_BYELLOW, TUI_RESET);
                for (int i = 0; i < g_external_tool_count; i++) {
                    const tool_metric_t *tm = tool_metrics_get(&tool_metrics, g_external_tools[i].name);
                    if (tm && tm->calls > 0) {
                        fprintf(stderr, "    %s%-22s%s %s%3dx%s %s%s%s\n",
                                TUI_CYAN, g_external_tools[i].name, TUI_RESET,
                                TUI_BWHITE, tm->calls, TUI_RESET,
                                TUI_DIM, g_external_tools[i].description, TUI_RESET);
                    } else {
                        fprintf(stderr, "    %s%-22s%s     %s%s%s\n",
                                TUI_CYAN, g_external_tools[i].name, TUI_RESET,
                                TUI_DIM, g_external_tools[i].description, TUI_RESET);
                    }
                }
            }
            fprintf(stderr, "\n  %s%d builtin + %d MCP + web_search + code_execution%s\n\n",
                    TUI_DIM, count, g_external_tool_count, TUI_RESET);
            baseline_log("command", "/tools", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/help") == 0) {
            fprintf(stderr, "\n");
            tui_header("Commands", TUI_BCYAN);
            fprintf(stderr, "  %s/clear%s       reset conversation\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/model [name]%s switch model (opus/sonnet/haiku)\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/effort [lvl]%s set effort (low/medium/high)\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/cost%s        show session cost\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/context%s     show token usage\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/compact%s     trim conversation history\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/save [name]%s save session\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/load [name]%s load session\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/sessions%s   list saved sessions\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/setup%s      save API keys to %s\n", TUI_CYAN, TUI_RESET, dsco_setup_env_path());
            fprintf(stderr, "  %s/force [tool]%s force next tool call\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/web%s        toggle web search\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/code%s       toggle code execution\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/budget [$]%s  set cost budget\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/thinking [auto|>=1024]%s set thinking budget (in tokens)\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/trust [tier]%s set trust tier (trusted/standard/untrusted)\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/status%s     full session status\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/tools%s      list all tools\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/plugins%s    list loaded plugins\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/plugins validate [manifest] [lock]%s validate plugin manifest + lockfile\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/mcp%s        show MCP servers + tools\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/provider%s   show/detect API provider\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/version%s    show version + build info\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/features%s   toggle UI features (F1-F40)\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/perf%s       latency waterfall + throughput\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/minimap%s    conversation minimap\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/dashboard%s  rich session overview panel\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/top%s        tool leaderboard by calls\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/flame%s      flame timeline for tool executions\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/help%s       show this help\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %squit%s        exit dsco\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "\n");
            tui_header("Images", TUI_BMAGENTA);
            fprintf(stderr, "  %sDrag & drop image files into the prompt:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"describe /path/to/screenshot.png\"%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %sSupported: .png .jpg .jpeg .gif .webp .bmp .tif .tiff .heic .heif .avif%s\n\n", TUI_DIM, TUI_RESET);
            tui_header("Swarm", TUI_BYELLOW);
            fprintf(stderr, "  %sSpawn sub-agents for parallel work:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"spawn 3 agents to build a REST API, frontend, and tests\"%s\n\n", TUI_DIM, TUI_RESET);
            tui_header("AST Introspection", TUI_BMAGENTA);
            fprintf(stderr, "  %sAnalyze C codebases at the AST level:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"inspect your own source code and find the most complex functions\"%s\n\n", TUI_DIM, TUI_RESET);
            tui_header("Crypto", TUI_BRED);
            fprintf(stderr, "  %sSHA-256, MD5, HMAC, UUID, HKDF, JWT — all pure C:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"hash this file with SHA-256\" or \"generate 5 UUIDs\"%s\n\n", TUI_DIM, TUI_RESET);
            tui_header("Pipeline", TUI_BGREEN);
            fprintf(stderr, "  %sCoroutine-powered streaming data transforms:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"pipe this log through filter:error|sort|uniq|head:20\"%s\n\n", TUI_DIM, TUI_RESET);
            tui_header("Eval", TUI_BCYAN);
            fprintf(stderr, "  %sMath engine with 50+ functions:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"eval sqrt(2)^3 + sin(pi/4)\" or \"big_factorial 100\"%s\n\n", TUI_DIM, TUI_RESET);
            baseline_log("command", "/help", NULL, NULL);
            continue;
        }
        if (strncmp(input_buf, "/plugins validate", 17) == 0) {
            const char *arg = input_buf + 17;
            while (*arg == ' ') arg++;

            char *copy = safe_strdup(arg);
            char *manifest_path = NULL;
            char *lock_path = NULL;
            if (*copy) {
                manifest_path = strtok(copy, " \t");
                lock_path = strtok(NULL, " \t");
            }

            char out[2048];
            bool ok = plugin_validate_manifest_and_lock(manifest_path, lock_path, out, sizeof(out));
            if (ok) tui_success(out);
            else tui_error(out);
            baseline_log("command", "/plugins validate", out, NULL);
            free(copy);
            continue;
        }
        if (strcmp(input_buf, "/plugins") == 0) {
            char buf[4096];
            plugin_list(&g_plugins, buf, sizeof(buf));
            fprintf(stderr, "\n%s\n", buf);
            baseline_log("command", "/plugins", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/mcp") == 0 || strcmp(input_buf, "/mcp reload") == 0) {
            if (strcmp(input_buf, "/mcp reload") == 0) {
                mcp_shutdown(&g_mcp);
                g_external_tool_count = 0;
                int n = mcp_init(&g_mcp);
                if (n > 0) mcp_register_discovered_tools(&g_mcp);
                char msg[128];
                snprintf(msg, sizeof(msg), "MCP reloaded: %d tools from %d servers",
                         g_mcp.tool_count, g_mcp.server_count);
                tui_success(msg);
            } else {
                fprintf(stderr, "\n");
                tui_header("MCP Servers", TUI_BCYAN);
                if (g_mcp.server_count == 0) {
                    fprintf(stderr, "  %sno MCP servers configured%s\n", TUI_DIM, TUI_RESET);
                    fprintf(stderr, "  %screate ~/.dsco/mcp.json to add servers%s\n\n", TUI_DIM, TUI_RESET);
                } else {
                    for (int i = 0; i < g_mcp.server_count; i++) {
                        fprintf(stderr, "  %s%s%s  %s%s%s  pid:%d  %s%s%s\n",
                                TUI_CYAN, g_mcp.servers[i].name, TUI_RESET,
                                TUI_DIM, g_mcp.servers[i].command, TUI_RESET,
                                g_mcp.servers[i].pid,
                                g_mcp.servers[i].initialized ? TUI_GREEN : TUI_RED,
                                g_mcp.servers[i].initialized ? "connected" : "disconnected",
                                TUI_RESET);
                    }
                    fprintf(stderr, "\n  %s%d MCP tools registered%s\n\n",
                            TUI_DIM, g_mcp.tool_count, TUI_RESET);
                    /* List MCP tools */
                    for (int i = 0; i < g_mcp.tool_count; i++) {
                        fprintf(stderr, "    %s%-30s%s %s%s%s\n",
                                TUI_CYAN, g_mcp.tools[i].name, TUI_RESET,
                                TUI_DIM, g_mcp.tools[i].description, TUI_RESET);
                    }
                    fprintf(stderr, "\n");
                }
            }
            baseline_log("command", "/mcp", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/provider") == 0) {
            fprintf(stderr, "  %sprovider:%s %s\n", TUI_DIM, TUI_RESET,
                    g_provider ? g_provider->name : "none");
            fprintf(stderr, "  %sdetected from:%s model=%s\n", TUI_DIM, TUI_RESET, session.model);
            fprintf(stderr, "  %savailable:%s anthropic, openai\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %snote: provider auto-detected from /model selection%s\n\n",
                    TUI_DIM, TUI_RESET);
            continue;
        }
        if (strncmp(input_buf, "/budget", 7) == 0) {
            const char *arg = input_buf + 7;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                double cost = session_cost(&session);
                if (g_cost_budget > 0) {
                    fprintf(stderr, "  %scost:%s $%.4f / $%.4f (%.0f%%)\n", TUI_DIM, TUI_RESET,
                            cost, g_cost_budget, 100.0 * cost / g_cost_budget);
                } else {
                    fprintf(stderr, "  %scost:%s $%.4f (no budget set)\n", TUI_DIM, TUI_RESET, cost);
                }
                fprintf(stderr, "  %susage: /budget <dollars> or /budget off%s\n", TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "off") == 0 || strcmp(arg, "none") == 0) {
                g_cost_budget = 0;
                tui_success("cost budget disabled");
            } else {
                double budget = atof(arg);
                if (budget > 0) {
                    g_cost_budget = budget;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "cost budget set to $%.2f", budget);
                    tui_success(msg);
                } else {
                    tui_error("budget must be a positive number");
                }
            }
            baseline_log("command", "/budget", NULL, NULL);
            continue;
        }
        if (strncmp(input_buf, "/trust", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                fprintf(stderr, "  %strust tier:%s %s\n", TUI_DIM, TUI_RESET,
                        session_trust_tier_to_string(session.trust_tier));
                fprintf(stderr, "  %susage: /trust trusted|standard|untrusted%s\n",
                        TUI_DIM, TUI_RESET);
            } else {
                bool ok = false;
                dsco_trust_tier_t tier = session_trust_tier_from_string(arg, &ok);
                if (!ok) {
                    tui_error("invalid trust tier (use trusted, standard, or untrusted)");
                } else {
                    session.trust_tier = tier;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "trust tier set to %s",
                             session_trust_tier_to_string(session.trust_tier));
                    tui_success(msg);
                    baseline_log("security", "trust_tier_set",
                                 session_trust_tier_to_string(session.trust_tier), NULL);
                }
            }
            continue;
        }
        if (strcmp(input_buf, "/status") == 0) {
            double cost = session_cost(&session);
            double avg_ttft = session.telemetry_samples > 0
                ? session.total_ttft_ms / session.telemetry_samples : 0;
            double avg_stream = session.telemetry_samples > 0
                ? session.total_stream_ms / session.telemetry_samples : 0;
            fprintf(stderr, "\n");
            tui_header("Session Status", TUI_BCYAN);
            fprintf(stderr, "  %sModel:%s       %s\n", TUI_DIM, TUI_RESET, session.model);
            fprintf(stderr, "  %sProvider:%s    %s\n", TUI_DIM, TUI_RESET,
                    g_provider ? g_provider->name : "none");
            fprintf(stderr, "  %sEffort:%s      %s\n", TUI_DIM, TUI_RESET, session.effort);
            fprintf(stderr, "  %sTrust tier:%s  %s\n", TUI_DIM, TUI_RESET,
                    session_trust_tier_to_string(session.trust_tier));
            if (session.temperature >= 0)
                fprintf(stderr, "  %sTemperature:%s %.2f\n", TUI_DIM, TUI_RESET, session.temperature);
            if (session.thinking_budget > 0)
                fprintf(stderr, "  %sThinking:%s   %d tokens\n", TUI_DIM, TUI_RESET, session.thinking_budget);
            fprintf(stderr, "  %sTurns:%s       %d\n", TUI_DIM, TUI_RESET, session.turn_count);
            fprintf(stderr, "  %sCost:%s        $%.4f", TUI_DIM, TUI_RESET, cost);
            if (g_cost_budget > 0)
                fprintf(stderr, " / $%.2f (%.0f%%)", g_cost_budget, 100.0 * cost / g_cost_budget);
            fprintf(stderr, "\n");
            fprintf(stderr, "  %sMessages:%s    %d\n", TUI_DIM, TUI_RESET, conv.count);
            fprintf(stderr, "  %sContext:%s     %d / %d tokens\n", TUI_DIM, TUI_RESET,
                    last_input_tokens, session.context_window);
            if (session.telemetry_samples > 0) {
                fprintf(stderr, "  %sAvg TTFT:%s    %.0fms\n", TUI_DIM, TUI_RESET, avg_ttft);
                fprintf(stderr, "  %sAvg stream:%s  %.0fms\n", TUI_DIM, TUI_RESET, avg_stream);
            }
            if (session.fallback_count > 0) {
                fprintf(stderr, "  %sFallback:%s    ", TUI_DIM, TUI_RESET);
                for (int fi = 0; fi < session.fallback_count; fi++)
                    fprintf(stderr, "%s%s", fi ? " -> " : "", session.fallback_models[fi]);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "  %sWeb search:%s  %s\n", TUI_DIM, TUI_RESET,
                    session.web_search ? "on" : "off");
            fprintf(stderr, "  %sCode exec:%s   %s\n", TUI_DIM, TUI_RESET,
                    session.code_execution ? "on" : "off");
            fprintf(stderr, "  %sMCP servers:%s %d (%d tools)\n", TUI_DIM, TUI_RESET,
                    g_mcp.server_count, g_mcp.tool_count);
            fprintf(stderr, "  %sCache:%s       %d hits / %d misses\n", TUI_DIM, TUI_RESET,
                    tool_cache.hits, tool_cache.misses);
            fprintf(stderr, "  %sTotal tools:%s %d\n\n", TUI_DIM, TUI_RESET, tool_count);
            continue;
        }
        /* /temp — temperature control */
        if (strncmp(input_buf, "/temp", 5) == 0) {
            const char *arg = input_buf + 5;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                fprintf(stderr, "  %stemperature:%s %s\n", TUI_DIM, TUI_RESET,
                        session.temperature >= 0 ? "custom" : "default");
                if (session.temperature >= 0)
                    fprintf(stderr, "  %svalue:%s %.2f\n", TUI_DIM, TUI_RESET, session.temperature);
                fprintf(stderr, "  %susage: /temp 0.0-2.0 | /temp off%s\n", TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "off") == 0 || strcmp(arg, "default") == 0) {
                session.temperature = -1.0;
                tui_success("temperature reset to default");
            } else {
                double t = atof(arg);
                if (t >= 0.0 && t <= 2.0) {
                    session.temperature = t;
                    char msg[64]; snprintf(msg, sizeof(msg), "temperature set to %.2f", t);
                    tui_success(msg);
                } else {
                    tui_error("temperature must be 0.0-2.0");
                }
            }
            continue;
        }
        /* /thinking — thinking budget control */
        if (strncmp(input_buf, "/thinking", 9) == 0) {
            const char *arg = input_buf + 9;
            while (*arg == ' ') arg++;
            if (*arg == '\0' || strcmp(arg, "auto") == 0 || strcmp(arg, "adaptive") == 0) {
                session.thinking_budget = 0;
                tui_success("thinking set to adaptive (auto)");
            } else {
                int budget = atoi(arg);
                if (budget >= 1024) {
                    session.thinking_budget = budget;
                    char msg[64]; snprintf(msg, sizeof(msg), "thinking budget: %d tokens", budget);
                    tui_success(msg);
                } else {
                    tui_error("thinking budget must be >= 1024 tokens, or 'auto'");
                }
            }
            continue;
        }
        /* /fallback — model fallback chain */
        if (strncmp(input_buf, "/fallback", 9) == 0) {
            const char *arg = input_buf + 9;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                if (session.fallback_count == 0) {
                    fprintf(stderr, "  %sfallback:%s none (single model)\n", TUI_DIM, TUI_RESET);
                } else {
                    fprintf(stderr, "  %sfallback chain:%s ", TUI_DIM, TUI_RESET);
                    for (int fi = 0; fi < session.fallback_count; fi++)
                        fprintf(stderr, "%s%s", fi ? " -> " : "", session.fallback_models[fi]);
                    fprintf(stderr, "\n");
                }
                fprintf(stderr, "  %susage: /fallback opus,sonnet,haiku | /fallback off%s\n", TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "off") == 0 || strcmp(arg, "none") == 0) {
                session.fallback_count = 0;
                tui_success("fallback chain disabled");
            } else {
                session.fallback_count = 0;
                char *copy = safe_strdup(arg);
                char *tok = strtok(copy, ",");
                while (tok && session.fallback_count < 4) {
                    while (*tok == ' ') tok++;
                    const char *resolved = model_resolve_alias(tok);
                    snprintf(session.fallback_models[session.fallback_count],
                             sizeof(session.fallback_models[0]), "%s", resolved);
                    session.fallback_count++;
                    tok = strtok(NULL, ",");
                }
                free(copy);
                char msg[256]; snprintf(msg, sizeof(msg), "fallback chain: %d models", session.fallback_count);
                tui_success(msg);
            }
            continue;
        }
        /* /metrics — per-tool performance metrics */
        if (strcmp(input_buf, "/metrics") == 0) {
            fprintf(stderr, "\n");
            tui_header("Tool Metrics", TUI_BCYAN);
            if (tool_metrics.count == 0) {
                fprintf(stderr, "  %sno tool calls recorded yet%s\n\n", TUI_DIM, TUI_RESET);
            } else {
                fprintf(stderr, "  %s%-22s %6s %5s %5s %4s %8s %8s%s\n", TUI_DIM,
                        "TOOL", "CALLS", "OK", "FAIL", "TMO", "AVG(ms)", "MAX(ms)", TUI_RESET);
                for (int i = 0; i < tool_metrics.count; i++) {
                    tool_metric_t *e = &tool_metrics.entries[i];
                    double avg = e->calls > 0 ? e->total_latency_ms / e->calls : 0;
                    fprintf(stderr, "  %-22s %6d %s%5d%s %s%5d%s %s%4d%s %8.0f %8.0f\n",
                            e->name, e->calls,
                            TUI_GREEN, e->successes, TUI_RESET,
                            e->failures > 0 ? TUI_RED : TUI_DIM, e->failures, TUI_RESET,
                            e->timeouts > 0 ? TUI_YELLOW : TUI_DIM, e->timeouts, TUI_RESET,
                            avg, e->max_latency_ms);
                }
                fprintf(stderr, "\n  %sCache: %d hits / %d misses (%.0f%% hit rate)%s\n\n",
                        TUI_DIM, tool_cache.hits, tool_cache.misses,
                        (tool_cache.hits + tool_cache.misses) > 0
                            ? 100.0 * tool_cache.hits / (tool_cache.hits + tool_cache.misses)
                            : 0.0,
                        TUI_RESET);
            }
            continue;
        }
        /* /telemetry — streaming performance */
        if (strcmp(input_buf, "/telemetry") == 0) {
            fprintf(stderr, "\n");
            tui_header("Streaming Telemetry", TUI_BCYAN);
            if (session.telemetry_samples == 0) {
                fprintf(stderr, "  %sno streaming data yet%s\n\n", TUI_DIM, TUI_RESET);
            } else {
                fprintf(stderr, "  %sSamples:%s    %d\n", TUI_DIM, TUI_RESET, session.telemetry_samples);
                fprintf(stderr, "  %sAvg TTFT:%s   %.0f ms\n", TUI_DIM, TUI_RESET,
                        session.total_ttft_ms / session.telemetry_samples);
                fprintf(stderr, "  %sAvg total:%s  %.0f ms\n", TUI_DIM, TUI_RESET,
                        session.total_stream_ms / session.telemetry_samples);
                fprintf(stderr, "\n");
            }
            continue;
        }
        /* /cache — tool cache management */
        if (strncmp(input_buf, "/cache", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                fprintf(stderr, "  %scache:%s %d entries, %d hits, %d misses\n",
                        TUI_DIM, TUI_RESET, tool_cache.count, tool_cache.hits, tool_cache.misses);
            } else if (strcmp(arg, "clear") == 0) {
                tool_cache_free(&tool_cache);
                tool_cache_init(&tool_cache);
                tui_success("tool cache cleared");
            }
            continue;
        }

        /* /trace — view trace spans */
        if (strncmp(input_buf, "/trace", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                trace_query_recent(10);
            } else {
                /* Treat arg as a trace_id to show waterfall */
                trace_print_waterfall(arg);
            }
            continue;
        }

        /* /features — toggle UI features */
        if (strncmp(input_buf, "/features", 9) == 0) {
            const char *arg = input_buf + 9;
            while (*arg == ' ') arg++;
            if (*arg == '\0') {
                tui_features_list(&g_features);
            } else {
                tui_features_toggle(&g_features, arg);
            }
            continue;
        }

        /* /perf — latency waterfall */
        if (strcmp(input_buf, "/perf") == 0) {
            fprintf(stderr, "\n");
            tui_header("Performance", TUI_BCYAN);
            tui_latency_breakdown_t lb = {
                .dns_ms = s_last_latency.dns_ms,
                .connect_ms = s_last_latency.connect_ms,
                .tls_ms = s_last_latency.tls_ms,
                .ttfb_ms = s_last_latency.ttfb_ms,
                .total_ms = s_last_latency.total_ms,
            };
            tui_latency_waterfall(&lb);
            tui_throughput_render(&s_throughput);
            fprintf(stderr, "\n");
            continue;
        }

        /* /minimap — conversation minimap (F16) */
        if (strcmp(input_buf, "/minimap") == 0) {
            tui_minimap_entry_t entries[256];
            int mc = 0;
            for (int i = 0; i < conv.count && mc < 256; i++) {
                entries[mc].type = conv.msgs[i].role == ROLE_USER ? 'u' : 'a';
                /* Estimate tokens from content */
                int est = 0;
                for (int j = 0; j < conv.msgs[i].content_count; j++) {
                    if (conv.msgs[i].content[j].text)
                        est += tui_estimate_tokens(conv.msgs[i].content[j].text);
                    if (conv.msgs[i].content[j].tool_name) {
                        entries[mc].type = 't';
                        est += 50; /* tool overhead estimate */
                    }
                }
                entries[mc].tokens = est > 0 ? est : 10;
                mc++;
            }
            tui_minimap_render(entries, mc, 0);
            continue;
        }

        /* /dashboard — rich session overview */
        if (strcmp(input_buf, "/dashboard") == 0) {
            fprintf(stderr, "\n");
            tui_header("Dashboard", TUI_BCYAN);
            double cost = session_cost(&session);
            const model_info_t *mi = model_lookup(session.model);
            int ctx_used = session.total_input_tokens + session.total_output_tokens;
            int ctx_max = session.context_window > 0 ? session.context_window : CONTEXT_WINDOW_TOKENS;
            double avg_ttft = session.telemetry_samples > 0
                ? session.total_ttft_ms / session.telemetry_samples : 0;
            double avg_stream = session.telemetry_samples > 0
                ? session.total_stream_ms / session.telemetry_samples : 0;
            double avg_tps = avg_stream > 0 && session.total_output_tokens > 0
                ? (session.total_output_tokens / (double)session.telemetry_samples)
                  / (avg_stream / 1000.0) : 0;

            /* Count total tools from metrics */
            int dash_total_tools = 0;
            for (int ti = 0; ti < tool_metrics.count; ti++)
                dash_total_tools += tool_metrics.entries[ti].calls;

            /* Session stats */
            fprintf(stderr, "  %s┌─ Session ───────────────────────────────────┐%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s│%s  Turns: %s%-6d%s  Tools: %s%-6d%s  Msgs: %s%-4d%s  %s│%s\n",
                    TUI_DIM, TUI_RESET, TUI_BWHITE, session.turn_count, TUI_RESET,
                    TUI_BWHITE, dash_total_tools, TUI_RESET,
                    TUI_BWHITE, conv.count, TUI_RESET, TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s│%s  Model: %s%-20s%s  Trust: %s%-8s%s %s│%s\n",
                    TUI_DIM, TUI_RESET, TUI_BCYAN, mi ? mi->alias : session.model, TUI_RESET,
                    TUI_BYELLOW, session_trust_tier_to_string(session.trust_tier), TUI_RESET,
                    TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s└─────────────────────────────────────────────┘%s\n", TUI_DIM, TUI_RESET);

            /* Cost breakdown */
            fprintf(stderr, "\n  %s┌─ Cost ─────────────────────────────────────┐%s\n", TUI_DIM, TUI_RESET);
            if (mi) {
                double in_cost = session.total_input_tokens * mi->input_price / 1e6;
                double out_cost = session.total_output_tokens * mi->output_price / 1e6;
                double cache_r = session.total_cache_read_tokens * mi->cache_read_price / 1e6;
                double cache_w = session.total_cache_write_tokens * mi->cache_write_price / 1e6;
                fprintf(stderr, "  %s│%s  Input:  %s$%.4f%s (%dk tok)                %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_GREEN, in_cost, TUI_RESET,
                        session.total_input_tokens / 1000, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Output: %s$%.4f%s (%dk tok)                %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_GREEN, out_cost, TUI_RESET,
                        session.total_output_tokens / 1000, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Cache:  %s$%.4f%s read + %s$%.4f%s write    %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BCYAN, cache_r, TUI_RESET,
                        TUI_BCYAN, cache_w, TUI_RESET, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Total:  %s%s$%.4f%s                         %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BOLD, TUI_BGREEN, cost, TUI_RESET,
                        TUI_DIM, TUI_RESET);
            }
            if (g_cost_budget > 0) {
                fprintf(stderr, "  %s│%s  Budget: $%.2f (%.0f%% used)               %s│%s\n",
                        TUI_DIM, TUI_RESET, g_cost_budget, 100.0 * cost / g_cost_budget,
                        TUI_DIM, TUI_RESET);
            }
            fprintf(stderr, "  %s└─────────────────────────────────────────────┘%s\n", TUI_DIM, TUI_RESET);

            /* Context gauge */
            fprintf(stderr, "\n  %sContext:%s ", TUI_DIM, TUI_RESET);
            tui_context_gauge(ctx_used, ctx_max, 40);

            /* Streaming performance */
            if (session.telemetry_samples > 0) {
                fprintf(stderr, "  %s┌─ Streaming ─────────────────────────────────┐%s\n", TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Avg TTFT:  %s%.0fms%s                          %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BCYAN, avg_ttft, TUI_RESET, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Avg tok/s: %s%.0f%s                             %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BCYAN, avg_tps, TUI_RESET, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s└─────────────────────────────────────────────┘%s\n", TUI_DIM, TUI_RESET);
            }

            /* Top tools */
            if (tool_metrics.count > 0) {
                fprintf(stderr, "\n  %sTop Tools:%s\n", TUI_DIM, TUI_RESET);
                /* Find top 5 by call count */
                int indices[5] = {-1,-1,-1,-1,-1};
                for (int t = 0; t < 5 && t < tool_metrics.count; t++) {
                    int best = -1;
                    for (int i = 0; i < tool_metrics.count; i++) {
                        bool skip = false;
                        for (int j = 0; j < t; j++) if (indices[j] == i) skip = true;
                        if (skip) continue;
                        if (best < 0 || tool_metrics.entries[i].calls > tool_metrics.entries[best].calls)
                            best = i;
                    }
                    if (best >= 0) indices[t] = best;
                }
                for (int t = 0; t < 5; t++) {
                    if (indices[t] < 0) break;
                    tool_metric_t *e = &tool_metrics.entries[indices[t]];
                    double avg = e->calls > 0 ? e->total_latency_ms / e->calls : 0;
                    fprintf(stderr, "    %s%d.%s %s%-20s%s %s%d calls%s  avg %s%.0fms%s\n",
                            TUI_DIM, t+1, TUI_RESET,
                            TUI_CYAN, e->name, TUI_RESET,
                            TUI_BWHITE, e->calls, TUI_RESET,
                            TUI_DIM, avg, TUI_RESET);
                }
            }

            /* Cache hit rate */
            int cache_total = tool_cache.hits + tool_cache.misses;
            if (cache_total > 0) {
                fprintf(stderr, "\n  %sCache:%s %d/%d hits (%.0f%%)\n",
                        TUI_DIM, TUI_RESET, tool_cache.hits, cache_total,
                        100.0 * tool_cache.hits / cache_total);
            }

            /* Git branch */
            {
                FILE *gf = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
                if (gf) {
                    char branch[128] = "";
                    if (fgets(branch, sizeof(branch), gf)) {
                        size_t bl = strlen(branch);
                        if (bl > 0 && branch[bl-1] == '\n') branch[bl-1] = '\0';
                        if (branch[0])
                            fprintf(stderr, "  %sGit:%s %s%s%s\n", TUI_DIM, TUI_RESET,
                                    TUI_BMAGENTA, branch, TUI_RESET);
                    }
                    pclose(gf);
                }
            }

            /* Fallback chain */
            if (session.fallback_count > 0) {
                fprintf(stderr, "  %sFallback:%s ", TUI_DIM, TUI_RESET);
                for (int fi = 0; fi < session.fallback_count; fi++)
                    fprintf(stderr, "%s%s%s%s", fi ? " → " : "",
                            TUI_BYELLOW, session.fallback_models[fi], TUI_RESET);
                fprintf(stderr, "\n");
            }

            /* Active features count */
            {
                int active = 0;
                const bool *flags = (const bool *)&g_features;
                for (int fi = 0; fi < TUI_FEATURE_COUNT; fi++)
                    if (flags[fi]) active++;
                fprintf(stderr, "  %sFeatures:%s %d/%d active\n", TUI_DIM, TUI_RESET, active, TUI_FEATURE_COUNT);
            }

            fprintf(stderr, "\n");
            continue;
        }

        /* /top — tool leaderboard */
        if (strcmp(input_buf, "/top") == 0) {
            fprintf(stderr, "\n");
            tui_header("Tool Leaderboard", TUI_BCYAN);
            if (tool_metrics.count == 0) {
                fprintf(stderr, "  %sno tool calls recorded yet%s\n\n", TUI_DIM, TUI_RESET);
            } else {
                /* Sort indices by call count descending */
                int idx[256];
                int n = tool_metrics.count > 256 ? 256 : tool_metrics.count;
                for (int i = 0; i < n; i++) idx[i] = i;
                for (int i = 0; i < n - 1; i++) {
                    for (int j = i + 1; j < n; j++) {
                        if (tool_metrics.entries[idx[j]].calls > tool_metrics.entries[idx[i]].calls) {
                            int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
                        }
                    }
                }
                fprintf(stderr, "  %s%-4s %-22s %6s %7s %8s %8s%s\n", TUI_DIM,
                        "RANK", "TOOL", "CALLS", "OK%", "AVG(ms)", "COST", TUI_RESET);

                const model_info_t *mi_top = model_lookup(session.model);
                for (int i = 0; i < n; i++) {
                    tool_metric_t *e = &tool_metrics.entries[idx[i]];
                    double avg = e->calls > 0 ? e->total_latency_ms / e->calls : 0;
                    double ok_pct = e->calls > 0 ? 100.0 * e->successes / e->calls : 0;
                    /* Estimate tool cost: rough token estimate per call */
                    double est_cost = 0;
                    if (mi_top) est_cost = e->calls * 500.0 * (mi_top->input_price + mi_top->output_price) / 2.0 / 1e6;

                    const char *speed_color = avg < 500 ? TUI_GREEN : (avg < 2000 ? TUI_YELLOW : TUI_RED);
                    const char *ok_color = ok_pct >= 95 ? TUI_GREEN : (ok_pct >= 80 ? TUI_YELLOW : TUI_RED);

                    fprintf(stderr, "  %s%2d.%s  %s%-22s%s %6d %s%6.0f%%%s %s%7.0f%s %s$%.3f%s\n",
                            TUI_DIM, i + 1, TUI_RESET,
                            TUI_CYAN, e->name, TUI_RESET,
                            e->calls,
                            ok_color, ok_pct, TUI_RESET,
                            speed_color, avg, TUI_RESET,
                            TUI_DIM, est_cost, TUI_RESET);
                }
                fprintf(stderr, "\n");
            }
            continue;
        }

        /* /flame — flame timeline for last turn */
        if (strcmp(input_buf, "/flame") == 0) {
            fprintf(stderr, "\n");
            tui_header("Flame Timeline", TUI_BCYAN);
            if (s_flame.count == 0) {
                fprintf(stderr, "  %sno tool executions recorded this session%s\n\n", TUI_DIM, TUI_RESET);
            } else {
                tui_flame_render(&s_flame);
                fprintf(stderr, "\n");
            }
            continue;
        }

        baseline_log("user", "prompt", input_buf, NULL);

        /* F19: Branch detection */
        tui_branch_detect(&s_branch, input_buf);
        tui_branch_push(&s_branch, input_buf);

        /* F21: Ghost suggestion history */
        tui_ghost_push(&s_ghost, input_buf);

        /* F22: Prompt token counter */
        if (g_features.prompt_tokens) {
            int est = tui_estimate_tokens(input_buf);
            int remaining = session.context_window - session.total_input_tokens - session.total_output_tokens;
            tui_prompt_token_display(est, remaining);
        }

        /* Prompt injection detection */
        injection_level_t inj = detect_prompt_injection(input_buf);
        if (inj == INJECTION_HIGH) {
            tui_warning("potential prompt injection detected (high confidence) — input sanitized");
            baseline_log("security", "injection_high", input_buf, NULL);
        } else if (inj == INJECTION_MED) {
            fprintf(stderr, "  %ssecurity: potential injection pattern detected%s\n",
                    TUI_DIM, TUI_RESET);
        }

        /* Check for image URLs (http/https links to images) */
        bool has_url_image = false;
        {
            char *url_start = strstr(input_buf, "http");
            if (url_start) {
                /* Extract the URL (ends at space or end of string) */
                char *url_end = url_start;
                while (*url_end && *url_end != ' ' && *url_end != '\t') url_end++;
                size_t url_len = (size_t)(url_end - url_start);
                char url_buf[4096];
                if (url_len < sizeof(url_buf)) {
                    memcpy(url_buf, url_start, url_len);
                    url_buf[url_len] = '\0';
                    /* Check if URL ends with an image extension */
                    const char *dot = strrchr(url_buf, '.');
                    if (dot && img_media_type_for_ext(dot)) {
                        has_url_image = true;
                        /* Extract surrounding text */
                        char text_before[MAX_INPUT_LINE] = "";
                        char text_after[MAX_INPUT_LINE] = "";
                        if (url_start > input_buf) {
                            size_t pre = (size_t)(url_start - input_buf);
                            if (pre < sizeof(text_before)) {
                                memcpy(text_before, input_buf, pre);
                                text_before[pre] = '\0';
                            }
                        }
                        if (*url_end) {
                            snprintf(text_after, sizeof(text_after), "%s", url_end + 1);
                        }
                        /* Combine text */
                        char combined[MAX_INPUT_LINE];
                        snprintf(combined, sizeof(combined), "%s%s", text_before, text_after);
                        /* Trim */
                        char *txt = combined;
                        while (*txt == ' ') txt++;
                        size_t tl = strlen(txt);
                        while (tl > 0 && txt[tl-1] == ' ') txt[--tl] = '\0';

                        tui_info("loading image from URL...");
                        conv_add_user_image_url(&conv, url_buf,
                                                tl > 0 ? txt : "Describe this image.");
                    }
                }
            }
        }

        /* Check for dragged image file paths in the input */
        if (!has_url_image) {
            int n_images = process_dragged_images(input_buf, &conv);
            if (n_images == 0) {
                conv_add_user_text(&conv, input_buf);
            }
        }

        int turns = 0;
        int total_input = 0, total_output = 0, total_cache_read = 0;
        int total_tools_used = 0;
        int pause_turn_streak = 0;

        /* Per-prompt trace ID */
        char trace_id[37];
        trace_new_id(trace_id, sizeof(trace_id));
        char prompt_span[37] = "";
        trace_span_begin(trace_id, "user_turn", NULL, prompt_span);

        /* Per-turn arena allocator */
        arena_t turn_arena;
        arena_init(&turn_arena);

        while (turns < MAX_AGENT_TURNS && !g_interrupted) {
            turns++;
            s_in_text_block = false;
            md_reset(&s_md);

            if (conv.count > 20) {
                int before = conv.count;
                conv_trim_old_results(&conv, 10, 512);
                /* F17: Auto-compact notification */
                if (conv.count < before)
                    tui_compact_flash(before, conv.count);
            }

            /* Cost budget check */
            if (!check_cost_budget(&session)) {
                tui_error("cost budget exceeded — use /budget to increase or reset");
                break;
            }

            /* Rate limit */
            if (!rate_limiter_acquire(&rate_limiter) || g_interrupted) {
                break;
            }

            /* Build request via provider */
            ensure_provider(&session, api_key);
            char *req = g_provider
                ? g_provider->build_request(g_provider, &conv, &session, MAX_TOKENS)
                : llm_build_request_ex(&conv, &session, MAX_TOKENS);
            if (!req) {
                tui_error("failed to build request");
                baseline_log("error", "request_build_failed", NULL, NULL);
                break;
            }

            /* Stream via provider with fallback chain */
            char llm_span[37] = "";
            trace_span_begin(trace_id, "llm_stream", prompt_span, llm_span);

            /* Start heartbeat — auto-detects silent stream and shows spinner */
            tui_stream_heartbeat_start(&s_heartbeat);
            g_stream_heartbeat = &s_heartbeat;

            stream_result_t sr = g_provider
                ? g_provider->stream(g_provider, api_key, req,
                                      on_stream_text, on_stream_tool_start,
                                      on_stream_thinking, NULL)
                : llm_stream(api_key, req,
                              on_stream_text, on_stream_tool_start,
                              on_stream_thinking, NULL);

            /* Fallback: if failed and fallback chain configured, try next model */
            if (!sr.ok && session.fallback_count > 0) {
                for (int fi = 0; fi < session.fallback_count && !sr.ok && !g_interrupted; fi++) {
                    const char *fb_model = session.fallback_models[fi];
                    if (strcmp(fb_model, session.model) == 0) continue;

                    fprintf(stderr, "  %sfallback: trying %s%s\n", TUI_YELLOW, fb_model, TUI_RESET);
                    tui_stream_heartbeat_poke(&s_heartbeat, "fallback...");
                    json_free_response(&sr.parsed);

                    /* Rebuild request with fallback model */
                    char saved_model[128];
                    snprintf(saved_model, sizeof(saved_model), "%s", session.model);
                    snprintf(session.model, sizeof(session.model), "%s", fb_model);
                    ensure_provider(&session, api_key);

                    free(req);
                    req = g_provider
                        ? g_provider->build_request(g_provider, &conv, &session, MAX_TOKENS)
                        : llm_build_request_ex(&conv, &session, MAX_TOKENS);
                    if (!req) break;

                    /* Resolve API key for fallback provider */
                    const char *fb_key = provider_resolve_api_key(g_provider->name);
                    if (!fb_key) fb_key = api_key;

                    sr = g_provider
                        ? g_provider->stream(g_provider, fb_key, req,
                                              on_stream_text, on_stream_tool_start,
                                              on_stream_thinking, NULL)
                        : llm_stream(fb_key, req,
                                      on_stream_text, on_stream_tool_start,
                                      on_stream_thinking, NULL);

                    if (sr.ok) {
                        fprintf(stderr, "  %sfallback succeeded with %s%s\n", TUI_GREEN, fb_model, TUI_RESET);
                    } else {
                        /* Restore original model for next fallback attempt */
                        snprintf(session.model, sizeof(session.model), "%s", saved_model);
                    }
                }
            }
            free(req);

            /* Stop heartbeat — stream is done */
            g_stream_heartbeat = NULL;
            tui_stream_heartbeat_stop(&s_heartbeat);

            /* Reset forced tool_choice after first turn (single-shot) */
            if (turns == 1 && session.tool_choice[0]) {
                session.tool_choice[0] = '\0';
            }

            if (!sr.ok) {
                char err[128];
                snprintf(err, sizeof(err), "stream failed (HTTP %d)", sr.http_status);
                trace_span_end(llm_span, "error", NULL);
                /* Show structured error if available */
                if (dsco_err_code() != DSCO_ERR_OK) {
                    fprintf(stderr, "  \033[2m[%s] %s\033[0m\n",
                            dsco_err_code_str(dsco_err_code()), dsco_err_msg());
                    dsco_err_clear();
                }
                tui_error(err);
                baseline_log("error", "stream_failed", err, NULL);
                json_free_response(&sr.parsed);
                if (turns == 1) {
                    conv_pop_last(&conv);
                }
                break;
            }
            trace_span_end(llm_span, "ok", NULL);

            /* Flush streamed text BEFORE any stderr output (thinking
               newline etc.) — stderr writes move the terminal cursor,
               which causes erase_partial_echo to miss the echoed line
               and produce duplicate output. */
            if (s_in_text_block) {
                tui_term_lock();
                md_flush(&s_md);
                fprintf(stderr, "\n");
                fflush(stderr);
                tui_term_unlock();
                s_in_text_block = false;
            }

            if (s_in_thinking_block) {
                if (g_features.collapsible_thinking) {
                    tui_thinking_end(&s_thinking);
                } else {
                    fprintf(stderr, "\033[0m\n");
                    fflush(stderr);
                }
                s_in_thinking_block = false;
            }

            total_input += sr.usage.input_tokens;
            total_output += sr.usage.output_tokens;
            total_cache_read += sr.usage.cache_read_input_tokens;
            last_input_tokens = sr.usage.input_tokens;

            /* Accumulate session cost */
            session.total_input_tokens += sr.usage.input_tokens;
            session.total_output_tokens += sr.usage.output_tokens;
            session.total_cache_read_tokens += sr.usage.cache_read_input_tokens;
            session.total_cache_write_tokens += sr.usage.cache_creation_input_tokens;
            session.turn_count++;

            /* Pre-count tool_use blocks so we can suppress noisy per-turn
               usage/telemetry lines when tools will show inline metadata */
            int tool_count_this_turn = 0;
            for (int ti = 0; ti < sr.parsed.count; ti++) {
                content_block_t *tb = &sr.parsed.blocks[ti];
                if (tb->type && strcmp(tb->type, "tool_use") == 0)
                    tool_count_this_turn++;
            }

            /* Only print usage/telemetry for non-tool turns (tool turns fold it inline) */
            if (tool_count_this_turn == 0)
                print_usage_ex(&sr.usage, session.model, &session);

            /* Update and render status bar with full cost (including cache pricing) */
            {
                double cost = session_cost(&session);
                tui_status_bar_update(&status_bar,
                    session.total_input_tokens, session.total_output_tokens,
                    cost, session.turn_count, total_tools_used);
                tui_status_bar_render(&status_bar);
            }

            /* Streaming telemetry — suppress for tool turns */
            if (sr.telemetry.ttft_ms > 0) {
                session.total_ttft_ms += sr.telemetry.ttft_ms;
                session.total_stream_ms += sr.telemetry.total_ms;
                session.telemetry_samples++;
                if (tool_count_this_turn == 0)
                    fprintf(stderr, "%s  [ttft:%.0fms total:%.0fms %.0f tok/s]%s\n",
                            TUI_DIM, sr.telemetry.ttft_ms, sr.telemetry.total_ms,
                            sr.telemetry.tokens_per_sec, TUI_RESET);
                /* F40: Save latency breakdown for /perf */
                s_last_latency.dns_ms = sr.telemetry.latency.dns_ms;
                s_last_latency.connect_ms = sr.telemetry.latency.connect_ms;
                s_last_latency.tls_ms = sr.telemetry.latency.tls_ms;
                s_last_latency.ttfb_ms = sr.telemetry.latency.ttfb_ms;
                s_last_latency.total_ms = sr.telemetry.latency.total_ms;
            }
            /* F39: Throughput data kept for /perf; rendered inline in section divider */
            /* F7: Citations already shown inline as [N] markers during streaming */
            tui_citation_init(&s_citations); /* reset for next turn */
            /* Reset per-turn flame + DAG (data kept for /perf) */
            tui_flame_init(&s_flame);
            tui_dag_init(&s_dag);

            /* Token budget awareness */
            int ctx_window = session.context_window > 0 ? session.context_window : CONTEXT_WINDOW_TOKENS;
            if (sr.usage.input_tokens > (int)(ctx_window * TOKEN_BUDGET_WARN)) {
                fprintf(stderr, "  \033[33m\xe2\x9a\xa0 token budget: %d/%d (%.0f%%)\033[0m\n",
                        sr.usage.input_tokens, ctx_window,
                        100.0 * sr.usage.input_tokens / ctx_window);
                conv_trim_old_results(&conv, 6, 256);
            }

            conv_add_assistant_raw(&conv, &sr.parsed);

            /* Execute tools — parallel when multiple independent calls.
               Continue the loop only when we created follow-up user input
               (local tool results, tool-generated media, etc.). */
            bool needs_followup_turn = false;

            /* tool_count_this_turn already computed above */
            total_tools_used += tool_count_this_turn;

            if (tool_count_this_turn == 1) {
                /* Single tool — execute inline with metrics + caching + watchdog */
                needs_followup_turn = true;
                for (int i = 0; i < sr.parsed.count; i++) {
                    content_block_t *blk = &sr.parsed.blocks[i];
                    if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                        /* Show merged ⚡ name + args on one line */
                        print_tool_start_line(blk->tool_name, blk->tool_input);

                        char trust_reason[256];
                        const char *tier = session_trust_tier_to_string(session.trust_tier);
                        if (!tools_is_allowed_for_tier(blk->tool_name, tier,
                                                       trust_reason, sizeof(trust_reason))) {
                            print_tool_result(blk->tool_name, false, trust_reason);
                            baseline_log("security", "tool_blocked", trust_reason, NULL);
                            conv_add_tool_result(&conv, blk->tool_id, trust_reason, true);
                            break;
                        }

                        /* Validate input schema */
                        char val_err[256];
                        if (!tools_validate_input(blk->tool_name, blk->tool_input,
                                                   val_err, sizeof(val_err))) {
                            fprintf(stderr, "  \033[31m\xe2\x9c\x98 %s\033[0m\n", val_err);
                            conv_add_tool_result(&conv, blk->tool_id, val_err, true);
                            break;
                        }

                        char *tool_result = safe_malloc(MAX_TOOL_RESULT);
                        tool_result[0] = '\0';
                        bool ok = false;

                        /* Check cache (under lock) */
                        pthread_mutex_lock(&g_locks.cache_lock);
                        bool cache_hit = tool_cache_get(&tool_cache, blk->tool_name,
                                                          blk->tool_input, tool_result,
                                                          MAX_TOOL_RESULT, &ok);
                        pthread_mutex_unlock(&g_locks.cache_lock);

                        if (cache_hit) {
                            /* Compact cached result — powerline style with [cached] badge */
                            const tui_glyphs_t *cgl = tui_glyph();
                            const char *nl = tool_result[0] ? strchr(tool_result, '\n') : NULL;
                            int plen = nl ? (int)(nl - tool_result) : (int)strlen(tool_result);
                            if (plen > 80) plen = 80;
                            bool use_pl = tui_detect_color_level() >= TUI_COLOR_256;
                            if (use_pl) {
                                /* Green pill for cached */
                                fprintf(stderr, "  \033[48;2;40;120;60m\033[38;2;200;255;200m %s %s \033[0m",
                                        cgl->ok, blk->tool_name);
                                fprintf(stderr, "\033[38;2;40;120;60m%s%s",
                                        cgl->pl_right ? cgl->pl_right : "", TUI_RESET);
                                fprintf(stderr, " %s⚡cached%s", TUI_DIM, TUI_RESET);
                            } else {
                                fprintf(stderr, "  %s%s%s %s%s%s %s[cached]%s",
                                        TUI_GREEN, cgl->ok, TUI_RESET,
                                        TUI_BOLD, blk->tool_name, TUI_RESET,
                                        TUI_DIM, TUI_RESET);
                            }
                            if (plen > 0)
                                fprintf(stderr, " %s%.*s%s", TUI_DIM, plen, tool_result, TUI_RESET);
                            fprintf(stderr, "\n");
                        } else {
                            /* Trace span for tool execution */
                            char tool_span[37] = "";
                            trace_span_begin(trace_id, blk->tool_name, prompt_span, tool_span);

                            /* Start async spinner + watchdog */
                            tui_async_spinner_t spinner;
                            tui_tool_type_t tt = tui_classify_tool(blk->tool_name);
                            tui_async_spinner_start(&spinner, blk->tool_name, tt);

                            tool_watchdog_t wd;
                            int timeout = tool_timeout_for(blk->tool_name);
                            g_tool_timed_out = 0;
                            watchdog_start(&wd, pthread_self(), blk->tool_name, timeout);
                            tl_tool_cancelled = 0;

                            double t0 = now_ms();
                            ok = tools_execute_for_tier(blk->tool_name, blk->tool_input, tier,
                                                        tool_result, MAX_TOOL_RESULT);
                            double elapsed = (now_ms() - t0) * 1000.0;

                            bool was_timeout = wd.timed_out;
                            watchdog_stop(&wd);

                            /* Build inline usage+cost suffix */
                            char spin_suffix[128] = "";
                            {
                                const model_info_t *mi = model_lookup(session.model);
                                if (mi) {
                                    double tc2 = sr.usage.input_tokens * mi->input_price / 1e6
                                               + sr.usage.output_tokens * mi->output_price / 1e6
                                               + sr.usage.cache_read_input_tokens * mi->cache_read_price / 1e6
                                               + sr.usage.cache_creation_input_tokens * mi->cache_write_price / 1e6;
                                    snprintf(spin_suffix, sizeof(spin_suffix),
                                             "[in:%d out:%d $%.4f]",
                                             sr.usage.input_tokens, sr.usage.output_tokens, tc2);
                                } else {
                                    snprintf(spin_suffix, sizeof(spin_suffix),
                                             "[in:%d out:%d]",
                                             sr.usage.input_tokens, sr.usage.output_tokens);
                                }
                            }

                            /* Stop spinner — shows completion line with inline metadata */
                            tui_async_spinner_stop(&spinner, ok && !was_timeout,
                                                   tool_result, elapsed, spin_suffix);

                            /* If watchdog set g_interrupted (not user), clear it */
                            if (was_timeout && g_tool_timed_out) {
                                g_interrupted = 0;
                                g_tool_timed_out = 0;
                                ok = false;
                                size_t cur = strlen(tool_result);
                                snprintf(tool_result + cur, MAX_TOOL_RESULT - cur,
                                         "\n[timeout: %s exceeded %ds]", blk->tool_name, timeout);
                            }

                            /* F8: Add to flame timeline */
                            tui_flame_add(&s_flame, blk->tool_name, t0 * 1000.0,
                                           (t0 + elapsed / 1000.0) * 1000.0,
                                           ok && !was_timeout, tt);
                            /* F10: Track tool dependency (sequential implies dependency) */
                            {
                                int node = tui_dag_add_node(&s_dag, blk->tool_name);
                                if (s_dag.node_count > 1 && node > 0)
                                    tui_dag_add_edge(&s_dag, node - 1, node);
                            }

                            /* Record metrics (under lock) */
                            pthread_mutex_lock(&g_locks.metrics_lock);
                            tool_metrics_record(&tool_metrics, blk->tool_name, ok, elapsed);
                            if (was_timeout) {
                                const tool_metric_t *m = tool_metrics_get(&tool_metrics, blk->tool_name);
                                if (m) ((tool_metric_t *)m)->timeouts++;
                            }
                            pthread_mutex_unlock(&g_locks.metrics_lock);

                            /* Cache result (under lock) — don't cache timeouts */
                            if (!was_timeout) {
                                pthread_mutex_lock(&g_locks.cache_lock);
                                tool_cache_put(&tool_cache, blk->tool_name, blk->tool_input,
                                                 tool_result, ok, 60.0);
                                pthread_mutex_unlock(&g_locks.cache_lock);
                            }

                            trace_span_end(tool_span, was_timeout ? "timeout" : (ok ? "ok" : "error"), NULL);
                        }
                        /* Spinner printed result for non-cached; cache-hit printed inline above */
                        conv_add_tool_result(&conv, blk->tool_id, tool_result, !ok);
                        free(tool_result);
                    }
                }
            } else if (tool_count_this_turn > 1) {
                /* Multiple tools — execute in sequence with batch spinner */
                needs_followup_turn = true;

                /* Collect tool names for batch spinner */
                const char *batch_names[TUI_BATCH_MAX];
                int batch_indices[TUI_BATCH_MAX]; /* maps batch idx → sr.parsed idx */
                int batch_n = 0;
                for (int i = 0; i < sr.parsed.count && batch_n < TUI_BATCH_MAX; i++) {
                    content_block_t *blk = &sr.parsed.blocks[i];
                    if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                        batch_names[batch_n] = blk->tool_name;
                        batch_indices[batch_n] = i;
                        batch_n++;
                    }
                }

                /* Start batch spinner */
                tui_batch_spinner_t batch_spinner;
                tui_batch_spinner_start(&batch_spinner, batch_names, batch_n);

                int batch_idx = 0;
                for (int i = 0; i < sr.parsed.count; i++) {
                    content_block_t *blk = &sr.parsed.blocks[i];
                    if (g_interrupted) break;
                    if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                        /* Populate args preview into batch entry (shown inline by spinner thread) */
                        {
                            char bp[128];
                            extract_tool_preview(blk->tool_name, blk->tool_input, bp, sizeof(bp));
                            pthread_mutex_lock(&batch_spinner.mutex);
                            if (batch_idx < batch_spinner.count)
                                snprintf(batch_spinner.entries[batch_idx].args_preview,
                                         sizeof(batch_spinner.entries[batch_idx].args_preview),
                                         "%s", bp);
                            pthread_mutex_unlock(&batch_spinner.mutex);
                        }

                        char trust_reason[256];
                        const char *tier = session_trust_tier_to_string(session.trust_tier);
                        if (!tools_is_allowed_for_tier(blk->tool_name, tier,
                                                       trust_reason, sizeof(trust_reason))) {
                            tui_batch_spinner_complete(&batch_spinner, batch_idx, false,
                                                      trust_reason, 0.0);
                            baseline_log("security", "tool_blocked", trust_reason, NULL);
                            conv_add_tool_result(&conv, blk->tool_id, trust_reason, true);
                            batch_idx++;
                            continue;
                        }

                        /* Validate input schema */
                        char val_err[256];
                        if (!tools_validate_input(blk->tool_name, blk->tool_input,
                                                   val_err, sizeof(val_err))) {
                            tui_batch_spinner_complete(&batch_spinner, batch_idx, false,
                                                      val_err, 0.0);
                            conv_add_tool_result(&conv, blk->tool_id, val_err, true);
                            batch_idx++;
                            continue;
                        }

                        char *tool_result = safe_malloc(MAX_TOOL_RESULT);
                        tool_result[0] = '\0';
                        bool ok = false;

                        pthread_mutex_lock(&g_locks.cache_lock);
                        bool cache_hit = tool_cache_get(&tool_cache, blk->tool_name,
                                                        blk->tool_input, tool_result,
                                                        MAX_TOOL_RESULT, &ok);
                        pthread_mutex_unlock(&g_locks.cache_lock);

                        if (cache_hit) {
                            tui_batch_spinner_complete(&batch_spinner, batch_idx, ok,
                                                      "cached", 0.0);
                        } else {
                            char tool_span[37] = "";
                            trace_span_begin(trace_id, blk->tool_name, prompt_span, tool_span);

                            tool_watchdog_t wd;
                            int timeout = tool_timeout_for(blk->tool_name);
                            g_tool_timed_out = 0;
                            watchdog_start(&wd, pthread_self(), blk->tool_name, timeout);
                            tl_tool_cancelled = 0;

                            double t0 = now_ms();
                            ok = tools_execute_for_tier(blk->tool_name, blk->tool_input, tier,
                                                        tool_result, MAX_TOOL_RESULT);
                            double elapsed = (now_ms() - t0) * 1000.0;

                            bool was_timeout = wd.timed_out;
                            watchdog_stop(&wd);

                            if (was_timeout && g_tool_timed_out) {
                                g_interrupted = 0;
                                g_tool_timed_out = 0;
                                ok = false;
                                size_t cur = strlen(tool_result);
                                snprintf(tool_result + cur, MAX_TOOL_RESULT - cur,
                                         "\n[timeout: %s exceeded %ds]", blk->tool_name, timeout);
                            }

                            /* Update batch spinner with completion */
                            tui_batch_spinner_complete(&batch_spinner, batch_idx,
                                                      ok && !was_timeout,
                                                      tool_result, elapsed);

                            pthread_mutex_lock(&g_locks.metrics_lock);
                            tool_metrics_record(&tool_metrics, blk->tool_name, ok, elapsed);
                            if (was_timeout) {
                                const tool_metric_t *m = tool_metrics_get(&tool_metrics, blk->tool_name);
                                if (m) ((tool_metric_t *)m)->timeouts++;
                            }
                            pthread_mutex_unlock(&g_locks.metrics_lock);

                            if (!was_timeout) {
                                pthread_mutex_lock(&g_locks.cache_lock);
                                tool_cache_put(&tool_cache, blk->tool_name, blk->tool_input,
                                               tool_result, ok, 60.0);
                                pthread_mutex_unlock(&g_locks.cache_lock);
                            }

                            trace_span_end(tool_span, was_timeout ? "timeout" : (ok ? "ok" : "error"), NULL);
                        }

                        conv_add_tool_result(&conv, blk->tool_id, tool_result, !ok);
                        free(tool_result);
                        batch_idx++;
                    }
                }

                /* Stop batch spinner — final state is already rendered */
                tui_batch_spinner_stop(&batch_spinner);

                /* Batch aggregate summary */
                if (batch_n >= 2) {
                    const model_info_t *mi = model_lookup(session.model);
                    char batch_cost_suffix[128] = "";
                    if (mi) {
                        double tc2 = sr.usage.input_tokens * mi->input_price / 1e6
                                   + sr.usage.output_tokens * mi->output_price / 1e6
                                   + sr.usage.cache_read_input_tokens * mi->cache_read_price / 1e6
                                   + sr.usage.cache_creation_input_tokens * mi->cache_write_price / 1e6;
                        snprintf(batch_cost_suffix, sizeof(batch_cost_suffix),
                                 "[in:%d out:%d $%.4f]",
                                 sr.usage.input_tokens, sr.usage.output_tokens, tc2);
                    }
                    tui_batch_summary(&batch_spinner, batch_cost_suffix);
                }
            }

            /* F10: Render tool dependency graph (compact, 1 line) */
            tui_dag_render(&s_dag);

            /* F30: Enhanced section divider with success/fail/cache/context */
            {
                double turn_cost = session_cost(&session);
                double tps = sr.telemetry.tokens_per_sec;
                int ctx_used = session.total_input_tokens + session.total_output_tokens;
                int ctx_max = session.context_window > 0 ? session.context_window : CONTEXT_WINDOW_TOKENS;
                double ctx_pct = ctx_max > 0 ? 100.0 * ctx_used / ctx_max : 0;

                /* Count successes/failures for this turn from flame data */
                int turn_ok = 0, turn_fail = 0, turn_cache = 0;
                for (int fi = 0; fi < s_flame.count; fi++) {
                    if (s_flame.entries[fi].ok) turn_ok++; else turn_fail++;
                }
                /* If no flame data, assume all tools succeeded */
                if (s_flame.count == 0 && tool_count_this_turn > 0)
                    turn_ok = tool_count_this_turn;
                turn_cache = tool_cache.hits;  /* session-level cache hits */

                /* Get git branch for divider */
                char div_branch[128] = "";
                if (g_features.branch_indicator) {
                    FILE *gbf = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
                    if (gbf) {
                        if (fgets(div_branch, sizeof(div_branch), gbf)) {
                            size_t bl = strlen(div_branch);
                            if (bl > 0 && div_branch[bl-1] == '\n') div_branch[bl-1] = '\0';
                        }
                        pclose(gbf);
                    }
                }

                tui_section_divider_ex(session.turn_count, turn_ok, turn_fail,
                                       turn_cache, turn_cost, session.model, tps,
                                       ctx_pct, div_branch);
            }

            /* Check for tool-generated images */
            char img_tmp[256];
            snprintf(img_tmp, sizeof(img_tmp), "/tmp/dsco_img_%d.b64", getpid());
            FILE *img_f = fopen(img_tmp, "r");
            if (img_f) {
                char media_type[64] = "";
                if (fgets(media_type, sizeof(media_type), img_f)) {
                    size_t mt_len = strlen(media_type);
                    if (mt_len > 0 && media_type[mt_len-1] == '\n') media_type[mt_len-1] = '\0';

                    fseek(img_f, 0, SEEK_END);
                    long b64_size = ftell(img_f) - (long)(strlen(media_type) + 1);
                    fseek(img_f, (long)(strlen(media_type) + 1), SEEK_SET);

                    if (b64_size > 0 && b64_size < 10 * 1024 * 1024) {
                        char *b64_data = safe_malloc((size_t)b64_size + 1);
                        size_t nr = fread(b64_data, 1, (size_t)b64_size, img_f);
                        b64_data[nr] = '\0';

                        conv_add_user_image_base64(&conv, media_type, b64_data,
                                                    "Analyze this image.");
                        needs_followup_turn = true;
                        free(b64_data);
                    }
                }
                fclose(img_f);
                unlink(img_tmp);
            }

            /* Check for tool-generated documents */
            char doc_tmp[256];
            snprintf(doc_tmp, sizeof(doc_tmp), "/tmp/dsco_doc_%d.b64", getpid());
            FILE *doc_f = fopen(doc_tmp, "r");
            if (doc_f) {
                char media_type[64] = "";
                if (fgets(media_type, sizeof(media_type), doc_f)) {
                    size_t mt_len = strlen(media_type);
                    if (mt_len > 0 && media_type[mt_len-1] == '\n') media_type[mt_len-1] = '\0';

                    fseek(doc_f, 0, SEEK_END);
                    long b64_size = ftell(doc_f) - (long)(strlen(media_type) + 1);
                    fseek(doc_f, (long)(strlen(media_type) + 1), SEEK_SET);

                    if (b64_size > 0 && b64_size < 50 * 1024 * 1024) {
                        char *b64_data = safe_malloc((size_t)b64_size + 1);
                        size_t nr = fread(b64_data, 1, (size_t)b64_size, doc_f);
                        b64_data[nr] = '\0';

                        conv_add_user_document(&conv, media_type, b64_data,
                                                NULL, "Analyze this document.");
                        needs_followup_turn = true;
                        free(b64_data);
                    }
                }
                fclose(doc_f);
                unlink(doc_tmp);
            }

            bool pause_turn = (sr.parsed.stop_reason &&
                               strcmp(sr.parsed.stop_reason, "pause_turn") == 0);
            if (pause_turn && !needs_followup_turn) {
                pause_turn_streak++;
            } else {
                pause_turn_streak = 0;
            }

            bool done = !needs_followup_turn && !pause_turn;
            if (pause_turn_streak >= 3) {
                tui_warning("provider returned pause_turn repeatedly; ending turn");
                done = true;
            }
            /* F34: Notification bell when multi-turn response completes */
            if (done && turns > 1) {
                tui_notify("dsco", "response complete");
            }

            baseline_log("turn",
                         done ? "turn_done" : "turn_continue",
                         sr.parsed.stop_reason ? sr.parsed.stop_reason : "",
                         NULL);

            /* IPC: heartbeat + inject pending messages */
            ipc_heartbeat();
            int ipc_flags = ipc_poll();
            if (ipc_flags & 1) {
                ipc_message_t msgs[8];
                int msg_count = ipc_recv(msgs, 8);
                if (msg_count > 0 && done) {
                    jbuf_t mb;
                    jbuf_init(&mb, 2048);
                    jbuf_append(&mb, "[IPC] Incoming messages from other agents:\n");
                    for (int mi = 0; mi < msg_count; mi++) {
                        char hdr[256];
                        snprintf(hdr, sizeof(hdr), "  From %s (topic: %s): ",
                                 msgs[mi].from_agent, msgs[mi].topic);
                        jbuf_append(&mb, hdr);
                        jbuf_append(&mb, msgs[mi].body ? msgs[mi].body : "");
                        jbuf_append(&mb, "\n");
                        free(msgs[mi].body);
                    }
                    if (mb.data) {
                        conv_add_user_text(&conv, mb.data);
                        done = false;
                    }
                    jbuf_free(&mb);
                } else {
                    for (int mi = 0; mi < msg_count; mi++) free(msgs[mi].body);
                }
            }

            json_free_response(&sr.parsed);
            arena_reset(&turn_arena);
            if (done) break;
        }

        /* End prompt-level trace span */
        trace_span_end(prompt_span, g_interrupted ? "interrupted" : "ok", NULL);
        arena_free(&turn_arena);

        if (g_interrupted) {
            fprintf(stderr, "\n");
            tui_warning("interrupted (press Ctrl+C again to force quit)");
        }
        if (turns >= MAX_AGENT_TURNS) {
            char msg[64];
            snprintf(msg, sizeof(msg), "max turns reached (%d)", MAX_AGENT_TURNS);
            tui_warning(msg);
        }
        if (turns > 1) {
            double multi_turn_cost = session_cost(&session);
            fprintf(stderr, "%s  [%d turns | in:%d out:%d cache-read:%d | $%.4f]%s\n",
                    TUI_DIM, turns, total_input, total_output, total_cache_read,
                    multi_turn_cost, TUI_RESET);
        }
        /* Use stderr for the blank line separator — stdout writes can
           desync with the scroll region and push the cursor into the
           bottom panel, causing input to appear in the middle. */
        fprintf(stderr, "\n");
        fflush(stderr);

        /* Periodic auto-save every 5 turns */
        if (session.turn_count % 5 == 0 && conv.count > 0) {
            autosave(&conv, &session);
        }
    }

    /* Disable bracketed paste mode */
    fprintf(stderr, "\033[?2004l");
    fflush(stderr);

    g_winch_sb = NULL;
    tui_status_bar_disable(&status_bar);

    g_autosave_conv = NULL;
    g_autosave_session = NULL;
    autosave(&conv, &session);
    conv_free(&conv);
    mcp_shutdown(&g_mcp);
    provider_free(g_provider);
    g_provider = NULL;
    tool_map_free(&g_tool_map);
    tool_cache_free(&tool_cache);
    dsco_locks_destroy(&g_locks);
    fprintf(stderr, "%s  goodbye%s\n", TUI_DIM, TUI_RESET);
}
