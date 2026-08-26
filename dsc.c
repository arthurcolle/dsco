/*
 * dsc — minimal multi-turn streaming Claude agent
 * Single file. No TUI. No heartbeat. No scroll regions. Just works.
 * Optimized for iTerm2 streaming output.
 *
 * Build: cc -O2 -o dsc dsc.c -lcurl -lreadline
 * Usage: ./dsc
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <pwd.h>
#include <curl/curl.h>
#include <readline/readline.h>
#include <readline/history.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

/* ── Config ────────────────────────────────────────────────────────────── */

#define API_URL         "https://api.anthropic.com/v1/messages"
#define API_VERSION     "2023-06-01"
#define ANTHROPIC_BETAS "interleaved-thinking-2025-05-14,code-execution-2025-05-22,advanced-tool-use-2025-11-20"
#define CLAUDE_CODE_OAUTH_BETA "oauth-2025-04-20"
#define CLAUDE_CODE_OAUTH_TOKEN_URL "https://platform.claude.com/v1/oauth/token"
#define CLAUDE_CODE_OAUTH_CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define CLAUDE_CODE_OAUTH_SCOPES "user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload"
#define CLAUDE_CODE_OAUTH_EXPIRY_BUFFER_MS (5LL * 60LL * 1000LL)
#define DSC_VERSION     "1.0.0"
#define DEFAULT_MODEL   "claude-sonnet-4-6"
#define MAX_TURNS       256
#define MAX_BLOCKS      64
#define INITIAL_CAP     4096

static volatile sig_atomic_t g_interrupted = 0;
static volatile sig_atomic_t g_winch       = 0;
static void sigint_handler(int sig)  { (void)sig; g_interrupted = 1; }
static void sigwinch_handler(int sig){ (void)sig; g_winch = 1; }

/* argv[0] saved for re-exec in --oneshot child agents */
static const char *g_argv0 = "dsc";

/* ── Terminal output: write(2) direct to fd for instant streaming ───── */

static int g_out_fd = -1;  /* stdout fd — for streaming text */

static void out_raw(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(g_out_fd, s, n);
        if (w <= 0) break;
        s += w; n -= (size_t)w;
    }
}

static void out(const char *s) { out_raw(s, strlen(s)); }

static void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) out_raw(tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
}

static bool dsc_env_truthy(const char *value) {
    return value && (value[0] == '1' || strcasecmp(value, "true") == 0 ||
                     strcasecmp(value, "yes") == 0);
}

static bool dsc_uses_claude_code_auth(const char *credential) {
    if (dsc_env_truthy(getenv("DSCO_FORCE_CLAUDE_CODE_AUTH"))) return true;
    return credential && strncmp(credential, "sk-ant-oat", 10) == 0;
}

static void dsc_expand_path(char *out, size_t out_len, const char *path) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;

    if (path[0] == '~' && path[1] == '/') {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return;
        snprintf(out, out_len, "%s/%s", home, path + 2);
        return;
    }

    snprintf(out, out_len, "%s", path);
}

static char *dsc_read_text_file(const char *path) {
    if (!path || !path[0]) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }

    size_t got = fread(data, 1, (size_t)size, fp);
    fclose(fp);
    data[got] = '\0';
    return data;
}

static const char *dsc_resolve_claude_code_oauth_token(void);

/* ── iTerm2 detection and features ─────────────────────────────────────── */

static bool g_iterm2 = false;
static bool g_color  = true;

static void detect_term(void) {
    const char *tp = getenv("TERM_PROGRAM");
    if (tp && strcmp(tp, "iTerm.app") == 0) g_iterm2 = true;
    const char *ct = getenv("COLORTERM");
    if (ct && (strcmp(ct, "truecolor") == 0 || strcmp(ct, "24bit") == 0))
        g_color = true;
    const char *fc = getenv("FORCE_COLOR");
    if (fc && *fc && strcmp(fc, "0") != 0) { g_color = true; return; }
    if (!isatty(STDOUT_FILENO)) g_color = false;
}

/* iTerm2 shell integration marks (optional, for proper prompt detection) */
static void iterm_mark_prompt_start(void) {
    if (g_iterm2) out("\033]133;A\007");
}
static void iterm_mark_prompt_end(void) {
    if (g_iterm2) out("\033]133;B\007");
}
static void iterm_mark_command_start(void) {
    if (g_iterm2) out("\033]133;C\007");
}
static void iterm_mark_command_end(int status) {
    if (g_iterm2) outf("\033]133;D;%d\007", status);
}

/* Get terminal width */
static int term_width(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

/* ── Persistent composer box ──────────────────────────────────────────── */
/* Draws a Claude-Code-style input box inline:
 *   ────────────────────────────────────────
 *   > user text here
 *   ────────────────────────────────────────
 *   ↵ send · ⌥↵ newline · ↑/↓ history
 * Returns malloc'd string on submit, NULL on Ctrl+C/EOF.
 */

#define DSC_COMPOSER_CAP 16384

static int dsc_utf8_cells(const char *s, size_t nbytes) {
    int cells = 0;
    for (size_t i = 0; i < nbytes; ) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') { i++; continue; }
        if      (c < 0x80)            { cells++; i += 1; }
        else if ((c & 0xE0) == 0xC0)  { cells++; i += 2; }
        else if ((c & 0xF0) == 0xE0)  { cells++; i += 3; }
        else if ((c & 0xF8) == 0xF0)  { cells++; i += 4; }
        else                           { i += 1; }
    }
    return cells;
}

static void dsc_composer_draw_top_rule(void) {
    int cols = term_width();
    if (cols > 78) cols = 78;
    if (g_color) fprintf(stdout, "\033[38;5;240m");
    for (int i = 0; i < cols; i++) fputs("─", stdout);
    if (g_color) fprintf(stdout, "\033[0m");
    fputc('\n', stdout);
}

static void dsc_composer_draw_bottom_rule(void) {
    int cols = term_width();
    if (cols > 78) cols = 78;
    if (g_color) fprintf(stdout, "\033[38;5;240m");
    for (int i = 0; i < cols; i++) fputs("─", stdout);
    if (g_color) fprintf(stdout, "\033[0m");
    fputc('\n', stdout);
    if (g_color)
        fputs("\033[2;38;5;244m  ↵ send  ·  ⌥↵ newline  ·  ↑/↓ history  ·  /help  ·  ctrl+c cancel\033[0m\n", stdout);
    else
        fputs("  enter send · alt+enter newline · ctrl+c cancel\n", stdout);
    fflush(stdout);
}

/* Draw the active line given current buffer + cursor (1 visual line at a time).
 * Positions cursor at the right spot. Called on every keystroke. */
static void dsc_composer_redraw_input(const char *buf, size_t len, size_t cur) {
    /* Move to start of input line, clear, reprint */
    fputs("\r\033[2K", stdout);
    if (g_color) fputs("\033[1;38;5;213m>\033[0m ", stdout);
    else         fputs("> ", stdout);

    /* Find logical line containing cursor */
    size_t ls = 0;
    for (size_t i = 0; i < cur; i++)
        if (buf[i] == '\n') ls = i + 1;
    size_t le = cur;
    while (le < len && buf[le] != '\n') le++;

    int cols = term_width() - 3;
    if (cols < 10) cols = 10;
    int line_cells = dsc_utf8_cells(buf + ls, le - ls);
    int cur_cells  = dsc_utf8_cells(buf + ls, cur - ls);

    size_t render_start = ls;
    int skip = 0;
    if (line_cells > cols) {
        int target = cols - 4;
        if (target < 0) target = 0;
        if (cur_cells > target) {
            skip = cur_cells - target;
            size_t i = ls;
            int s2 = 0;
            while (i < le && s2 < skip) {
                unsigned char c = (unsigned char)buf[i];
                if      (c < 0x80)            i += 1;
                else if ((c & 0xE0) == 0xC0)  i += 2;
                else if ((c & 0xF0) == 0xE0)  i += 3;
                else if ((c & 0xF8) == 0xF0)  i += 4;
                else                          i += 1;
                s2++;
            }
            render_start = i;
        }
    }
    int multi_pref = 0;
    if (ls > 0)    { if (g_color) fputs("\033[38;5;240m… \033[0m", stdout); else fputs("... ", stdout); multi_pref = 2; }

    int written = 0;
    size_t j = render_start;
    while (j < le && written + multi_pref < cols) {
        unsigned char c = (unsigned char)buf[j];
        size_t clen = 1;
        if      (c < 0x80)            clen = 1;
        else if ((c & 0xE0) == 0xC0)  clen = 2;
        else if ((c & 0xF0) == 0xE0)  clen = 3;
        else if ((c & 0xF8) == 0xF0)  clen = 4;
        if (j + clen > le) clen = 1;
        fwrite(buf + j, 1, clen, stdout);
        j += clen;
        written++;
    }
    if (le > j) { if (g_color) fputs("\033[38;5;240m…\033[0m", stdout); else fputc('.', stdout); }
    if (le < len) { if (g_color) fputs("\033[38;5;240m ⏎\033[0m", stdout); }

    /* Place cursor: prompt (2) + multi_pref + (cur_cells - skip) */
    int target_col = 1 + 2 + multi_pref + (cur_cells - skip);
    fprintf(stdout, "\r\033[%dC", target_col - 1);
    fflush(stdout);
}

static void dsc_composer_insert(char *buf, size_t cap, size_t *len, size_t *cur,
                                const char *s, size_t slen) {
    if (*len + slen >= cap - 1) return;
    memmove(buf + *cur + slen, buf + *cur, *len - *cur);
    memcpy(buf + *cur, s, slen);
    *len += slen;
    *cur += slen;
    buf[*len] = '\0';
}

static void dsc_composer_bs(char *buf, size_t *len, size_t *cur) {
    if (*cur == 0) return;
    size_t start = *cur - 1;
    while (start > 0 && ((unsigned char)buf[start] & 0xC0) == 0x80) start--;
    size_t rem = *cur - start;
    memmove(buf + start, buf + *cur, *len - *cur);
    *len -= rem;
    *cur = start;
    buf[*len] = '\0';
}

static void dsc_composer_del(char *buf, size_t *len, size_t *cur) {
    if (*cur >= *len) return;
    size_t end = *cur + 1;
    while (end < *len && ((unsigned char)buf[end] & 0xC0) == 0x80) end++;
    size_t rem = end - *cur;
    memmove(buf + *cur, buf + end, *len - end);
    *len -= rem;
    buf[*len] = '\0';
}

static void dsc_cur_left(const char *buf, size_t *cur) {
    if (*cur == 0) return;
    (*cur)--;
    while (*cur > 0 && ((unsigned char)buf[*cur] & 0xC0) == 0x80) (*cur)--;
}

static void dsc_cur_right(const char *buf, size_t len, size_t *cur) {
    if (*cur >= len) return;
    (*cur)++;
    while (*cur < len && ((unsigned char)buf[*cur] & 0xC0) == 0x80) (*cur)++;
}

static ssize_t dsc_read_byte_timed(int fd, int ms, unsigned char *out) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, ms);
    if (r <= 0) return r;
    return read(fd, out, 1);
}

/* History stash (cross-call) for up/down navigation. */
static char **g_dsc_hist = NULL;
static int    g_dsc_hist_n = 0;
static int    g_dsc_hist_cap = 0;

static void dsc_hist_push(const char *s) {
    if (!s || !*s) return;
    if (g_dsc_hist_n > 0 && strcmp(g_dsc_hist[g_dsc_hist_n - 1], s) == 0) return;
    if (g_dsc_hist_n == g_dsc_hist_cap) {
        g_dsc_hist_cap = g_dsc_hist_cap ? g_dsc_hist_cap * 2 : 32;
        g_dsc_hist = realloc(g_dsc_hist, sizeof(char *) * (size_t)g_dsc_hist_cap);
    }
    g_dsc_hist[g_dsc_hist_n++] = strdup(s);
}

static char *dsc_composer_read(void) {
    if (!isatty(STDIN_FILENO)) {
        /* Fallback: plain fgets */
        char line[4096];
        if (!fgets(line, sizeof(line), stdin)) return NULL;
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        return strdup(line);
    }

    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        char line[4096];
        if (!fgets(line, sizeof(line), stdin)) return NULL;
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        return strdup(line);
    }
    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    fputs("\033[?2004h", stdout);  /* bracketed paste on */

    dsc_composer_draw_top_rule();
    /* Print empty input row so subsequent \r stays on it */
    if (g_color) fputs("\033[1;38;5;213m>\033[0m ", stdout);
    else         fputs("> ", stdout);
    fflush(stdout);

    char *buf = calloc(1, DSC_COMPOSER_CAP);
    size_t len = 0, cur = 0;
    int hist_pos = -1;
    char saved_buf[DSC_COMPOSER_CAP] = {0};
    size_t saved_len = 0, saved_cur = 0;
    bool done = false, cancelled = false;
    bool in_paste = false;

    while (!done) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) { cancelled = true; break; }

        if (in_paste) {
            if (c == '\033') {
                unsigned char seq[5] = {0};
                int got = 0;
                while (got < 5) {
                    if (read(STDIN_FILENO, &seq[got], 1) != 1) break;
                    got++;
                }
                if (got == 5 && memcmp(seq, "[201~", 5) == 0) {
                    in_paste = false;
                    dsc_composer_redraw_input(buf, len, cur);
                    continue;
                }
                /* Not end marker — insert raw */
                dsc_composer_insert(buf, DSC_COMPOSER_CAP, &len, &cur, "\033", 1);
                for (int k = 0; k < got; k++) {
                    if (seq[k] == '\r') seq[k] = '\n';
                    dsc_composer_insert(buf, DSC_COMPOSER_CAP, &len, &cur, (char*)&seq[k], 1);
                }
                dsc_composer_redraw_input(buf, len, cur);
                continue;
            }
            if (c == '\r') c = '\n';
            dsc_composer_insert(buf, DSC_COMPOSER_CAP, &len, &cur, (char*)&c, 1);
            dsc_composer_redraw_input(buf, len, cur);
            continue;
        }

        if (c == 0x03) { cancelled = true; break; }                   /* Ctrl+C */
        if (c == 0x04) { if (len == 0) { cancelled = true; break; }
                         dsc_composer_del(buf, &len, &cur);
                         dsc_composer_redraw_input(buf, len, cur); continue; }
        if (c == 0x01) { while (cur > 0 && buf[cur-1] != '\n') cur--;
                         dsc_composer_redraw_input(buf, len, cur); continue; }  /* Ctrl+A */
        if (c == 0x05) { while (cur < len && buf[cur] != '\n') cur++;
                         dsc_composer_redraw_input(buf, len, cur); continue; }  /* Ctrl+E */
        if (c == 0x15) {
            size_t ls = cur;
            while (ls > 0 && buf[ls-1] != '\n') ls--;
            memmove(buf + ls, buf + cur, len - cur);
            len -= (cur - ls); cur = ls; buf[len] = '\0';
            dsc_composer_redraw_input(buf, len, cur); continue;
        }
        if (c == 0x17) {
            size_t c2 = cur;
            while (c2 > 0 && (buf[c2-1] == ' ' || buf[c2-1] == '\t')) c2--;
            while (c2 > 0 && buf[c2-1] != ' ' && buf[c2-1] != '\t' && buf[c2-1] != '\n') c2--;
            memmove(buf + c2, buf + cur, len - cur);
            len -= (cur - c2); cur = c2; buf[len] = '\0';
            dsc_composer_redraw_input(buf, len, cur); continue;
        }
        if (c == '\r' || c == '\n') { done = true; break; }
        if (c == 0x0A) {  /* Ctrl+J literal newline */
            dsc_composer_insert(buf, DSC_COMPOSER_CAP, &len, &cur, "\n", 1);
            dsc_composer_redraw_input(buf, len, cur); continue;
        }
        if (c == 0x7F || c == 0x08) {
            dsc_composer_bs(buf, &len, &cur);
            dsc_composer_redraw_input(buf, len, cur); continue;
        }
        if (c == 0x09) {
            dsc_composer_insert(buf, DSC_COMPOSER_CAP, &len, &cur, "  ", 2);
            dsc_composer_redraw_input(buf, len, cur); continue;
        }
        if (c == 0x1B) {
            unsigned char n1;
            if (dsc_read_byte_timed(STDIN_FILENO, 30, &n1) <= 0) {
                cancelled = true; break;
            }
            if (n1 == '\r' || n1 == '\n') {
                dsc_composer_insert(buf, DSC_COMPOSER_CAP, &len, &cur, "\n", 1);
                dsc_composer_redraw_input(buf, len, cur); continue;
            }
            if (n1 != '[' && n1 != 'O') { continue; }
            unsigned char n2;
            if (dsc_read_byte_timed(STDIN_FILENO, 30, &n2) <= 0) continue;
            if (n2 == '2') {
                unsigned char n3, n4;
                if (dsc_read_byte_timed(STDIN_FILENO, 30, &n3) > 0 &&
                    dsc_read_byte_timed(STDIN_FILENO, 30, &n4) > 0 &&
                    n3 == '0' && n4 == '0') {
                    unsigned char tilde;
                    dsc_read_byte_timed(STDIN_FILENO, 30, &tilde);
                    in_paste = true;
                    continue;
                }
                continue;
            }
            switch (n2) {
                case 'A':  /* Up */
                    if (g_dsc_hist_n > 0) {
                        if (hist_pos == -1) {
                            memcpy(saved_buf, buf, len);
                            saved_buf[len] = '\0';
                            saved_len = len; saved_cur = cur;
                            hist_pos = g_dsc_hist_n - 1;
                        } else if (hist_pos > 0) {
                            hist_pos--;
                        }
                        if (hist_pos >= 0 && hist_pos < g_dsc_hist_n) {
                            const char *h = g_dsc_hist[hist_pos];
                            size_t hl = strlen(h);
                            if (hl >= DSC_COMPOSER_CAP) hl = DSC_COMPOSER_CAP - 1;
                            memcpy(buf, h, hl);
                            buf[hl] = '\0';
                            len = hl; cur = hl;
                        }
                    }
                    dsc_composer_redraw_input(buf, len, cur); continue;
                case 'B':  /* Down */
                    if (hist_pos >= 0) {
                        hist_pos++;
                        if (hist_pos >= g_dsc_hist_n) {
                            memcpy(buf, saved_buf, saved_len);
                            buf[saved_len] = '\0';
                            len = saved_len; cur = saved_cur;
                            hist_pos = -1;
                        } else {
                            const char *h = g_dsc_hist[hist_pos];
                            size_t hl = strlen(h);
                            if (hl >= DSC_COMPOSER_CAP) hl = DSC_COMPOSER_CAP - 1;
                            memcpy(buf, h, hl);
                            buf[hl] = '\0';
                            len = hl; cur = hl;
                        }
                    }
                    dsc_composer_redraw_input(buf, len, cur); continue;
                case 'C': dsc_cur_right(buf, len, &cur); dsc_composer_redraw_input(buf, len, cur); continue;
                case 'D': dsc_cur_left(buf, &cur); dsc_composer_redraw_input(buf, len, cur); continue;
                case 'H': while (cur > 0 && buf[cur-1] != '\n') cur--;
                          dsc_composer_redraw_input(buf, len, cur); continue;
                case 'F': while (cur < len && buf[cur] != '\n') cur++;
                          dsc_composer_redraw_input(buf, len, cur); continue;
                case '3': { unsigned char tilde; dsc_read_byte_timed(STDIN_FILENO, 30, &tilde);
                            dsc_composer_del(buf, &len, &cur);
                            dsc_composer_redraw_input(buf, len, cur); continue; }
                default: continue;
            }
        }

        if (c >= 0x20 || c >= 0x80) {
            unsigned char utf[4]; utf[0] = c; int want = 0;
            if      ((c & 0x80) == 0)      want = 0;
            else if ((c & 0xE0) == 0xC0)   want = 1;
            else if ((c & 0xF0) == 0xE0)   want = 2;
            else if ((c & 0xF8) == 0xF0)   want = 3;
            for (int k = 0; k < want; k++) {
                if (read(STDIN_FILENO, &utf[1+k], 1) != 1) break;
            }
            dsc_composer_insert(buf, DSC_COMPOSER_CAP, &len, &cur,
                                (char*)utf, (size_t)(1 + want));
            dsc_composer_redraw_input(buf, len, cur);
            continue;
        }
    }

    fputs("\033[?2004l", stdout);
    fputc('\n', stdout);
    dsc_composer_draw_bottom_rule();
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);

    if (cancelled && len == 0) { free(buf); return NULL; }
    if (cancelled) { free(buf); return strdup(""); }

    char *result = strndup(buf, len);
    free(buf);
    dsc_hist_push(result);
    return result;
}

/* Millisecond timestamp */
static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ── Dynamic string buffer ─────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len, cap;
} buf_t;

static void buf_init(buf_t *b) {
    b->cap = INITIAL_CAP;
    b->data = malloc(b->cap);
    b->len = 0;
    b->data[0] = '\0';
}

static void buf_free(buf_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }
static void buf_reset(buf_t *b) { b->len = 0; b->data[0] = '\0'; }

static void buf_grow(buf_t *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    while (b->cap < b->len + need + 1) b->cap *= 2;
    b->data = realloc(b->data, b->cap);
    if (!b->data) { fprintf(stderr, "dsc: out of memory\n"); exit(1); }
}

static void buf_append(buf_t *b, const char *s, size_t n) {
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_cat(buf_t *b, const char *s) { buf_append(b, s, strlen(s)); }
static void buf_char(buf_t *b, char c) { buf_grow(b, 1); b->data[b->len++] = c; b->data[b->len] = '\0'; }

static void buf_printf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void buf_printf(buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[4096];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_append(b, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
}

/* ── JSON escape ───────────────────────────────────────────────────────── */

static void buf_json_str(buf_t *b, const char *s) {
    buf_char(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  buf_cat(b, "\\\""); break;
            case '\\': buf_cat(b, "\\\\"); break;
            case '\b': buf_cat(b, "\\b");  break;
            case '\f': buf_cat(b, "\\f");  break;
            case '\n': buf_cat(b, "\\n");  break;
            case '\r': buf_cat(b, "\\r");  break;
            case '\t': buf_cat(b, "\\t");  break;
            default:
                if (*p < 0x20) {
                    char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", *p);
                    buf_cat(b, esc);
                } else {
                    buf_char(b, (char)*p);
                }
        }
    }
    buf_char(b, '"');
}

/* ── Tiny JSON field extraction ────────────────────────────────────────── */

static const char *jskip(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *jskip_value(const char *p) {
    p = jskip(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        if (*p == '"') p++;
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } if (*p) p++; }
            else if (*p == open || *p == '{' || *p == '[') {
                if (*p == open) depth++;
                else if (*p == '{' || *p == '[') depth++;
                p++;
            }
            else if (*p == close || *p == '}' || *p == ']') {
                if (*p == close) depth--;
                else if (*p == '}' || *p == ']') depth--;
                p++;
            }
            else p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}

static const char *jfind(const char *json, const char *key) {
    if (!json) return NULL;
    const char *p = jskip(json);
    if (*p != '{') return NULL;
    p++;
    while (*p) {
        p = jskip(p);
        if (*p == '}') return NULL;
        if (*p != '"') return NULL;
        p++;
        const char *ks = p;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        size_t klen = (size_t)(p - ks);
        if (*p == '"') p++;
        p = jskip(p);
        if (*p == ':') p++;
        p = jskip(p);
        if (klen == strlen(key) && memcmp(ks, key, klen) == 0)
            return p;
        p = jskip_value(p);
        p = jskip(p);
        if (*p == ',') p++;
    }
    return NULL;
}

static char *jstr(const char *json, const char *key) {
    const char *v = jfind(json, key);
    if (!v || *v != '"') return NULL;
    v++;
    buf_t b; buf_init(&b);
    while (*v && *v != '"') {
        if (*v == '\\') {
            v++;
            switch (*v) {
                case '"': case '\\': case '/': buf_char(&b, *v); break;
                case 'b': buf_char(&b, '\b'); break;
                case 'f': buf_char(&b, '\f'); break;
                case 'n': buf_char(&b, '\n'); break;
                case 'r': buf_char(&b, '\r'); break;
                case 't': buf_char(&b, '\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4 && v[1]; i++) { v++;
                        cp *= 16;
                        if (*v >= '0' && *v <= '9') cp += *v - '0';
                        else if (*v >= 'a' && *v <= 'f') cp += *v - 'a' + 10;
                        else if (*v >= 'A' && *v <= 'F') cp += *v - 'A' + 10;
                    }
                    /* Handle surrogate pairs */
                    if (cp >= 0xD800 && cp <= 0xDBFF && v[1] == '\\' && v[2] == 'u') {
                        unsigned lo = 0;
                        v += 2; /* skip \u */
                        for (int i = 0; i < 4 && v[1]; i++) { v++;
                            lo *= 16;
                            if (*v >= '0' && *v <= '9') lo += *v - '0';
                            else if (*v >= 'a' && *v <= 'f') lo += *v - 'a' + 10;
                            else if (*v >= 'A' && *v <= 'F') lo += *v - 'A' + 10;
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    /* Encode as UTF-8 */
                    if (cp < 0x80) buf_char(&b, (char)cp);
                    else if (cp < 0x800) {
                        buf_char(&b, (char)(0xC0 | (cp >> 6)));
                        buf_char(&b, (char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        buf_char(&b, (char)(0xE0 | (cp >> 12)));
                        buf_char(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        buf_char(&b, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        buf_char(&b, (char)(0xF0 | (cp >> 18)));
                        buf_char(&b, (char)(0x80 | ((cp >> 12) & 0x3F)));
                        buf_char(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        buf_char(&b, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: buf_char(&b, *v);
            }
        } else {
            buf_char(&b, *v);
        }
        v++;
    }
    return b.data;
}

static char *jraw(const char *json, const char *key) {
    const char *v = jfind(json, key);
    if (!v) return NULL;
    const char *end = jskip_value(v);
    size_t len = (size_t)(end - v);
    char *r = malloc(len + 1);
    memcpy(r, v, len);
    r[len] = '\0';
    return r;
}

static int jint(const char *json, const char *key, int def) {
    const char *v = jfind(json, key);
    if (!v) return def;
    return atoi(v);
}

typedef enum {
    DSC_OAUTH_SOURCE_MISSING = 0,
    DSC_OAUTH_SOURCE_ENV,
    DSC_OAUTH_SOURCE_KEYCHAIN,
    DSC_OAUTH_SOURCE_FILE,
} dsc_oauth_source_t;

typedef struct {
    dsc_oauth_source_t source;
    char access_token[4096];
    char refresh_token[4096];
    long long expires_at_ms;
    char credentials_path[1024];
    char keychain_service[128];
    char keychain_account[128];
    char *storage_json;
    char *oauth_json;
} dsc_oauth_bundle_t;

static void dsc_oauth_bundle_init(dsc_oauth_bundle_t *bundle) {
    memset(bundle, 0, sizeof(*bundle));
}

static void dsc_oauth_bundle_free(dsc_oauth_bundle_t *bundle) {
    if (!bundle) return;
    free(bundle->storage_json);
    free(bundle->oauth_json);
    bundle->storage_json = NULL;
    bundle->oauth_json = NULL;
}

static void dsc_get_username(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    const char *user = getenv("USER");
    if (user && user[0]) {
        snprintf(out, out_len, "%s", user);
        return;
    }

    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name && pw->pw_name[0]) {
        snprintf(out, out_len, "%s", pw->pw_name);
        return;
    }

    snprintf(out, out_len, "claude-code-user");
}

static bool dsc_oauth_expired(long long expires_at_ms) {
    if (expires_at_ms <= 0) return false;
    return now_ms() + CLAUDE_CODE_OAUTH_EXPIRY_BUFFER_MS >= expires_at_ms;
}

static void dsc_build_claude_code_service_name(char *out, size_t out_len) {
    const char *override = getenv("DSCO_CLAUDE_CODE_KEYCHAIN_SERVICE");
    if (override && override[0]) {
        snprintf(out, out_len, "%s", override);
        return;
    }

    const char *oauth_suffix = "";
    if (getenv("CLAUDE_CODE_CUSTOM_OAUTH_URL")) {
        oauth_suffix = "-custom-oauth";
    } else if (getenv("USER_TYPE") &&
               strcmp(getenv("USER_TYPE"), "ant") == 0 &&
               dsc_env_truthy(getenv("USE_LOCAL_OAUTH"))) {
        oauth_suffix = "-local-oauth";
    } else if (getenv("USER_TYPE") &&
               strcmp(getenv("USER_TYPE"), "ant") == 0 &&
               dsc_env_truthy(getenv("USE_STAGING_OAUTH"))) {
        oauth_suffix = "-staging-oauth";
    }

    char dir_suffix[16] = "";
    const char *config_dir = getenv("CLAUDE_CONFIG_DIR");
    if (config_dir && config_dir[0]) {
#ifdef __APPLE__
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(config_dir, (CC_LONG)strlen(config_dir), digest);
        snprintf(dir_suffix, sizeof(dir_suffix), "-%02x%02x%02x%02x",
                 digest[0], digest[1], digest[2], digest[3]);
#endif
    }

    snprintf(out, out_len, "Claude Code%s-credentials%s", oauth_suffix, dir_suffix);
}

static void dsc_build_claude_code_credentials_path(char *out, size_t out_len) {
    const char *override_path = getenv("DSCO_CLAUDE_CODE_CREDENTIALS_FILE");
    if (override_path && override_path[0]) {
        dsc_expand_path(out, out_len, override_path);
        return;
    }

    const char *config_dir = getenv("CLAUDE_CONFIG_DIR");
    if (config_dir && config_dir[0]) {
        snprintf(out, out_len, "%s/.credentials.json", config_dir);
        return;
    }

    dsc_expand_path(out, out_len, "~/.claude/.credentials.json");
}

static char *dsc_shell_quote(const char *s) {
    buf_t b; buf_init(&b);
    buf_char(&b, '\'');
    if (s) {
        for (const char *p = s; *p; p++) {
            if (*p == '\'') buf_cat(&b, "'\"'\"'");
            else buf_char(&b, *p);
        }
    }
    buf_char(&b, '\'');
    return b.data;
}

static bool dsc_command_read_all(const char *cmd, char *out, size_t out_len) {
    if (!cmd || !out || out_len == 0) return false;
    out[0] = '\0';

    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    size_t got = fread(out, 1, out_len - 1, fp);
    out[got] = '\0';
    int rc = pclose(fp);
    return rc == 0 && got > 0;
}

static bool dsc_extract_oauth_bundle(const char *json, dsc_oauth_bundle_t *bundle) {
    if (!json || !bundle) return false;

    char *oauth = jraw(json, "claudeAiOauth");
    if (!oauth) return false;

    char *access = jstr(oauth, "accessToken");
    if (!access || !access[0]) {
        free(access);
        free(oauth);
        return false;
    }

    char *refresh = jstr(oauth, "refreshToken");
    char *expires_raw = jraw(oauth, "expiresAt");

    snprintf(bundle->access_token, sizeof(bundle->access_token), "%s", access);
    if (refresh && refresh[0])
        snprintf(bundle->refresh_token, sizeof(bundle->refresh_token), "%s", refresh);
    bundle->expires_at_ms = expires_raw ? atoll(expires_raw) : 0;
    bundle->storage_json = strdup(json);
    bundle->oauth_json = oauth;

    free(access);
    free(refresh);
    free(expires_raw);
    return bundle->storage_json != NULL;
}

static bool dsc_load_oauth_from_keychain(dsc_oauth_bundle_t *bundle) {
#ifdef __APPLE__
    char service[128];
    char account[128];
    dsc_build_claude_code_service_name(service, sizeof(service));
    dsc_get_username(account, sizeof(account));

    char *q_service = dsc_shell_quote(service);
    char *q_account = dsc_shell_quote(account);
    char json[8192];
    bool ok = false;

    if (q_service && q_account) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "security find-generic-password -a %s -s %s -w 2>/dev/null",
                 q_account, q_service);
        if (dsc_command_read_all(cmd, json, sizeof(json)) &&
            dsc_extract_oauth_bundle(json, bundle)) {
            bundle->source = DSC_OAUTH_SOURCE_KEYCHAIN;
            snprintf(bundle->keychain_service, sizeof(bundle->keychain_service), "%s", service);
            snprintf(bundle->keychain_account, sizeof(bundle->keychain_account), "%s", account);
            ok = true;
        }
    }

    if (!ok && q_service) {
        char cmd[384];
        snprintf(cmd, sizeof(cmd),
                 "security find-generic-password -s %s -w 2>/dev/null",
                 q_service);
        if (dsc_command_read_all(cmd, json, sizeof(json)) &&
            dsc_extract_oauth_bundle(json, bundle)) {
            bundle->source = DSC_OAUTH_SOURCE_KEYCHAIN;
            snprintf(bundle->keychain_service, sizeof(bundle->keychain_service), "%s", service);
            snprintf(bundle->keychain_account, sizeof(bundle->keychain_account), "%s", account);
            ok = true;
        }
    }

    free(q_service);
    free(q_account);
    return ok;
#else
    (void)bundle;
    return false;
#endif
}

static bool dsc_load_oauth_from_file(dsc_oauth_bundle_t *bundle) {
    char creds_path[1024];
    dsc_build_claude_code_credentials_path(creds_path, sizeof(creds_path));

    char *json = dsc_read_text_file(creds_path);
    if (!json) return false;
    bool ok = dsc_extract_oauth_bundle(json, bundle);
    free(json);
    if (!ok) return false;

    bundle->source = DSC_OAUTH_SOURCE_FILE;
    snprintf(bundle->credentials_path, sizeof(bundle->credentials_path), "%s", creds_path);
    return true;
}

static bool dsc_load_oauth_bundle(dsc_oauth_bundle_t *bundle) {
    dsc_oauth_bundle_init(bundle);

    const char *env = getenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN");
    if (env && env[0]) {
        bundle->source = DSC_OAUTH_SOURCE_ENV;
        snprintf(bundle->access_token, sizeof(bundle->access_token), "%s", env);
        return true;
    }
    env = getenv("CLAUDE_CODE_OAUTH_TOKEN");
    if (env && env[0]) {
        bundle->source = DSC_OAUTH_SOURCE_ENV;
        snprintf(bundle->access_token, sizeof(bundle->access_token), "%s", env);
        return true;
    }

    if (dsc_env_truthy(getenv("DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY")))
        return false;

    if (dsc_load_oauth_from_keychain(bundle)) return true;
    if (dsc_load_oauth_from_file(bundle)) return true;
    dsc_oauth_bundle_free(bundle);
    return false;
}

static char *dsc_default_scope_json_array(void) {
    buf_t b; buf_init(&b);
    const char *scopes[] = {
        "user:profile",
        "user:inference",
        "user:sessions:claude_code",
        "user:mcp_servers",
        "user:file_upload",
        NULL
    };
    buf_char(&b, '[');
    for (int i = 0; scopes[i]; i++) {
        if (i) buf_char(&b, ',');
        buf_json_str(&b, scopes[i]);
    }
    buf_char(&b, ']');
    return b.data;
}

static char *dsc_build_refreshed_oauth_json(const dsc_oauth_bundle_t *bundle,
                                            const char *scope_string) {
    char *scopes_raw = bundle->oauth_json ? jraw(bundle->oauth_json, "scopes") : NULL;
    char *subscription_type = bundle->oauth_json ? jstr(bundle->oauth_json, "subscriptionType") : NULL;
    char *rate_limit_tier = bundle->oauth_json ? jstr(bundle->oauth_json, "rateLimitTier") : NULL;
    char *default_scopes = NULL;

    buf_t out; buf_init(&out);
    buf_char(&out, '{');
    buf_json_str(&out, "accessToken");
    buf_cat(&out, ":");
    buf_json_str(&out, bundle->access_token);
    buf_cat(&out, ",");
    buf_json_str(&out, "refreshToken");
    buf_cat(&out, ":");
    buf_json_str(&out, bundle->refresh_token);
    buf_cat(&out, ",");
    buf_json_str(&out, "expiresAt");
    buf_printf(&out, ":%lld", bundle->expires_at_ms);
    buf_cat(&out, ",");
    buf_json_str(&out, "scopes");
    buf_cat(&out, ":");
    if (scope_string && scope_string[0]) {
        buf_char(&out, '[');
        const char *cur = scope_string;
        bool first = true;
        while (*cur) {
            while (*cur == ' ') cur++;
            if (!*cur) break;
            const char *end = strchr(cur, ' ');
            if (!end) end = cur + strlen(cur);
            if (!first) buf_char(&out, ',');
            char scope[128];
            size_t n = (size_t)(end - cur);
            if (n >= sizeof(scope)) n = sizeof(scope) - 1;
            memcpy(scope, cur, n);
            scope[n] = '\0';
            buf_json_str(&out, scope);
            first = false;
            cur = end;
        }
        buf_char(&out, ']');
    } else if (scopes_raw && scopes_raw[0]) {
        buf_cat(&out, scopes_raw);
    } else {
        default_scopes = dsc_default_scope_json_array();
        buf_cat(&out, default_scopes);
    }
    if (subscription_type && subscription_type[0]) {
        buf_cat(&out, ",");
        buf_json_str(&out, "subscriptionType");
        buf_cat(&out, ":");
        buf_json_str(&out, subscription_type);
    }
    if (rate_limit_tier && rate_limit_tier[0]) {
        buf_cat(&out, ",");
        buf_json_str(&out, "rateLimitTier");
        buf_cat(&out, ":");
        buf_json_str(&out, rate_limit_tier);
    }
    buf_char(&out, '}');

    free(scopes_raw);
    free(subscription_type);
    free(rate_limit_tier);
    free(default_scopes);
    return out.data;
}

static bool dsc_find_json_value_span(const char *json, const char *key,
                                     const char **out_start, const char **out_end) {
    if (!json || !key || !out_start || !out_end) return false;

    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{') return false;

    const char *start = p;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; *p; p++) {
        char ch = *p;
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') { in_string = true; continue; }
        if (ch == '{') depth++;
        else if (ch == '}') {
            depth--;
            if (depth == 0) {
                *out_start = start;
                *out_end = p + 1;
                return true;
            }
        }
    }
    return false;
}

static char *dsc_replace_oauth_json(const dsc_oauth_bundle_t *bundle, const char *new_oauth_json) {
    if (!bundle->storage_json || !bundle->storage_json[0] || !strchr(bundle->storage_json, '{')) {
        buf_t out; buf_init(&out);
        buf_cat(&out, "{\"claudeAiOauth\":");
        buf_cat(&out, new_oauth_json);
        buf_char(&out, '}');
        return out.data;
    }

    const char *value_start = NULL;
    const char *value_end = NULL;
    if (!dsc_find_json_value_span(bundle->storage_json, "claudeAiOauth", &value_start, &value_end)) {
        buf_t out; buf_init(&out);
        size_t len = strlen(bundle->storage_json);
        if (len > 0 && bundle->storage_json[len - 1] == '}') {
            buf_append(&out, bundle->storage_json, len - 1);
            if (len > 2) buf_char(&out, ',');
            buf_json_str(&out, "claudeAiOauth");
            buf_char(&out, ':');
            buf_cat(&out, new_oauth_json);
            buf_char(&out, '}');
            return out.data;
        }
        return strdup(bundle->storage_json);
    }

    buf_t out; buf_init(&out);
    buf_append(&out, bundle->storage_json, (size_t)(value_start - bundle->storage_json));
    buf_cat(&out, new_oauth_json);
    buf_cat(&out, value_end);
    return out.data;
}

static bool dsc_write_oauth_file(const char *path, const char *json) {
    if (!path || !path[0] || !json) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, fp) == len;
    fclose(fp);
    return ok;
}

static bool dsc_write_oauth_keychain(const dsc_oauth_bundle_t *bundle, const char *json) {
#ifdef __APPLE__
    if (!bundle || !json || !bundle->keychain_service[0]) return false;
    char *q_service = dsc_shell_quote(bundle->keychain_service);
    char *q_account = dsc_shell_quote(bundle->keychain_account[0]
                                      ? bundle->keychain_account
                                      : "claude-code-user");
    char *q_json = dsc_shell_quote(json);
    bool ok = false;
    if (q_service && q_account && q_json) {
        buf_t cmd; buf_init(&cmd);
        buf_cat(&cmd, "security add-generic-password -U -a ");
        buf_cat(&cmd, q_account);
        buf_cat(&cmd, " -s ");
        buf_cat(&cmd, q_service);
        buf_cat(&cmd, " -w ");
        buf_cat(&cmd, q_json);
        buf_cat(&cmd, " >/dev/null 2>&1");
        ok = system(cmd.data) == 0;
        buf_free(&cmd);
    }
    free(q_service);
    free(q_account);
    free(q_json);
    return ok;
#else
    (void)bundle;
    (void)json;
    return false;
#endif
}

static bool dsc_persist_oauth_bundle(const dsc_oauth_bundle_t *bundle, const char *scope_string) {
    if (!bundle || bundle->source == DSC_OAUTH_SOURCE_ENV) return true;

    char *oauth_json = dsc_build_refreshed_oauth_json(bundle, scope_string);
    char *storage_json = dsc_replace_oauth_json(bundle, oauth_json);
    free(oauth_json);
    if (!storage_json) return false;

    bool ok = false;
    if (bundle->source == DSC_OAUTH_SOURCE_KEYCHAIN)
        ok = dsc_write_oauth_keychain(bundle, storage_json);
    else if (bundle->source == DSC_OAUTH_SOURCE_FILE)
        ok = dsc_write_oauth_file(bundle->credentials_path, storage_json);
    free(storage_json);
    return ok;
}

static size_t dsc_http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    buf_t *buf = (buf_t *)userdata;
    size_t n = size * nmemb;
    buf_append(buf, ptr, n);
    return n;
}

static bool dsc_refresh_oauth_bundle(dsc_oauth_bundle_t *bundle) {
    if (!bundle || !bundle->refresh_token[0]) return false;

    const char *token_url = getenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN_URL");
    if (!token_url || !token_url[0]) token_url = CLAUDE_CODE_OAUTH_TOKEN_URL;
    const char *client_id = getenv("DSCO_CLAUDE_CODE_OAUTH_CLIENT_ID");
    if (!client_id || !client_id[0]) client_id = CLAUDE_CODE_OAUTH_CLIENT_ID;
    const char *scopes = getenv("DSCO_CLAUDE_CODE_OAUTH_SCOPES");
    if (!scopes || !scopes[0]) scopes = CLAUDE_CODE_OAUTH_SCOPES;

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    buf_t req; buf_init(&req);
    buf_cat(&req, "{\"grant_type\":\"refresh_token\",\"refresh_token\":");
    buf_json_str(&req, bundle->refresh_token);
    buf_cat(&req, ",\"client_id\":");
    buf_json_str(&req, client_id);
    buf_cat(&req, ",\"scope\":");
    buf_json_str(&req, scopes);
    buf_char(&req, '}');

    buf_t resp; buf_init(&resp);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, token_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dsc_http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    buf_free(&req);

    if (res != CURLE_OK || http_code != 200) {
        buf_free(&resp);
        return false;
    }

    char *access_token = jstr(resp.data, "access_token");
    char *refresh_token = jstr(resp.data, "refresh_token");
    char *scope_string = jstr(resp.data, "scope");
    int expires_in = jint(resp.data, "expires_in", -1);

    bool ok = access_token && access_token[0] && expires_in > 0;
    if (ok) {
        snprintf(bundle->access_token, sizeof(bundle->access_token), "%s", access_token);
        if (refresh_token && refresh_token[0])
            snprintf(bundle->refresh_token, sizeof(bundle->refresh_token), "%s", refresh_token);
        bundle->expires_at_ms = now_ms() + (long long)expires_in * 1000LL;
        (void)dsc_persist_oauth_bundle(bundle, scope_string);
    }

    free(access_token);
    free(refresh_token);
    free(scope_string);
    buf_free(&resp);
    return ok;
}

static const char *dsc_resolve_claude_code_oauth_token(void) {
    static char token[4096];
    token[0] = '\0';

    dsc_oauth_bundle_t bundle;
    if (!dsc_load_oauth_bundle(&bundle)) return NULL;

    if (bundle.source != DSC_OAUTH_SOURCE_ENV &&
        bundle.refresh_token[0] &&
        dsc_oauth_expired(bundle.expires_at_ms)) {
        (void)dsc_refresh_oauth_bundle(&bundle);
    }

    if (!bundle.access_token[0]) {
        dsc_oauth_bundle_free(&bundle);
        return NULL;
    }

    snprintf(token, sizeof(token), "%s", bundle.access_token);
    dsc_oauth_bundle_free(&bundle);
    return token;
}

/* ── Conversation history ──────────────────────────────────────────────── */

typedef struct {
    char *role;
    char *content;  /* JSON content array */
} msg_t;

static msg_t g_msgs[MAX_TURNS];
static int   g_msg_count = 0;

static void conv_drop_oldest(void) {
    if (g_msg_count < 2) return;
    free(g_msgs[0].role); free(g_msgs[0].content);
    free(g_msgs[1].role); free(g_msgs[1].content);
    memmove(&g_msgs[0], &g_msgs[2], sizeof(msg_t) * (size_t)(g_msg_count - 2));
    g_msg_count -= 2;
}

static void conv_add_user(const char *text) {
    if (g_msg_count >= MAX_TURNS) conv_drop_oldest();
    buf_t b; buf_init(&b);
    buf_cat(&b, "[{\"type\":\"text\",\"text\":");
    buf_json_str(&b, text);
    buf_cat(&b, "}]");
    g_msgs[g_msg_count].role = strdup("user");
    g_msgs[g_msg_count].content = b.data;
    g_msg_count++;
}

static void conv_add_assistant(const char *content_json) {
    if (g_msg_count >= MAX_TURNS) conv_drop_oldest();
    g_msgs[g_msg_count].role = strdup("assistant");
    g_msgs[g_msg_count].content = strdup(content_json);
    g_msg_count++;
}

static void conv_add_tool_result(const char *tool_use_id, const char *result) {
    if (g_msg_count >= MAX_TURNS) conv_drop_oldest();
    buf_t b; buf_init(&b);
    buf_cat(&b, "[{\"type\":\"tool_result\",\"tool_use_id\":");
    buf_json_str(&b, tool_use_id);
    buf_cat(&b, ",\"content\":");
    buf_json_str(&b, result);
    buf_cat(&b, "}]");
    g_msgs[g_msg_count].role = strdup("user");
    g_msgs[g_msg_count].content = b.data;
    g_msg_count++;
}

static char *conv_build_messages(void) {
    buf_t b; buf_init(&b);
    buf_char(&b, '[');
    for (int i = 0; i < g_msg_count; i++) {
        if (i > 0) buf_char(&b, ',');
        buf_cat(&b, "{\"role\":");
        buf_json_str(&b, g_msgs[i].role);
        buf_cat(&b, ",\"content\":");
        buf_cat(&b, g_msgs[i].content);
        buf_char(&b, '}');
    }
    buf_char(&b, ']');
    return b.data;
}

/* ── Task queue (model-driven, Claude Code style) ──────────────────────── */

#define TASK_MAX         128
#define TASK_TITLE_LEN   192
#define TASK_ACT_LEN     128
#define AGENT_MAX        8
#define CHILD_TAIL_CAP   4096

typedef enum {
    TS_PENDING = 0,
    TS_RUNNING,
    TS_DONE,
    TS_FAILED,
} task_state_t;

typedef struct {
    int          id;
    task_state_t state;
    char         title[TASK_TITLE_LEN];
    char         activity[TASK_ACT_LEN];
    long long    created_ms;
    long long    started_ms;
    long long    completed_ms;
    int          assigned_agent;   /* -1 = main, 0..AGENT_MAX-1 = child slot */
} task_t;

typedef struct {
    pid_t     pid;
    int       stdout_fd;           /* read end of child stdout pipe */
    int       task_id;             /* which task this child is executing */
    long long started_ms;
    int       in_tok, out_tok;
    double    cost;
    bool      active;
    bool      eof;
    char      tail[CHILD_TAIL_CAP];
    size_t    tail_len;
} agent_child_t;

static task_t        g_tasks[TASK_MAX];
static int           g_task_count   = 0;
static int           g_next_task_id = 1;

static agent_child_t g_children[AGENT_MAX];
static int           g_children_active = 0;

static bool          g_panel_enabled   = true;
static int           g_panel_rows      = 0;
static long long     g_session_start_ms = 0;
static long long     g_last_paint_ms    = 0;
static bool          g_scroll_region_set = false;
static bool          g_oneshot_mode     = false;
static char          g_current_activity[TASK_ACT_LEN] = "";

/* Forward declaration — panel_paint is used by tool handlers below. */
static void panel_paint(bool force);
static void panel_reset(void);
static void children_poll(int timeout_ms);
static void children_reap(void);

/* ── Task helpers ─────────────────────────────────────────────────────── */

static long long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

static const char *task_state_name(task_state_t s) {
    switch (s) {
        case TS_PENDING: return "pending";
        case TS_RUNNING: return "running";
        case TS_DONE:    return "done";
        case TS_FAILED:  return "failed";
    }
    return "?";
}

static task_t *task_find(int id) {
    for (int i = 0; i < g_task_count; i++)
        if (g_tasks[i].id == id) return &g_tasks[i];
    return NULL;
}

/* Append a task in PENDING state. Returns its id, or -1 if full. */
static int task_add(const char *title) {
    if (g_task_count >= TASK_MAX) return -1;
    task_t *t = &g_tasks[g_task_count++];
    memset(t, 0, sizeof(*t));
    t->id = g_next_task_id++;
    t->state = TS_PENDING;
    if (title) {
        strncpy(t->title, title, TASK_TITLE_LEN - 1);
        t->title[TASK_TITLE_LEN - 1] = '\0';
    }
    t->activity[0] = '\0';
    t->created_ms = mono_ms();
    t->assigned_agent = -1;
    return t->id;
}

static bool task_set_state(int id, task_state_t st) {
    task_t *t = task_find(id);
    if (!t) return false;
    if (t->state == st) return true;
    long long now = mono_ms();
    t->state = st;
    if (st == TS_RUNNING && t->started_ms == 0) t->started_ms = now;
    if (st == TS_DONE || st == TS_FAILED)       t->completed_ms = now;
    return true;
}

static bool task_set_activity(int id, const char *act) {
    task_t *t = task_find(id);
    if (!t || !act) return false;
    strncpy(t->activity, act, TASK_ACT_LEN - 1);
    t->activity[TASK_ACT_LEN - 1] = '\0';
    /* Collapse newlines — activity is single-line */
    for (char *p = t->activity; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    return true;
}

static void task_rollup(int *pending, int *running, int *done, int *failed) {
    int p = 0, r = 0, d = 0, f = 0;
    for (int i = 0; i < g_task_count; i++) {
        switch (g_tasks[i].state) {
            case TS_PENDING: p++; break;
            case TS_RUNNING: r++; break;
            case TS_DONE:    d++; break;
            case TS_FAILED:  f++; break;
        }
    }
    if (pending) *pending = p;
    if (running) *running = r;
    if (done)    *done    = d;
    if (failed)  *failed  = f;
}

/* ── Tool definitions ──────────────────────────────────────────────────── */

static const char *TOOLS_JSON =
    "["
    "{\"name\":\"shell\","
    "\"description\":\"Execute a shell command and return stdout+stderr. Use for running programs, checking system state, fetching URLs with curl, etc.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell command to run\"}},\"required\":[\"command\"]}},"
    "{\"name\":\"read_file\","
    "\"description\":\"Read a file's contents. Returns the full text.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"write_file\","
    "\"description\":\"Write content to a file (creates or overwrites).\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"content\":{\"type\":\"string\",\"description\":\"File content\"}},\"required\":[\"path\",\"content\"]}}"
    "]";

/* ── Tool execution ────────────────────────────────────────────────────── */

static char *exec_shell(const char *command) {
    outf("\033[36m  $ %s\033[0m\n", command);
    /* Run the command from a temp script so heredocs/multiline shells survive intact. */
    char script_path[] = "/tmp/dsc_shell_XXXXXX";
    int fd = mkstemp(script_path);
    if (fd < 0) return strdup("error: mkstemp failed");

    size_t len = strlen(command);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, command + off, len - off);
        if (n <= 0) {
            close(fd);
            unlink(script_path);
            return strdup("error: failed to write temp script");
        }
        off += (size_t)n;
    }
    if (len == 0 || command[len - 1] != '\n') {
        if (write(fd, "\n", 1) != 1) {
            close(fd);
            unlink(script_path);
            return strdup("error: failed to finalize temp script");
        }
    }
    close(fd);

    buf_t cmd; buf_init(&cmd);
    buf_printf(&cmd, "/bin/sh '%s' 2>&1", script_path);
    FILE *fp = popen(cmd.data, "r");
    buf_free(&cmd);
    if (!fp) {
        unlink(script_path);
        return strdup("error: popen failed");
    }
    buf_t b; buf_init(&b);
    char chunk[4096];
    while (fgets(chunk, sizeof(chunk), fp))
        buf_cat(&b, chunk);
    int status = pclose(fp);
    unlink(script_path);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        buf_printf(&b, "\n[exit code: %d]", WEXITSTATUS(status));
    if (b.len > 50000) {
        b.data[50000] = '\0'; b.len = 50000;
        buf_cat(&b, "\n...[truncated]");
    }
    return b.data;
}

static char *exec_read_file(const char *path) {
    outf("\033[2m  read: %s\033[0m\n", path);
    FILE *fp = fopen(path, "r");
    if (!fp) { char e[512]; snprintf(e, sizeof(e), "error: cannot open '%s'", path); return strdup(e); }
    buf_t b; buf_init(&b);
    char chunk[8192]; size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) buf_append(&b, chunk, n);
    fclose(fp);
    if (b.len > 80000) {
        b.data[80000] = '\0'; b.len = 80000;
        buf_cat(&b, "\n...[truncated]");
    }
    return b.data;
}

static char *exec_write_file(const char *path, const char *content) {
    outf("\033[2m  write: %s (%zu bytes)\033[0m\n", path, strlen(content));
    FILE *fp = fopen(path, "w");
    if (!fp) { char e[512]; snprintf(e, sizeof(e), "error: cannot write '%s'", path); return strdup(e); }
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
    char ok[512]; snprintf(ok, sizeof(ok), "wrote %zu bytes to %s", strlen(content), path);
    return strdup(ok);
}

static char *execute_tool(const char *name, const char *input_json) {
    if (strcmp(name, "shell") == 0) {
        char *cmd = jstr(input_json, "command");
        if (!cmd) return strdup("error: missing 'command'");
        char *r = exec_shell(cmd); free(cmd); return r;
    }
    if (strcmp(name, "read_file") == 0) {
        char *p = jstr(input_json, "path");
        if (!p) return strdup("error: missing 'path'");
        char *r = exec_read_file(p); free(p); return r;
    }
    if (strcmp(name, "write_file") == 0) {
        char *p = jstr(input_json, "path");
        char *c = jstr(input_json, "content");
        if (!p || !c) { free(p); free(c); return strdup("error: missing path/content"); }
        char *r = exec_write_file(p, c); free(p); free(c); return r;
    }
    char err[256]; snprintf(err, sizeof(err), "error: unknown tool '%s'", name);
    return strdup(err);
}

/* ── Streaming Markdown + LaTeX Renderer ──────────────────────────────── */

/* ANSI escape helper — only emits when color is enabled */
static void md_ansi(const char *seq) { if (g_color) out(seq); }

static const struct { const char *cmd; const char *sym; } g_latex[] = {
    /* ── Greek lowercase ── */
    {"alpha","α"},{"beta","β"},{"gamma","γ"},{"delta","δ"},{"epsilon","ε"},
    {"varepsilon","ε"},{"zeta","ζ"},{"eta","η"},{"theta","θ"},{"vartheta","ϑ"},
    {"iota","ι"},{"kappa","κ"},{"varkappa","ϰ"},{"lambda","λ"},{"mu","μ"},
    {"nu","ν"},{"xi","ξ"},{"pi","π"},{"varpi","ϖ"},{"rho","ρ"},{"varrho","ϱ"},
    {"sigma","σ"},{"varsigma","ς"},{"tau","τ"},{"upsilon","υ"},{"phi","φ"},
    {"varphi","φ"},{"chi","χ"},{"psi","ψ"},{"omega","ω"},{"digamma","ϝ"},
    /* ── Greek uppercase ── */
    {"Gamma","Γ"},{"Delta","Δ"},{"Theta","Θ"},{"Lambda","Λ"},{"Xi","Ξ"},
    {"Pi","Π"},{"Sigma","Σ"},{"Upsilon","Υ"},{"Phi","Φ"},{"Psi","Ψ"},{"Omega","Ω"},
    /* ── Hebrew ── */
    {"aleph","ℵ"},{"beth","ℶ"},{"gimel","ℷ"},{"daleth","ℸ"},
    /* ── Binary operators ── */
    {"pm","±"},{"mp","∓"},{"times","×"},{"div","÷"},{"cdot","·"},{"ast","∗"},
    {"star","⋆"},{"circ","∘"},{"bullet","•"},{"oplus","⊕"},{"otimes","⊗"},
    {"odot","⊙"},{"ominus","⊖"},{"oslash","⊘"},{"boxplus","⊞"},
    {"boxminus","⊟"},{"boxtimes","⊠"},{"boxdot","⊡"},
    {"amalg","⨿"},{"wr","≀"},{"diamond","⋄"},{"bigtriangleup","△"},
    {"bigtriangledown","▽"},{"triangleleft","◁"},{"triangleright","▷"},
    {"lhd","⊲"},{"rhd","⊳"},{"unlhd","⊴"},{"unrhd","⊵"},
    {"ltimes","⋉"},{"rtimes","⋊"},{"bowtie","⋈"},{"Join","⨝"},
    {"dotplus","∔"},{"centerdot","·"},{"intercal","⊺"},{"barwedge","⊼"},
    {"veebar","⊻"},{"doublebarwedge","⩞"},{"curlywedge","⋏"},
    {"curlyvee","⋎"},{"circleddash","⊝"},{"circledast","⊛"},{"circledcirc","⊚"},
    /* ── Relations ── */
    {"leq","≤"},{"le","≤"},{"geq","≥"},{"ge","≥"},{"neq","≠"},{"ne","≠"},
    {"approx","≈"},{"equiv","≡"},{"sim","∼"},{"simeq","≃"},{"cong","≅"},
    {"propto","∝"},{"ll","≪"},{"gg","≫"},{"prec","≺"},{"succ","≻"},
    {"preceq","⪯"},{"succeq","⪰"},{"preccurlyeq","≼"},{"succcurlyeq","≽"},
    {"precsim","≾"},{"succsim","≿"},{"doteq","≐"},{"circeq","≗"},
    {"triangleq","≜"},{"bumpeq","≏"},{"Bumpeq","≎"},{"fallingdotseq","≒"},
    {"risingdotseq","≓"},{"backsim","∽"},{"backsimeq","⋍"},{"asymp","≍"},
    {"vdash","⊢"},{"dashv","⊣"},{"Vdash","⊩"},{"vDash","⊨"},{"Vvdash","⊪"},
    {"models","⊧"},{"therefore","∴"},{"because","∵"},{"between","≬"},
    {"pitchfork","⋔"},{"smile","⌣"},{"frown","⌢"},{"bowtie","⋈"},
    {"sqsubset","⊏"},{"sqsupset","⊐"},{"sqsubseteq","⊑"},{"sqsupseteq","⊒"},
    /* ── Negated relations ── */
    {"nless","≮"},{"ngtr","≯"},{"nleq","≰"},{"ngeq","≱"},{"nprec","⊀"},
    {"nsucc","⊁"},{"nsubseteq","⊈"},{"nsupseteq","⊉"},{"nparallel","∦"},
    {"nvdash","⊬"},{"nvDash","⊭"},{"nVdash","⊮"},{"nVDash","⊯"},
    {"ntriangleleft","⋪"},{"ntriangleright","⋫"},{"ntrianglelefteq","⋬"},
    {"ntrianglerighteq","⋭"},{"ncong","≇"},{"nsim","≁"},
    /* ── Set theory ── */
    {"in","∈"},{"notin","∉"},{"ni","∋"},{"subset","⊂"},{"supset","⊃"},
    {"subseteq","⊆"},{"supseteq","⊇"},{"subsetneq","⊊"},{"supsetneq","⊋"},
    {"cup","∪"},{"cap","∩"},{"emptyset","∅"},{"varnothing","∅"},{"setminus","∖"},
    {"complement","∁"},{"sqcup","⊔"},{"sqcap","⊓"},{"uplus","⊎"},
    /* ── Logic ── */
    {"forall","∀"},{"exists","∃"},{"nexists","∄"},{"neg","¬"},{"lnot","¬"},
    {"land","∧"},{"lor","∨"},{"implies","⟹"},{"iff","⟺"},
    {"top","⊤"},{"bot","⊥"},{"vdots","⋮"},
    /* ── Arrows ── */
    {"rightarrow","→"},{"to","→"},{"leftarrow","←"},{"leftrightarrow","↔"},
    {"Rightarrow","⇒"},{"Leftarrow","⇐"},{"Leftrightarrow","⇔"},
    {"uparrow","↑"},{"downarrow","↓"},{"Uparrow","⇑"},{"Downarrow","⇓"},
    {"Updownarrow","⇕"},{"updownarrow","↕"},{"mapsto","↦"},
    {"nearrow","↗"},{"searrow","↘"},{"nwarrow","↖"},{"swarrow","↙"},
    {"hookrightarrow","↪"},{"hookleftarrow","↩"},
    {"longrightarrow","⟶"},{"longleftarrow","⟵"},{"longmapsto","⟼"},
    {"Longrightarrow","⟹"},{"Longleftarrow","⟸"},{"Longleftrightarrow","⟺"},
    {"rightrightarrows","⇉"},{"leftleftarrows","⇇"},
    {"rightleftarrows","⇄"},{"leftrightarrows","⇆"},
    {"rightharpoonup","⇀"},{"rightharpoondown","⇁"},
    {"leftharpoonup","↼"},{"leftharpoondown","↽"},
    {"upharpoonright","↾"},{"upharpoonleft","↿"},
    {"downharpoonright","⇂"},{"downharpoonleft","⇃"},
    {"rightarrowtail","↣"},{"leftarrowtail","↢"},
    {"twoheadrightarrow","↠"},{"twoheadleftarrow","↞"},
    {"curvearrowright","↷"},{"curvearrowleft","↶"},
    {"circlearrowright","↻"},{"circlearrowleft","↺"},
    {"Lsh","↰"},{"Rsh","↱"},{"looparrowright","↬"},{"looparrowleft","↫"},
    {"multimap","⊸"},{"leadsto","⇝"},
    {"nleftarrow","↚"},{"nrightarrow","↛"},{"nLeftarrow","⇍"},
    {"nRightarrow","⇏"},{"nLeftrightarrow","⇎"},{"nleftrightarrow","↮"},
    /* ── Big operators ── */
    {"sum","∑"},{"prod","∏"},{"coprod","∐"},{"int","∫"},{"iint","∬"},
    {"iiint","∭"},{"oint","∮"},{"oiint","∯"},{"oiiint","∰"},
    {"bigcup","⋃"},{"bigcap","⋂"},{"bigsqcup","⨆"},
    {"bigoplus","⨁"},{"bigotimes","⨂"},{"bigodot","⨀"},
    {"biguplus","⨄"},{"bigvee","⋁"},{"bigwedge","⋀"},
    /* ── Misc math ── */
    {"infty","∞"},{"partial","∂"},{"nabla","∇"},{"surd","√"},
    {"triangle","△"},{"angle","∠"},{"measuredangle","∡"},
    {"sphericalangle","∢"},
    {"perp","⊥"},{"parallel","∥"},{"mid","∣"},{"nmid","∤"},
    {"prime","′"},{"dprime","″"},{"backprime","‵"},
    {"hbar","ℏ"},{"ell","ℓ"},{"Re","ℜ"},{"Im","ℑ"},{"wp","℘"},
    {"mho","℧"},{"Finv","Ⅎ"},{"Game","⅁"},{"eth","ð"},
    {"therefore","∴"},{"because","∵"},
    /* ── Delimiters ── */
    {"langle","⟨"},{"rangle","⟩"},{"lceil","⌈"},{"rceil","⌉"},
    {"lfloor","⌊"},{"rfloor","⌋"},{"lbrace","{"},{"rbrace","}"},
    {"lVert","‖"},{"rVert","‖"},{"lvert","|"},{"rvert","|"},
    {"ulcorner","⌜"},{"urcorner","⌝"},{"llcorner","⌞"},{"lrcorner","⌟"},
    {"lgroup","⟮"},{"rgroup","⟯"},
    /* ── Sizing (consumed silently) ── */
    {"left",""},{"right",""},{"big",""},{"Big",""},{"bigg",""},{"Bigg",""},
    {"bigl",""},{"bigr",""},{"Bigl",""},{"Bigr",""},
    {"biggl",""},{"biggr",""},{"Biggl",""},{"Biggr",""},
    {"middle",""},
    /* ── Dots ── */
    {"dots","…"},{"ldots","…"},{"cdots","⋯"},{"vdots","⋮"},{"ddots","⋱"},
    {"iddots","⋰"},
    /* ── Spacing ── */
    {"quad","  "},{"qquad","    "},{"enspace"," "},{"thinspace"," "},
    {"negthinspace",""},{"negmedspace",""},{"negthickspace",""},
    /* ── Card / music / misc symbols ── */
    {"clubsuit","♣"},{"diamondsuit","♢"},{"heartsuit","♡"},{"spadesuit","♠"},
    {"flat","♭"},{"natural","♮"},{"sharp","♯"},
    {"checkmark","✓"},{"maltese","✠"},{"dag","†"},{"ddag","‡"},
    {"S","§"},{"P","¶"},{"copyright","©"},{"registered","®"},{"trademark","™"},
    {"yen","¥"},{"pounds","£"},{"euro","€"},{"cent","¢"},
    {"aa","å"},{"AA","Å"},{"ae","æ"},{"AE","Æ"},{"oe","œ"},{"OE","Œ"},
    {"ss","ß"},{"o","ø"},{"O","Ø"},
    /* ── Roots ── */
    {"sqrt","√"},{"cbrt","∛"},{"fourthroot","∜"},
    /* ── Accents (standalone combining chars for abort fallback) ── */
    {"hat","̂"},{"tilde","̃"},{"bar","̄"},{"dot","̇"},{"ddot","̈"},
    {"dddot","⃛"},{"vec","⃗"},{"check","̌"},{"breve","̆"},
    {"acute","́"},{"grave","̀"},{"ring","̊"},{"widetilde","̃"},{"widehat","̂"},
    /* ── Degree / misc ── */
    {"degree","°"},{"dagger","†"},{"ddagger","‡"},{"circ","∘"},
    {"lozenge","◊"},{"blacklozenge","⧫"},{"bigcirc","◯"},
    {"Box","□"},{"Diamond","◇"},{"blacksquare","■"},{"blacktriangle","▲"},
    {"blacktriangledown","▼"},{"blacktriangleleft","◀"},
    {"blacktriangleright","▶"},{"square","□"},
    /* ── Zodiac (U+2648–U+2653, systematic: base + sign index) ── */
    {"aries","♈"},{"taurus","♉"},{"gemini","♊"},{"cancer","♋"},
    {"leo","♌"},{"virgo","♍"},{"libra","♎"},{"scorpio","♏"},
    {"sagittarius","♐"},{"capricorn","♑"},{"aquarius","♒"},{"pisces","♓"},
    /* ── Planets / astronomical ── */
    {"mercury","☿"},{"venus","♀"},{"earth","♁"},{"mars","♂"},
    {"jupiter","♃"},{"saturn","♄"},{"uranus","♅"},{"neptune","♆"},{"pluto","♇"},
    {"conjunction","☌"},{"opposition","☍"},{"ascnode","☊"},{"descnode","☋"},
    /* ── Chess pieces (U+2654–U+265F, systematic: white then black) ── */
    {"wking","♔"},{"wqueen","♕"},{"wrook","♖"},{"wbishop","♗"},
    {"wknight","♘"},{"wpawn","♙"},
    {"bking","♚"},{"bqueen","♛"},{"brook","♜"},{"bbishop","♝"},
    {"bknight","♞"},{"bpawn","♟"},
    /* ── Dice (U+2680–U+2685, systematic: base + face) ── */
    {"diceone","⚀"},{"dicetwo","⚁"},{"dicethree","⚂"},
    {"dicefour","⚃"},{"dicefive","⚄"},{"dicesix","⚅"},
    /* ── I Ching trigrams (U+2630–U+2637) ── */
    {"triheaven","☰"},{"trilake","☱"},{"trifire","☲"},{"trithunder","☳"},
    {"triwind","☴"},{"triwater","☵"},{"trimountain","☶"},{"triearth","☷"},
    /* ── Weather ── */
    {"sun","☀"},{"cloud","☁"},{"umbrella","☂"},{"snowman","☃"},{"comet","☄"},
    {"rain","☔"},{"snowflake","❄"},{"lightning","⚡"},{"thunder","⛈"},
    {"tornado","🌪"},{"fog","🌫"},{"wind","🌬"},{"thermometer","🌡"},
    {"sunrise","🌅"},{"rainbow","🌈"},
    /* ── Music (U+1D100 block + misc) ── */
    {"trebleclef","𝄞"},{"altoclef","𝄡"},{"bassclef","𝄢"},
    {"quarternote","♩"},{"eighthnote","♪"},{"beamednotes","♫"},
    {"sixteenthnote","♬"},{"coda","𝄌"},{"segno","𝄋"},{"fermata","𝄐"},
    {"wholerest","𝄻"},{"halfrest","𝄼"},{"quarterrest","𝄽"},
    /* ── Box drawing (U+2500–U+257F, systematic intersection grid) ── */
    {"boxh","─"},{"boxH","━"},{"boxv","│"},{"boxV","┃"},
    {"boxdr","┌"},{"boxdR","┍"},{"boxDr","┎"},{"boxDR","┏"},
    {"boxdl","┐"},{"boxdL","┑"},{"boxDl","┒"},{"boxDL","┓"},
    {"boxur","└"},{"boxuR","┕"},{"boxUr","┖"},{"boxUR","┗"},
    {"boxul","┘"},{"boxuL","┙"},{"boxUl","┚"},{"boxUL","┛"},
    {"boxvr","├"},{"boxVR","┣"},{"boxvl","┤"},{"boxVL","┫"},
    {"boxhd","┬"},{"boxHD","┳"},{"boxhu","┴"},{"boxHU","┻"},
    {"boxvh","┼"},{"boxVH","╋"},
    {"boxdh","═"},{"boxdv","║"},
    {"boxddr","╔"},{"boxddl","╗"},{"boxdur","╚"},{"boxdul","╝"},
    {"boxdvr","╠"},{"boxdvl","╣"},{"boxdhd","╦"},{"boxdhu","╩"},{"boxdvh","╬"},
    /* ── Block elements (U+2580–U+259F) ── */
    {"blockfull","█"},{"blocktop","▀"},{"blockbot","▄"},
    {"blockleft","▌"},{"blockright","▐"},
    {"shade1","░"},{"shade2","▒"},{"shade3","▓"},
    /* ── Electrical / technical ── */
    {"ohm","Ω"},{"micro","µ"},{"celsius","℃"},{"fahrenheit","℉"},
    {"angstrom","Å"},{"planck","ℎ"},{"euler","ℯ"},
    {"ground","⏚"},{"fuse","⏛"},{"power","⏻"},{"eject","⏏"},
    {"helm","⎈"},{"propeller","⌀"},{"projective","⌭"},
    {"benzene","⏣"},{"sinusoid","∿"},{"awareness","⏧"},
    /* ── Hazard / warning ── */
    {"skull","☠"},{"radioactive","☢"},{"biohazard","☣"},{"caduceus","☤"},
    {"warning","⚠"},{"highvoltage","⚡"},{"noentry","⛔"},
    /* ── Misc pictographs / symbols ── */
    {"peace","☮"},{"yinyang","☯"},{"smiley","☺"},{"frowney","☹"},
    {"female","♀"},{"male","♂"},{"atom","⚛"},{"recycle","♻"},
    {"wheelchair","♿"},{"anchor","⚓"},{"scales","⚖"},{"gear","⚙"},
    {"crossed","⚔"},{"flag","⚑"},{"hotsprings","♨"},{"telephone","☎"},
    {"envelope","✉"},{"airplane","✈"},{"hourglass","⌛"},{"watch","⌚"},
    {"pencil","✏"},{"scissors","✂"},{"pick","⛏"},{"link","🔗"},
    {"key","🔑"},{"lock","🔒"},{"bell","🔔"},{"magnify","🔍"},
    {"gem","💎"},{"fire","🔥"},{"globe","🌍"},{"tada","🎉"},
    /* ── Stars / decorative ── */
    {"bigstar","★"},{"whitestar","☆"},{"fourstar","✦"},{"sixstar","✡"},
    {"eightstar","✴"},{"sparkle","❇"},{"asterism","⁂"},
    {"fleuron","❧"},{"hedera","❦"},{"floralheart","❦"},
    /* ── Religious / cultural ── */
    {"cross","✝"},{"orthodoxcross","☦"},{"stardavid","✡"},
    {"crescent","☪"},{"dharma","☸"},{"om","🕉"},{"khanda","🪯"},
    /* ── Currency (extended) ── */
    {"bitcoin","₿"},{"rupee","₹"},{"ruble","₽"},{"won","₩"},
    {"dong","₫"},{"peso","₱"},{"lira","₺"},{"naira","₦"},
    {"baht","฿"},{"shekel","₪"},{"generic","¤"},
    /* ── Arrows (supplemental) ── */
    {"arrowup","⬆"},{"arrowdown","⬇"},{"arrowleft","⬅"},{"arrowright","➡"},
    {"arrowne","⬈"},{"arrowse","⬊"},{"arrownw","⬉"},{"arrowsw","⬋"},
    {"arrowreturn","↩"},{"arrowredo","↪"},{"arrowcw","↻"},{"arrowccw","↺"},
    {"arrowboth","⬌"},{"arrowupdown","⬍"},
    /* ── Transport / map ── */
    {"car","🚗"},{"bus","🚌"},{"train","🚂"},{"ship","🚢"},
    {"rocket","🚀"},{"satellite","🛰"},{"bicycle","🚲"},{"helicopter","🚁"},
    /* ── Hands / gestures ── */
    {"pointright","👉"},{"pointleft","👈"},{"pointup","👆"},
    {"pointdown","👇"},{"thumbsup","👍"},{"thumbsdown","👎"},
    {"wave","👋"},{"clap","👏"},{"pray","🙏"},
    /* ── Alchemical (U+1F700–U+1F77F) ── */
    {"alfire","🜂"},{"alwater","🜄"},{"alair","🜁"},{"alearth","🜃"},
    {"alsalt","🜔"},{"almercury","🜐"},{"alsulfur","🜍"},{"algold","🜚"},
    {"alsilver","🜛"},{"aliron","🜜"},{"alcopper","🜠"},{"altin","🜩"},
    {"allead","🜪"},{"alantimony","🜫"},
    {NULL,NULL}
};

/* ── UTF-8 encoder + codepoint emitter ─────────────────────────────────── */

static int utf8_encode(int cp, char *buf) {
    if (cp < 0x80)    { buf[0]=(char)cp; return 1; }
    if (cp < 0x800)   { buf[0]=(char)(0xC0|(cp>>6)); buf[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000) { buf[0]=(char)(0xE0|(cp>>12)); buf[1]=(char)(0x80|((cp>>6)&0x3F));
                         buf[2]=(char)(0x80|(cp&0x3F)); return 3; }
    if (cp < 0x110000){ buf[0]=(char)(0xF0|(cp>>18)); buf[1]=(char)(0x80|((cp>>12)&0x3F));
                         buf[2]=(char)(0x80|((cp>>6)&0x3F)); buf[3]=(char)(0x80|(cp&0x3F)); return 4; }
    return 0;
}

static void emit_cp(int cp) {
    char b[4]; int n = utf8_encode(cp, b);
    if (n > 0) out_raw(b, (size_t)n);
}

/* ── Mathematical font variants — algorithmic via Unicode block offsets ── */
/*
 * Unicode Mathematical Alphanumeric Symbols (U+1D400–U+1D7FF) are laid out
 * systematically: each font variant occupies a contiguous 26-letter (or 10-digit)
 * range.  We compute codepoints from base offsets rather than hard-coding each
 * character.  A small exception table covers the handful of letters that were
 * assigned earlier codepoints (e.g. ℂ, ℍ, ℕ, ℝ, ℤ for double-struck).
 */

typedef struct { const char *name; int upper, lower, digit; } mathfont_t;

static const mathfont_t g_mathfonts[] = {
    /* name             A-Z base    a-z base    0-9 base   */
    {"mathbf",          0x1D400,    0x1D41A,    0x1D7CE},
    {"boldsymbol",      0x1D400,    0x1D41A,    0x1D7CE},
    {"mathit",          0x1D434,    0x1D44E,    -1},
    {"mathbi",          0x1D468,    0x1D482,    -1},
    {"mathcal",         0x1D49C,    -1,         -1},
    {"mathscr",         0x1D49C,    -1,         -1},
    {"mathbscr",        0x1D4D0,    0x1D4EA,    -1},
    {"mathfrak",        0x1D504,    0x1D51E,    -1},
    {"mathbb",          0x1D538,    0x1D552,    0x1D7D8},
    {"mathbfrak",       0x1D56C,    0x1D586,    -1},
    {"mathsf",          0x1D5A0,    0x1D5BA,    0x1D7E2},
    {"mathsfbf",        0x1D5D4,    0x1D5EE,    0x1D7EC},
    {"mathsfit",        0x1D608,    0x1D622,    -1},
    {"mathsfbfit",      0x1D63C,    0x1D656,    -1},
    {"mathtt",          0x1D670,    0x1D68A,    0x1D7F6},
    {NULL, 0, 0, 0}
};

/* Characters that have pre-existing codepoints outside the math block */
typedef struct { const char *font; char ch; int cp; } fontex_t;
static const fontex_t g_fontex[] = {
    /* Script / calligraphic */
    {"mathcal",'B',0x212C},{"mathcal",'E',0x2130},{"mathcal",'F',0x2131},
    {"mathcal",'H',0x210B},{"mathcal",'I',0x2110},{"mathcal",'L',0x2112},
    {"mathcal",'M',0x2133},{"mathcal",'R',0x211B},
    {"mathscr",'B',0x212C},{"mathscr",'E',0x2130},{"mathscr",'F',0x2131},
    {"mathscr",'H',0x210B},{"mathscr",'I',0x2110},{"mathscr",'L',0x2112},
    {"mathscr",'M',0x2133},{"mathscr",'R',0x211B},
    /* Fraktur */
    {"mathfrak",'C',0x212D},{"mathfrak",'H',0x210C},{"mathfrak",'I',0x2111},
    {"mathfrak",'R',0x211C},{"mathfrak",'Z',0x2128},
    /* Double-struck */
    {"mathbb",'C',0x2102},{"mathbb",'H',0x210D},{"mathbb",'N',0x2115},
    {"mathbb",'P',0x2119},{"mathbb",'Q',0x211A},{"mathbb",'R',0x211D},
    {"mathbb",'Z',0x2124},
    /* Italic h (Planck constant) */
    {"mathit",'h',0x210E},
    {NULL,0,0}
};

static bool emit_mathfont_char(const char *font, char ch) {
    /* 1. Check exception table */
    for (int i = 0; g_fontex[i].font; i++)
        if (g_fontex[i].ch == ch && strcmp(g_fontex[i].font, font) == 0)
            { emit_cp(g_fontex[i].cp); return true; }
    /* 2. Compute from systematic offset */
    for (int i = 0; g_mathfonts[i].name; i++) {
        if (strcmp(g_mathfonts[i].name, font) != 0) continue;
        int cp = -1;
        if (ch >= 'A' && ch <= 'Z' && g_mathfonts[i].upper >= 0)
            cp = g_mathfonts[i].upper + (ch - 'A');
        else if (ch >= 'a' && ch <= 'z' && g_mathfonts[i].lower >= 0)
            cp = g_mathfonts[i].lower + (ch - 'a');
        else if (ch >= '0' && ch <= '9' && g_mathfonts[i].digit >= 0)
            cp = g_mathfonts[i].digit + (ch - '0');
        if (cp >= 0) { emit_cp(cp); return true; }
        return false;
    }
    return false;
}

/* ── Superscript / Subscript — codepoint tables (no systematic Unicode block) ── */

static const struct { char ch; int cp; } g_sup[] = {
    {'0',0x2070},{'1',0x00B9},{'2',0x00B2},{'3',0x00B3},{'4',0x2074},
    {'5',0x2075},{'6',0x2076},{'7',0x2077},{'8',0x2078},{'9',0x2079},
    {'+',0x207A},{'-',0x207B},{'=',0x207C},{'(',0x207D},{')',0x207E},
    {'i',0x2071},{'n',0x207F},
    {'a',0x1D43},{'b',0x1D47},{'c',0x1D9C},{'d',0x1D48},{'e',0x1D49},
    {'f',0x1DA0},{'g',0x1D4D},{'h',0x02B0},{'j',0x02B2},{'k',0x1D4F},
    {'l',0x02E1},{'m',0x1D50},{'o',0x1D52},{'p',0x1D56},{'r',0x02B3},
    {'s',0x02E2},{'t',0x1D57},{'u',0x1D58},{'v',0x1D5B},{'w',0x02B7},
    {'x',0x02E3},{'y',0x02B8},{'z',0x1DBB},
    {'A',0x1D2C},{'B',0x1D2E},{'D',0x1D30},{'E',0x1D31},{'G',0x1D33},
    {'H',0x1D34},{'I',0x1D35},{'J',0x1D36},{'K',0x1D37},{'L',0x1D38},
    {'M',0x1D39},{'N',0x1D3A},{'O',0x1D3C},{'P',0x1D3E},{'R',0x1D3F},
    {'T',0x1D40},{'U',0x1D41},{'V',0x2C7D},{'W',0x1D42},
    {0,0}
};

static const struct { char ch; int cp; } g_sub[] = {
    {'0',0x2080},{'1',0x2081},{'2',0x2082},{'3',0x2083},{'4',0x2084},
    {'5',0x2085},{'6',0x2086},{'7',0x2087},{'8',0x2088},{'9',0x2089},
    {'+',0x208A},{'-',0x208B},{'=',0x208C},{'(',0x208D},{')',0x208E},
    {'a',0x2090},{'e',0x2091},{'h',0x2095},{'i',0x1D62},{'j',0x2C7C},
    {'k',0x2096},{'l',0x2097},{'m',0x2098},{'n',0x2099},{'o',0x209A},
    {'p',0x209B},{'r',0x1D63},{'s',0x209C},{'t',0x209D},{'u',0x1D64},
    {'v',0x1D65},{'x',0x2093},
    {0,0}
};

static bool emit_sup(char c) {
    for (int i = 0; g_sup[i].ch; i++)
        if (g_sup[i].ch == c) { emit_cp(g_sup[i].cp); return true; }
    return false;
}

static bool emit_sub(char c) {
    for (int i = 0; g_sub[i].ch; i++)
        if (g_sub[i].ch == c) { emit_cp(g_sub[i].cp); return true; }
    return false;
}

static const char *latex_lookup(const char *cmd) {
    for (int i = 0; g_latex[i].cmd; i++)
        if (strcmp(g_latex[i].cmd, cmd) == 0) return g_latex[i].sym;
    return NULL;
}

/* Brace-argument LaTeX commands: \cmd{arg1}{arg2} */
enum {
    LCMD_NONE = 0, LCMD_FRAC, LCMD_SQRT, LCMD_TEXT, LCMD_TEXTBF,
    LCMD_TEXTIT, LCMD_MATHFONT, LCMD_BOXED, LCMD_OVERLINE,
    LCMD_UNDERLINE, LCMD_ACCENT, LCMD_UNICODE,
    LCMD_BRAILLE, LCMD_HEXAGRAM, LCMD_CARD
};

static int latex_brace_cmd(const char *cmd) {
    if (strcmp(cmd,"frac")==0||strcmp(cmd,"dfrac")==0||strcmp(cmd,"tfrac")==0)
        return LCMD_FRAC;
    if (strcmp(cmd,"sqrt")==0)       return LCMD_SQRT;
    if (strcmp(cmd,"text")==0||strcmp(cmd,"mathrm")==0||strcmp(cmd,"textrm")==0||
        strcmp(cmd,"operatorname")==0||strcmp(cmd,"textnormal")==0)
        return LCMD_TEXT;
    if (strcmp(cmd,"textbf")==0)     return LCMD_TEXTBF;
    if (strcmp(cmd,"textit")==0||strcmp(cmd,"emph")==0)
        return LCMD_TEXTIT;
    /* All math font variants → systematic Unicode computation */
    /* All math font variants → systematic Unicode block computation */
    for (int i = 0; g_mathfonts[i].name; i++)
        if (strcmp(cmd, g_mathfonts[i].name) == 0) return LCMD_MATHFONT;
    /* Arbitrary Unicode: \u{XXXX} */
    if (strcmp(cmd,"u")==0||strcmp(cmd,"U")==0) return LCMD_UNICODE;
    /* Algorithmic Unicode blocks */
    if (strcmp(cmd,"braille")==0)   return LCMD_BRAILLE;
    if (strcmp(cmd,"hexagram")==0||strcmp(cmd,"iching")==0) return LCMD_HEXAGRAM;
    if (strcmp(cmd,"card")==0)      return LCMD_CARD;
    if (strcmp(cmd,"boxed")==0||strcmp(cmd,"fbox")==0)
        return LCMD_BOXED;
    if (strcmp(cmd,"overline")==0)   return LCMD_OVERLINE;
    if (strcmp(cmd,"underline")==0)  return LCMD_UNDERLINE;
    if (strcmp(cmd,"hat")==0||strcmp(cmd,"widehat")==0||strcmp(cmd,"check")==0||
        strcmp(cmd,"tilde")==0||strcmp(cmd,"widetilde")==0||strcmp(cmd,"dot")==0||
        strcmp(cmd,"ddot")==0||strcmp(cmd,"dddot")==0||strcmp(cmd,"vec")==0||
        strcmp(cmd,"bar")==0||strcmp(cmd,"breve")==0||strcmp(cmd,"acute")==0||
        strcmp(cmd,"grave")==0||strcmp(cmd,"ring")==0)
        return LCMD_ACCENT;
    return LCMD_NONE;
}

static int latex_brace_argc(int cmd) { return (cmd == LCMD_FRAC) ? 2 : 1; }

typedef struct {
    int  stars, ticks, hashes, tildes, dollars;
    char latex[64];  int latexn;  bool in_latex;
    /* brace-arg collection for \frac{}{} etc. */
    int  brace_cmd;
    char ba1[512]; int ba1n;
    char ba2[512]; int ba2n;
    int  brace_depth, brace_which;
    bool brace_wait;
    /* active formatting */
    bool bold, italic, code, fence, header, strike, math;
    bool sol; int sol_dashes; bool sol_gt;
    char sol_digits[16]; int sol_dign; bool sol_dot;
    int  link_state;  /* 0=none 1=text 2=expect_paren 3=url */
    int  sup_sub;     /* 0=none 1=superscript 2=subscript (math mode) */
    bool sup_sub_brace;
    char fence_lang[32]; int fence_langn; bool fence_lang_col;
} md_t;

static md_t g_md;

static void md_begin_block(void) {
    memset(&g_md, 0, sizeof(g_md));
    g_md.sol = true;
}

/* forward decls */
static void md_resolve_stars(void);
static void md_resolve_ticks(void);

static void md_emit_brace_result(void) {
    g_md.ba1[g_md.ba1n] = '\0';
    g_md.ba2[g_md.ba2n] = '\0';
    switch (g_md.brace_cmd) {
    case LCMD_FRAC:
        out(g_md.ba1); out("/"); out(g_md.ba2); break;
    case LCMD_SQRT:
        out("√("); out(g_md.ba1); out(")"); break;
    case LCMD_TEXT:
        out(g_md.ba1); break;
    case LCMD_TEXTBF:
        md_ansi("\033[1m"); out(g_md.ba1); md_ansi("\033[22m"); break;
    case LCMD_TEXTIT:
        md_ansi("\033[3m"); out(g_md.ba1); md_ansi("\033[23m"); break;
    case LCMD_MATHFONT:
        for (int i = 0; i < g_md.ba1n; i++) {
            if (!emit_mathfont_char(g_md.latex, g_md.ba1[i]))
                out_raw(&g_md.ba1[i], 1);
        }
        break;
    case LCMD_BOXED:
        out("⌈"); out(g_md.ba1); out("⌉"); break;
    case LCMD_OVERLINE:
        for (int i = 0; i < g_md.ba1n; i++) {
            out_raw(&g_md.ba1[i], 1); out("\xCC\x85");
        }
        break;
    case LCMD_UNDERLINE:
        md_ansi("\033[4m"); out(g_md.ba1); md_ansi("\033[24m"); break;
    case LCMD_UNICODE: {
        g_md.ba1[g_md.ba1n] = '\0';
        int cp = (int)strtol(g_md.ba1, NULL, 16);
        if (cp > 0 && cp < 0x110000) emit_cp(cp);
        break;
    }
    case LCMD_BRAILLE: {
        /* \braille{12345678} → U+2800 + bitmask of dot positions
         * Dot layout: 1 4 / 2 5 / 3 6 / 7 8
         * Dot N → bit (N-1) in codepoint offset */
        int mask = 0;
        for (int i = 0; i < g_md.ba1n; i++) {
            int d = g_md.ba1[i] - '0';
            if (d >= 1 && d <= 8) mask |= (1 << (d - 1));
        }
        emit_cp(0x2800 + mask);
        break;
    }
    case LCMD_HEXAGRAM: {
        /* \hexagram{N} → U+4DC0 + (N-1), N ∈ [1,64] */
        g_md.ba1[g_md.ba1n] = '\0';
        int n = atoi(g_md.ba1);
        if (n >= 1 && n <= 64) emit_cp(0x4DC0 + n - 1);
        break;
    }
    case LCMD_CARD: {
        /* \card{RankSuit} → Unicode playing card
         * Rank: A 2-9 T J Q K  Suit: s h d c
         * Suits at offsets: s=0xA0 h=0xB0 d=0xC0 c=0xD0
         * Ranks: A=1 2=2..9=9 T=10 J=11 Q=13 K=14 (12=knight skipped) */
        if (g_md.ba1n >= 2) {
            char rc = g_md.ba1[0], sc = g_md.ba1[g_md.ba1n - 1];
            int suit = -1, rank = -1;
            switch (sc) {
                case 's': case 'S': suit = 0xA0; break;
                case 'h': case 'H': suit = 0xB0; break;
                case 'd': case 'D': suit = 0xC0; break;
                case 'c': case 'C': suit = 0xD0; break;
            }
            switch (rc) {
                case 'A': case 'a': rank = 1; break;
                case 'T': case 't': rank = 10; break;
                case 'J': case 'j': rank = 11; break;
                case 'Q': case 'q': rank = 13; break;
                case 'K': case 'k': rank = 14; break;
                default: if (rc >= '2' && rc <= '9') rank = rc - '0'; break;
            }
            if (suit >= 0 && rank >= 0) emit_cp(0x1F000 + suit + rank);
        }
        break;
    }
    case LCMD_ACCENT: {
        /* Look up combining character from stored command name */
        static const struct { const char *n; const char *u; } acc[] = {
            {"hat","\xCC\x82"},{"widehat","\xCC\x82"},{"check","\xCC\x8C"},
            {"tilde","\xCC\x83"},{"widetilde","\xCC\x83"},{"dot","\xCC\x87"},
            {"ddot","\xCC\x88"},{"dddot","\xE2\x83\x9B"},{"vec","\xE2\x83\x97"},
            {"bar","\xCC\x84"},{"breve","\xCC\x86"},{"acute","\xCC\x81"},
            {"grave","\xCC\x80"},{"ring","\xCC\x8A"},{NULL,NULL}
        };
        const char *mark = NULL;
        for (int i = 0; acc[i].n; i++)
            if (strcmp(acc[i].n, g_md.latex) == 0) { mark = acc[i].u; break; }
        for (int i = 0; i < g_md.ba1n; i++) {
            out_raw(&g_md.ba1[i], 1);
            if (mark) out(mark);
        }
        break;
    }
    }
    g_md.brace_cmd = LCMD_NONE;
    g_md.ba1n = g_md.ba2n = 0;
    g_md.brace_depth = g_md.brace_which = 0;
    g_md.brace_wait = false;
}

static void md_resolve_latex(void) {
    g_md.latex[g_md.latexn] = '\0';
    g_md.in_latex = false;
    if (g_md.latexn == 0) { out("\\"); return; }
    int bcmd = latex_brace_cmd(g_md.latex);
    if (bcmd != LCMD_NONE) {
        g_md.brace_cmd = bcmd;
        g_md.brace_which = 1;
        g_md.ba1n = g_md.ba2n = 0;
        g_md.brace_depth = 0;
        g_md.brace_wait = true;
        /* keep latex/latexn for abort fallback */
        return;
    }
    const char *sym = latex_lookup(g_md.latex);
    if (sym) out(sym);
    else { out("\\"); out_raw(g_md.latex, (size_t)g_md.latexn); }
    g_md.latexn = 0;
}

static void md_resolve_stars(void) {
    int s = g_md.stars; g_md.stars = 0;
    if (s >= 3) {
        if (g_md.bold && g_md.italic) {
            md_ansi("\033[22;23m"); g_md.bold = g_md.italic = false;
        } else { md_ansi("\033[1;3m"); g_md.bold = g_md.italic = true; }
        for (int i = 3; i < s; i++) out("*");
    } else if (s == 2) {
        if (g_md.bold) { md_ansi("\033[22m"); g_md.bold = false; }
        else { md_ansi("\033[1m"); g_md.bold = true; }
    } else if (s == 1) {
        if (g_md.italic) { md_ansi("\033[23m"); g_md.italic = false; }
        else { md_ansi("\033[3m"); g_md.italic = true; }
    }
}

static void md_resolve_tildes(void) {
    int t = g_md.tildes; g_md.tildes = 0;
    if (t >= 2) {
        if (g_md.strike) { md_ansi("\033[29m"); g_md.strike = false; }
        else { md_ansi("\033[9m"); g_md.strike = true; }
        for (int i = 2; i < t; i++) out("~");
    } else {
        for (int i = 0; i < t; i++) out("~");
    }
}

static void md_resolve_ticks(void) {
    int t = g_md.ticks; g_md.ticks = 0;
    if (t >= 3) {
        if (g_md.fence) { md_ansi("\033[0m"); g_md.fence = false; }
        else {
            g_md.fence = true;
            g_md.fence_lang_col = true; g_md.fence_langn = 0;
            md_ansi("\033[32m");
        }
    } else {
        if (g_md.fence) {
            for (int i = 0; i < t; i++) out("`");
        } else if (g_md.code) {
            md_ansi("\033[0m");
            if (g_md.bold) md_ansi("\033[1m");
            if (g_md.italic) md_ansi("\033[3m");
            if (g_md.header) {
                int h = g_md.hashes ? g_md.hashes : 1;
                if (h == 1) md_ansi("\033[1;35m");
                else if (h == 2) md_ansi("\033[1;36m");
                else md_ansi("\033[1;34m");
            }
            g_md.code = false;
        } else {
            md_ansi("\033[36m"); g_md.code = true;
        }
    }
}

static void md_feed(const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        char c = (char)*p;

        /* ── Brace argument collection ── */
        if (g_md.brace_cmd != LCMD_NONE) {
            if (g_md.brace_wait) {
                if (c == '{') { g_md.brace_wait = false; g_md.brace_depth = 1; continue; }
                if (isspace((unsigned char)c)) continue;
                const char *sym = latex_lookup(g_md.latex);
                if (sym) out(sym);
                else { out("\\"); out_raw(g_md.latex, (size_t)g_md.latexn); }
                g_md.brace_cmd = LCMD_NONE; g_md.latexn = 0;
            } else {
                if (c == '{') { g_md.brace_depth++; }
                else if (c == '}') {
                    g_md.brace_depth--;
                    if (g_md.brace_depth == 0) {
                        if (g_md.brace_which < latex_brace_argc(g_md.brace_cmd)) {
                            g_md.brace_which++; g_md.brace_wait = true;
                        } else { md_emit_brace_result(); }
                        continue;
                    }
                }
                if (g_md.brace_depth > 0) {
                    if (g_md.brace_which == 1 && g_md.ba1n < 510) g_md.ba1[g_md.ba1n++] = c;
                    else if (g_md.brace_which == 2 && g_md.ba2n < 510) g_md.ba2[g_md.ba2n++] = c;
                }
                continue;
            }
        }

        /* ── LaTeX command ── */
        if (g_md.in_latex) {
            if (isalpha((unsigned char)c)) {
                if (g_md.latexn < 62) g_md.latex[g_md.latexn++] = c;
                continue;
            }
            /* Special char escapes + LaTeX math delimiters */
            if (g_md.latexn == 0) {
                g_md.in_latex = false;
                switch (c) {
                    case '#': case '$': case '%': case '&': case '_':
                    case '{': case '}':
                        out_raw(&c, 1); continue;
                    /* \( \) → inline math mode (like $...$) */
                    case '(':
                        if (!g_md.math) { md_ansi("\033[33m"); g_md.math = true; }
                        continue;
                    case ')':
                        if (g_md.math) {
                            md_ansi("\033[0m");
                            if (g_md.bold) md_ansi("\033[1m");
                            if (g_md.italic) md_ansi("\033[3m");
                            g_md.math = false;
                        }
                        continue;
                    /* \[ \] → display math mode */
                    case '[':
                        if (!g_md.math) { md_ansi("\033[33m"); g_md.math = true; }
                        continue;
                    case ']':
                        if (g_md.math) {
                            md_ansi("\033[0m");
                            if (g_md.bold) md_ansi("\033[1m");
                            if (g_md.italic) md_ansi("\033[3m");
                            g_md.math = false;
                        }
                        continue;
                    case ',': out("\xe2\x80\x89"); continue; /* thin space */
                    case ';': case ':': out(" "); continue;
                    case '!': continue; /* negative thin space */
                    case ' ': out(" "); continue;
                    case '\\': out("\n"); continue; /* \\ → line break */
                    default: out("\\"); break; /* emit \ and fall through */
                }
            } else {
                md_resolve_latex();
                if (g_md.brace_cmd != LCMD_NONE) { p--; continue; }
            }
        }

        /* ── Superscript / Subscript in math mode ── */
        if (g_md.sup_sub > 0) {
            if (c == '{' && !g_md.sup_sub_brace) {
                g_md.sup_sub_brace = true; continue;
            }
            if (g_md.sup_sub_brace) {
                if (c == '}') { g_md.sup_sub = 0; g_md.sup_sub_brace = false; continue; }
                bool ok = (g_md.sup_sub == 1) ? emit_sup(c) : emit_sub(c);
                if (!ok) out_raw(&c, 1);
                continue;
            }
            /* Single char super/subscript */
            bool ok = (g_md.sup_sub == 1) ? emit_sup(c) : emit_sub(c);
            if (!ok) out_raw(&c, 1);
            g_md.sup_sub = 0;
            continue;
        }
        if (g_md.math && (c == '^' || c == '_') && !g_md.code && !g_md.fence) {
            g_md.sup_sub = (c == '^') ? 1 : 2;
            continue;
        }

        /* ── Fence language tag ── */
        if (g_md.fence_lang_col) {
            if (c == '\n') {
                g_md.fence_lang_col = false;
                g_md.fence_lang[g_md.fence_langn] = '\0';
                if (g_md.fence_langn > 0) {
                    md_ansi("\033[2;3m"); out(g_md.fence_lang); md_ansi("\033[22;23m");
                }
                out("\n"); g_md.sol = true; continue;
            }
            if (g_md.fence_langn < 30) g_md.fence_lang[g_md.fence_langn++] = c;
            continue;
        }

        /* ── Inside fence: literal except closing ``` ── */
        if (g_md.fence && c != '`' && g_md.ticks == 0) {
            out_raw(&c, 1); g_md.sol = (c == '\n'); continue;
        }

        /* ── Backtick accumulation ── */
        if (c == '`') {
            if (g_md.stars > 0 && !g_md.fence) md_resolve_stars();
            if (g_md.tildes > 0) md_resolve_tildes();
            if (g_md.hashes > 0) { for (int i = 0; i < g_md.hashes; i++) out("#"); g_md.hashes = 0; }
            g_md.ticks++; continue;
        }
        if (g_md.ticks > 0) {
            md_resolve_ticks();
            if (g_md.fence && g_md.fence_lang_col) { p--; continue; }
            if (g_md.code || g_md.fence) { out_raw(&c, 1); g_md.sol = (c == '\n'); continue; }
        }

        /* ── Inside inline code: literal ── */
        if (g_md.code) { out_raw(&c, 1); g_md.sol = (c == '\n'); continue; }

        /* ── Star accumulation ── */
        if (c == '*') {
            if (g_md.hashes > 0) { for (int i = 0; i < g_md.hashes; i++) out("#"); g_md.hashes = 0; }
            g_md.stars++; continue;
        }
        if (g_md.stars > 0) md_resolve_stars();

        /* ── Tilde accumulation → ~~strikethrough~~ ── */
        if (c == '~' && !g_md.code && !g_md.fence) {
            g_md.tildes++; continue;
        }
        if (g_md.tildes > 0) md_resolve_tildes();

        /* ── Hash at SOL ── */
        if (g_md.sol && c == '#') { g_md.hashes++; continue; }
        if (g_md.hashes > 0) {
            if (c == ' ') {
                int h = g_md.hashes; g_md.hashes = 0;
                if (h == 1) md_ansi("\033[1;35m");
                else if (h == 2) md_ansi("\033[1;36m");
                else md_ansi("\033[1;34m");
                g_md.header = true; g_md.sol = false; continue;
            }
            for (int i = 0; i < g_md.hashes; i++) out("#"); g_md.hashes = 0;
        }

        /* ── Dollar → math mode styling ── */
        if (c == '$' && !g_md.code && !g_md.fence) {
            g_md.dollars++; continue;
        }
        if (g_md.dollars > 0) {
            if (g_md.math) {
                md_ansi("\033[0m");
                if (g_md.bold) md_ansi("\033[1m");
                if (g_md.italic) md_ansi("\033[3m");
                if (g_md.header) md_ansi("\033[1;35m");
                g_md.math = false;
            } else {
                md_ansi("\033[33m");
                g_md.math = true;
            }
            g_md.dollars = 0;
        }

        /* ── Backslash → LaTeX ── */
        if (c == '\\' && !g_md.code && !g_md.fence) {
            g_md.in_latex = true; g_md.latexn = 0; continue;
        }

        /* ── Numbered list at SOL: "1. item" ── */
        if (g_md.sol && c >= '0' && c <= '9') {
            if (g_md.sol_dign < 14) g_md.sol_digits[g_md.sol_dign++] = c;
            continue;
        }
        if (g_md.sol_dign > 0) {
            if (c == '.' && !g_md.sol_dot) { g_md.sol_dot = true; continue; }
            if (c == ' ' && g_md.sol_dot) {
                g_md.sol_digits[g_md.sol_dign] = '\0';
                out("  "); out(g_md.sol_digits); out(". ");
                g_md.sol = false; g_md.sol_dign = 0; g_md.sol_dot = false;
                continue;
            }
            for (int i = 0; i < g_md.sol_dign; i++) out_raw(&g_md.sol_digits[i], 1);
            if (g_md.sol_dot) out(".");
            g_md.sol_dign = 0; g_md.sol_dot = false;
        }

        /* ── Dash at SOL → bullet (1) or horizontal rule (3+) ── */
        if (g_md.sol && c == '-') { g_md.sol_dashes++; continue; }
        if (g_md.sol_dashes > 0 && c != '-') {
            if (c == ' ' && g_md.sol_dashes == 1) {
                out("  • "); g_md.sol = false; g_md.sol_dashes = 0; continue;
            }
            if (c == '\n' && g_md.sol_dashes >= 3) {
                int w = term_width();
                md_ansi("\033[2m");
                for (int i = 0; i < w && i < 72; i++) out("─");
                md_ansi("\033[0m"); out("\n");
                g_md.sol = true; g_md.sol_dashes = 0; continue;
            }
            for (int i = 0; i < g_md.sol_dashes; i++) out("-");
            g_md.sol_dashes = 0;
        }

        /* ── > at SOL → blockquote ── */
        if (g_md.sol && c == '>') { g_md.sol_gt = true; continue; }
        if (g_md.sol_gt) {
            g_md.sol_gt = false;
            if (c == ' ') {
                md_ansi("\033[2;37m"); out("▎ "); md_ansi("\033[0m");
                if (g_md.bold) md_ansi("\033[1m");
                if (g_md.italic) md_ansi("\033[3m");
                if (g_md.math) md_ansi("\033[33m");
                g_md.sol = false; continue;
            }
            out(">");
        }

        /* ── Link rendering [text](url) ── */
        if (c == '[' && !g_md.code && !g_md.fence && g_md.link_state == 0) {
            md_ansi("\033[36;4m"); g_md.link_state = 1; continue;
        }
        if (g_md.link_state == 1 && c == ']') {
            md_ansi("\033[39;24m"); g_md.link_state = 2; continue;
        }
        if (g_md.link_state == 2) {
            if (c == '(') { md_ansi("\033[2m"); g_md.link_state = 3; continue; }
            g_md.link_state = 0;
        }
        if (g_md.link_state == 3 && c == ')') {
            md_ansi("\033[22m"); g_md.link_state = 0; continue;
        }

        /* ── Newline ── */
        if (c == '\n') {
            out("\n");
            if (g_md.header) { md_ansi("\033[0m"); g_md.header = false; }
            g_md.sol = true; continue;
        }

        /* ── Default ── */
        g_md.sol = false;
        out_raw(&c, 1);
    }
}

static void md_end_block(void) {
    if (g_md.in_latex) md_resolve_latex();
    if (g_md.brace_cmd != LCMD_NONE) g_md.brace_cmd = LCMD_NONE;
    if (g_md.stars > 0) { for (int i = 0; i < g_md.stars; i++) out("*"); g_md.stars = 0; }
    if (g_md.tildes > 0) { for (int i = 0; i < g_md.tildes; i++) out("~"); g_md.tildes = 0; }
    if (g_md.ticks > 0) { for (int i = 0; i < g_md.ticks; i++) out("`"); g_md.ticks = 0; }
    if (g_md.hashes > 0) { for (int i = 0; i < g_md.hashes; i++) out("#"); g_md.hashes = 0; }
    if (g_md.dollars > 0) g_md.dollars = 0;
    if (g_md.sol_dashes > 0) {
        for (int i = 0; i < g_md.sol_dashes; i++) out("-");
        g_md.sol_dashes = 0;
    }
    if (g_md.sol_dign > 0) {
        for (int i = 0; i < g_md.sol_dign; i++) out_raw(&g_md.sol_digits[i], 1);
        if (g_md.sol_dot) out(".");
        g_md.sol_dign = 0; g_md.sol_dot = false;
    }
    if (g_md.sol_gt) { out(">"); g_md.sol_gt = false; }
    g_md.link_state = 0;
    g_md.sup_sub = 0; g_md.sup_sub_brace = false;
    if (g_md.bold || g_md.italic || g_md.code || g_md.fence ||
        g_md.header || g_md.strike || g_md.math)
        md_ansi("\033[0m");
    g_md.bold = g_md.italic = g_md.code = g_md.fence = false;
    g_md.header = g_md.strike = g_md.math = false;
    g_md.sol = true;
}

/* ── SSE streaming state ───────────────────────────────────────────────── */

typedef struct {
    char *type;
    char *text;
    char *tool_name;
    char *tool_id;
    char *tool_input;
} block_t;

typedef struct {
    buf_t    line_buf;
    buf_t    text_buf;
    buf_t    input_buf;

    block_t  blocks[MAX_BLOCKS];
    int      block_count;
    int      cur_idx;

    char    *cur_type;
    char    *cur_tool_name;
    char    *cur_tool_id;

    char    *stop_reason;
    bool     got_error;
    char    *error_msg;

    int      input_tokens;
    int      output_tokens;
    int      cache_read_tokens;
    int      cache_create_tokens;

    long long t_start;
    long long t_first_token;
    bool      got_first;

    buf_t    raw_body;   /* capture full response for error diagnosis */
} sse_t;

static void sse_init(sse_t *s) {
    memset(s, 0, sizeof(*s));
    buf_init(&s->line_buf);
    buf_init(&s->text_buf);
    buf_init(&s->input_buf);
    buf_init(&s->raw_body);
    s->cur_idx = -1;
    s->t_start = now_ms();
}

static void sse_free(sse_t *s) {
    buf_free(&s->line_buf);
    buf_free(&s->text_buf);
    buf_free(&s->input_buf);
    buf_free(&s->raw_body);
    for (int i = 0; i < s->block_count; i++) {
        free(s->blocks[i].type); free(s->blocks[i].text);
        free(s->blocks[i].tool_name); free(s->blocks[i].tool_id);
        free(s->blocks[i].tool_input);
    }
    free(s->cur_type); free(s->cur_tool_name); free(s->cur_tool_id);
    free(s->stop_reason); free(s->error_msg);
}

static void sse_finalize_block(sse_t *s) {
    if (s->block_count >= MAX_BLOCKS) return;
    block_t *b = &s->blocks[s->block_count];
    memset(b, 0, sizeof(*b));

    if (s->cur_type && strcmp(s->cur_type, "tool_use") == 0) {
        b->type = strdup("tool_use");
        b->tool_name = s->cur_tool_name ? strdup(s->cur_tool_name) : NULL;
        b->tool_id = s->cur_tool_id ? strdup(s->cur_tool_id) : NULL;
        b->tool_input = s->input_buf.len > 0 ? strdup(s->input_buf.data) : strdup("{}");
        s->block_count++;
    } else if (s->cur_type && strcmp(s->cur_type, "text") == 0) {
        b->type = strdup("text");
        b->text = s->text_buf.len > 0 ? strdup(s->text_buf.data) : strdup("");
        s->block_count++;
    }
    /* thinking and other blocks: just drop */
    buf_reset(&s->text_buf);
    buf_reset(&s->input_buf);
}

static void sse_handle_event(sse_t *s, const char *data) {
    char *type = jstr(data, "type");
    if (!type) return;

    if (strcmp(type, "message_start") == 0) {
        char *msg = jraw(data, "message");
        if (msg) {
            char *usage = jraw(msg, "usage");
            if (usage) {
                s->input_tokens = jint(usage, "input_tokens", 0);
                s->cache_read_tokens = jint(usage, "cache_read_input_tokens", 0);
                s->cache_create_tokens = jint(usage, "cache_creation_input_tokens", 0);
                free(usage);
            }
            free(msg);
        }
    } else if (strcmp(type, "content_block_start") == 0) {
        s->cur_idx = jint(data, "index", 0);
        char *cb = jraw(data, "content_block");
        if (cb) {
            free(s->cur_type);
            s->cur_type = jstr(cb, "type");

            if (s->cur_type && strcmp(s->cur_type, "tool_use") == 0) {
                free(s->cur_tool_name); s->cur_tool_name = jstr(cb, "name");
                free(s->cur_tool_id);   s->cur_tool_id = jstr(cb, "id");
                buf_reset(&s->input_buf);
                if (s->cur_tool_name)
                    outf("\n\033[1;33m  ⚡ %s\033[0m\n", s->cur_tool_name);
            } else if (s->cur_type && strcmp(s->cur_type, "thinking") == 0) {
                buf_reset(&s->text_buf);
                out("\033[2m\033[3m");  /* dim italic for thinking */
            } else if (s->cur_type && strcmp(s->cur_type, "text") == 0) {
                buf_reset(&s->text_buf);
                md_begin_block();
            }
            free(cb);
        }
    } else if (strcmp(type, "content_block_delta") == 0) {
        char *delta = jraw(data, "delta");
        if (delta) {
            char *dt = jstr(delta, "type");
            if (dt && strcmp(dt, "text_delta") == 0) {
                char *text = jstr(delta, "text");
                if (text && text[0]) {
                    if (!s->got_first) {
                        s->t_first_token = now_ms();
                        s->got_first = true;
                    }
                    buf_cat(&s->text_buf, text);
                    /* Stream directly to terminal — instant display */
                    md_feed(text);
                    free(text);
                }
            } else if (dt && strcmp(dt, "thinking_delta") == 0) {
                char *thinking = jstr(delta, "thinking");
                if (thinking && thinking[0]) {
                    out(thinking);
                    free(thinking);
                }
            } else if (dt && strcmp(dt, "input_json_delta") == 0) {
                char *partial = jstr(delta, "partial_json");
                if (partial) {
                    buf_cat(&s->input_buf, partial);
                    free(partial);
                }
            }
            free(dt);
            free(delta);
        }
    } else if (strcmp(type, "content_block_stop") == 0) {
        if (s->cur_type && strcmp(s->cur_type, "thinking") == 0)
            out("\033[0m\n");  /* end dim italic */
        else if (s->cur_type && strcmp(s->cur_type, "text") == 0)
            md_end_block();
        sse_finalize_block(s);
    } else if (strcmp(type, "message_delta") == 0) {
        char *delta = jraw(data, "delta");
        if (delta) {
            free(s->stop_reason);
            s->stop_reason = jstr(delta, "stop_reason");
            free(delta);
        }
        char *usage = jraw(data, "usage");
        if (usage) {
            s->output_tokens = jint(usage, "output_tokens", 0);
            free(usage);
        }
    } else if (strcmp(type, "error") == 0) {
        s->got_error = true;
        char *err = jraw(data, "error");
        if (err) { s->error_msg = jstr(err, "message"); free(err); }
    }

    free(type);
}

static void sse_process_line(sse_t *s, const char *line) {
    if (strncmp(line, "data: ", 6) == 0) {
        const char *d = line + 6;
        if (strcmp(d, "[DONE]") != 0)
            sse_handle_event(s, d);
    }
}

/* Curl write callback — feed SSE lines, immediate streaming via out() */
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    sse_t *s = (sse_t *)userdata;
    if (g_interrupted) return 0;

    /* Capture raw body for error diagnosis (first 4KB) */
    if (s->raw_body.len < 4096)
        buf_append(&s->raw_body, (const char *)ptr,
                   total < 4096 - s->raw_body.len ? total : 4096 - s->raw_body.len);

    const char *p = (const char *)ptr;
    for (size_t i = 0; i < total; i++) {
        if (g_interrupted) return 0;
        if (p[i] == '\n') {
            if (s->line_buf.len > 0) {
                sse_process_line(s, s->line_buf.data);
                buf_reset(&s->line_buf);
            }
        } else if (p[i] != '\r') {
            buf_char(&s->line_buf, p[i]);
        }
    }
    return total;
}

/* ── Build content JSON from blocks ────────────────────────────────────── */

static char *blocks_to_json(block_t *blocks, int count) {
    buf_t b; buf_init(&b);
    buf_char(&b, '[');
    for (int i = 0; i < count; i++) {
        if (i > 0) buf_char(&b, ',');
        if (blocks[i].type && strcmp(blocks[i].type, "text") == 0) {
            buf_cat(&b, "{\"type\":\"text\",\"text\":");
            buf_json_str(&b, blocks[i].text ? blocks[i].text : "");
            buf_char(&b, '}');
        } else if (blocks[i].type && strcmp(blocks[i].type, "tool_use") == 0) {
            buf_cat(&b, "{\"type\":\"tool_use\",\"id\":");
            buf_json_str(&b, blocks[i].tool_id ? blocks[i].tool_id : "");
            buf_cat(&b, ",\"name\":");
            buf_json_str(&b, blocks[i].tool_name ? blocks[i].tool_name : "");
            buf_cat(&b, ",\"input\":");
            buf_cat(&b, blocks[i].tool_input ? blocks[i].tool_input : "{}");
            buf_char(&b, '}');
        }
    }
    buf_char(&b, ']');
    return b.data;
}

/* ── API call ──────────────────────────────────────────────────────────── */

static const char *g_model = DEFAULT_MODEL;
static const char *g_system = "You are a helpful coding assistant with access to shell, read_file, and write_file tools. Use them proactively. Be concise and direct.";

/* Session tracking */
static int g_total_in = 0, g_total_out = 0;
static int g_total_cache_read = 0, g_total_cache_write = 0;
static double g_total_cost = 0.0;
static int g_turn_count = 0;

/* Extended thinking */
static bool g_thinking = false;
static int g_thinking_budget = 10000;
static int g_max_tokens = 16384;

static bool api_stream(const char *api_key, sse_t *state) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    char *messages = conv_build_messages();

    buf_t req; buf_init(&req);
    buf_cat(&req, "{\"model\":");
    buf_json_str(&req, g_model);
    int effective_max = g_max_tokens;
    if (g_thinking && g_thinking_budget + 16384 > effective_max)
        effective_max = g_thinking_budget + 16384;
    buf_printf(&req, ",\"max_tokens\":%d", effective_max);
    buf_cat(&req, ",\"stream\":true");
    /* System prompt with cache_control for prompt caching */
    buf_cat(&req, ",\"system\":[{\"type\":\"text\",\"text\":");
    buf_json_str(&req, g_system);
    buf_cat(&req, ",\"cache_control\":{\"type\":\"ephemeral\"}}]");
    buf_cat(&req, ",\"messages\":");
    buf_cat(&req, messages);
    buf_cat(&req, ",\"tools\":");
    buf_cat(&req, TOOLS_JSON);
    if (g_thinking)
        buf_printf(&req, ",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":%d}",
                   g_thinking_budget);
    buf_char(&req, '}');
    free(messages);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    bool oauth_auth = dsc_uses_claude_code_auth(api_key);
    char auth[512];
    if (oauth_auth)
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    else
        snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    char ver[128]; snprintf(ver, sizeof(ver), "anthropic-version: %s", API_VERSION);
    hdrs = curl_slist_append(hdrs, ver);
    char beta[256];
    if (oauth_auth)
        snprintf(beta, sizeof(beta), "anthropic-beta: %s,%s",
                 CLAUDE_CODE_OAUTH_BETA, ANTHROPIC_BETAS);
    else
        snprintf(beta, sizeof(beta), "anthropic-beta: %s", ANTHROPIC_BETAS);
    hdrs = curl_slist_append(hdrs, beta);
    hdrs = curl_slist_append(hdrs, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    /* HTTP/2 for better streaming multiplexing */
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    /* TCP keepalive to prevent idle drops */
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
    /* Disable Nagle for lower latency on small SSE chunks */
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    /* Disable output buffering on the curl side */
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    buf_free(&req);

    if (res != CURLE_OK && !g_interrupted) {
        outf("\n\033[31merror: %s\033[0m\n", curl_easy_strerror(res));
        return false;
    }

    /* Non-200 responses are JSON errors, not SSE — parse them */
    if (http_code != 200 && !g_interrupted) {
        state->got_error = true;
        /* Try to extract error message from JSON body */
        char *err_obj = jraw(state->raw_body.data, "error");
        if (err_obj) {
            char *msg = jstr(err_obj, "message");
            char *etype = jstr(err_obj, "type");
            free(state->error_msg);
            if (msg && etype) {
                char combined[2048];
                snprintf(combined, sizeof(combined), "HTTP %ld — %s: %s", http_code, etype, msg);
                state->error_msg = strdup(combined);
            } else if (msg) {
                state->error_msg = msg; msg = NULL;
            } else {
                char fallback[256];
                snprintf(fallback, sizeof(fallback), "HTTP %ld (no message)", http_code);
                state->error_msg = strdup(fallback);
            }
            free(msg); free(etype); free(err_obj);
        } else {
            char fallback[4096];
            snprintf(fallback, sizeof(fallback), "HTTP %ld: %.3900s", http_code,
                     state->raw_body.len > 0 ? state->raw_body.data : "(empty body)");
            state->error_msg = strdup(fallback);
        }
        return false;
    }

    return true;
}

/* ── Agentic loop ──────────────────────────────────────────────────────── */

/* Forward-declared; body lives below next to main() so it can see g_model
 * and the full exec-handoff helper set. */
__attribute__((noreturn))
static void dsc_exec_dsco_new(int argc, char **argv, const char *reason);

/* Detect credit/billing error messages across providers — kept in sync
 * with provider_msg_is_credit_too_low() in src/provider.c so both the
 * standalone dsc and the full dsco build share the same classification. */
static bool dsc_err_is_credit(const char *msg) {
    if (!msg || !*msg) return false;
    static const char *needles[] = {
        "credit balance is too low",
        "credit balance too low",
        "insufficient_quota",
        "insufficient funds",
        "insufficient credit",
        "billing_hard_limit_reached",
        "exceeded your current quota",
        "payment required",
        "quota_exceeded",
        "HTTP 402",
        NULL,
    };
    for (int i = 0; needles[i]; i++) {
        const char *n = needles[i];
        size_t nlen = strlen(n);
        for (const char *p = msg; *p; p++) {
            if (strncasecmp(p, n, nlen) == 0) return true;
        }
    }
    return false;
}

/* Context for main so agent_turn can trigger a handoff on credit error. */
static int    g_main_argc = 0;
static char **g_main_argv = NULL;

static void agent_turn(const char *api_key) {
    for (int iter = 0; iter < 25; iter++) {
        g_interrupted = 0;

        sse_t state;
        int retries = 0;
    retry_api:
        sse_init(&state);

        bool ok = api_stream(api_key, &state);

        if (state.got_error) {
            /* Auto-retry on rate limit (429) */
            if (state.error_msg && strstr(state.error_msg, "HTTP 429") && retries < 3) {
                int wait = (retries + 1) * 5;
                outf("\033[33m  rate limited — retrying in %ds\033[0m\n", wait);
                sse_free(&state);
                sleep((unsigned)wait);
                retries++;
                goto retry_api;
            }
            /* Auto-retry on overloaded (529) */
            if (state.error_msg && strstr(state.error_msg, "HTTP 529") && retries < 3) {
                int wait = (retries + 1) * 10;
                outf("\033[33m  overloaded — retrying in %ds\033[0m\n", wait);
                sse_free(&state);
                sleep((unsigned)wait);
                retries++;
                goto retry_api;
            }
            /* Credit / billing exhaustion — if another provider is
             * configured, hand off to dsco-new automatically so the user
             * isn't dead in the water. Otherwise print an actionable hint. */
            if (dsc_err_is_credit(state.error_msg)) {
                const char *has_other =
                    getenv("OPENROUTER_API_KEY") ?: getenv("XAI_API_KEY") ?:
                    getenv("GROK_API_KEY")       ?: getenv("OPENAI_API_KEY") ?:
                    getenv("GOOGLE_API_KEY")     ?: getenv("GEMINI_API_KEY");
                outf("\n\033[31manthropic billing error:\033[0m %s\n",
                     state.error_msg ? state.error_msg : "credit balance too low");
                if (has_other && g_main_argv) {
                    sse_free(&state);
                    dsc_exec_dsco_new(g_main_argc, g_main_argv,
                                      "anthropic credit exhausted");
                    /* noreturn */
                }
                outf("  \033[2mhint: set OPENROUTER_API_KEY (or XAI_API_KEY, OPENAI_API_KEY,\n"
                     "    GOOGLE_API_KEY, GEMINI_API_KEY) and re-run; dsc will hand off\n"
                     "    to ./dsco-new which can route through any provider.\033[0m\n"
                     "  \033[2malternatively: `./dsco-new -m x-ai/grok-4.20-beta`\033[0m\n");
                sse_free(&state);
                return;
            }
            outf("\n\033[31mAPI error: %s\033[0m\n",
                 state.error_msg ? state.error_msg : "unknown");
            sse_free(&state);
            return;
        }

        if (!ok) { sse_free(&state); return; }

        out("\n");

        /* Cost estimation */
        double cost = (double)state.input_tokens * 3.0 / 1000000.0
                    + (double)state.output_tokens * 15.0 / 1000000.0
                    + (double)state.cache_read_tokens * 0.30 / 1000000.0
                    + (double)state.cache_create_tokens * 3.75 / 1000000.0;
        g_total_in += state.input_tokens;
        g_total_out += state.output_tokens;
        g_total_cache_read += state.cache_read_tokens;
        g_total_cache_write += state.cache_create_tokens;
        g_total_cost += cost;
        g_turn_count++;

        /* Stats line */
        long long elapsed = now_ms() - state.t_start;
        long long ttft = state.got_first ? state.t_first_token - state.t_start : 0;
        int tps = elapsed > 0 ? (int)((long long)state.output_tokens * 1000 / elapsed) : 0;
        outf("\033[2m  [in:%d out:%d", state.input_tokens, state.output_tokens);
        if (state.cache_read_tokens > 0)
            outf(" cache:%d", state.cache_read_tokens);
        outf(" | ttft:%lldms %dtok/s %lldms | $%.4f]\033[0m\n", ttft, tps, elapsed, cost);

        /* Add to conversation */
        char *cj = blocks_to_json(state.blocks, state.block_count);
        conv_add_assistant(cj);
        free(cj);

        /* Check for tool use */
        bool has_tools = false;
        for (int i = 0; i < state.block_count; i++)
            if (state.blocks[i].type && strcmp(state.blocks[i].type, "tool_use") == 0)
                has_tools = true;

        if (!has_tools || g_interrupted) { sse_free(&state); return; }

        /* Execute tools */
        for (int i = 0; i < state.block_count; i++) {
            if (!state.blocks[i].type || strcmp(state.blocks[i].type, "tool_use") != 0)
                continue;
            char *result = execute_tool(state.blocks[i].tool_name, state.blocks[i].tool_input);
            outf("\033[2m  → %zu bytes\033[0m\n", result ? strlen(result) : 0);
            conv_add_tool_result(state.blocks[i].tool_id, result);
            free(result);
        }

        sse_free(&state);
    }
    out("\033[33m[max iterations]\033[0m\n");
}

/* ── Multi-provider handoff ────────────────────────────────────────────── */

/* dsc is an Anthropic-only single-file binary. When the user asks for a
 * non-Anthropic model (e.g. grok-4.20, gpt-5.4, x-ai/grok-4.20-beta) or when
 * Anthropic credit runs out, re-exec into ./dsco-new (the multi-provider
 * build) so the full routing table, OpenAI-compat streaming, and fallback
 * chain take over. Falls back to a clear hint if dsco-new isn't present. */

static bool dsc_model_is_anthropic(const char *m) {
    if (!m || !*m) return true;
    /* Any org/model.id slash means it's OpenRouter-routed; not native. */
    if (strchr(m, '/')) return false;
    if (strstr(m, "claude") || strstr(m, "opus") ||
        strstr(m, "sonnet") || strstr(m, "haiku"))
        return true;
    return false;
}

static char *dsc_sibling_path(const char *argv0, const char *name,
                              char *out, size_t out_len) {
    if (!out || out_len == 0) return NULL;
    out[0] = '\0';
    if (!argv0 || !*argv0) {
        snprintf(out, out_len, "./%s", name);
        return out;
    }
    const char *slash = strrchr(argv0, '/');
    if (!slash) {
        snprintf(out, out_len, "./%s", name);
        return out;
    }
    size_t dlen = (size_t)(slash - argv0);
    if (dlen + strlen(name) + 2 >= out_len) return NULL;
    memcpy(out, argv0, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, strlen(name) + 1);
    return out;
}

__attribute__((noreturn))
static void dsc_exec_dsco_new(int argc, char **argv, const char *reason) {
    char path[1024];
    char *p = dsc_sibling_path(argv[0], "dsco-new", path, sizeof(path));
    if (!p || access(p, X_OK) != 0) {
        /* Try relative ./dsco-new */
        p = (char *)"./dsco-new";
        if (access(p, X_OK) != 0) p = (char *)"dsco-new";
    }
    if (reason && *reason) {
        fprintf(stderr, "\033[33mdsc: %s — handing off to %s\033[0m\n", reason, p);
    }
    /* Build new argv: replace argv[0], keep the rest untouched so --model / -m
     * and any user flags get passed through verbatim. */
    char **new_argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!new_argv) {
        fprintf(stderr, "dsc: out of memory during handoff\n");
        _exit(1);
    }
    new_argv[0] = p;
    for (int i = 1; i < argc; i++) new_argv[i] = argv[i];
    new_argv[argc] = NULL;
    execvp(p, new_argv);
    /* execvp returned — it failed */
    fprintf(stderr,
            "\033[31mdsc: handoff to %s failed: %s\033[0m\n"
            "  \033[2mhint: run `make dsco-new` or use `./dsco-new -m %s` directly.\033[0m\n",
            p, strerror(errno), g_model ? g_model : "MODEL");
    free(new_argv);
    _exit(1);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    g_main_argc = argc;
    g_main_argv = argv;
    /* First pass: parse --model / -m so we can short-circuit into dsco-new
     * for non-Anthropic models before touching the Anthropic OAuth flow. */
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--model") == 0 || strcmp(argv[i], "-m") == 0) && i + 1 < argc) {
            g_model = argv[i + 1];
            if (!dsc_model_is_anthropic(g_model)) {
                dsc_exec_dsco_new(argc, argv, "non-Anthropic model");
            }
            break;
        }
    }

    const char *api_key = dsc_resolve_claude_code_oauth_token();
    if (!api_key || !*api_key) api_key = getenv("ANTHROPIC_API_KEY");
    if (!api_key || !*api_key) {
        /* No Anthropic auth available — if another provider key is present,
         * hand off automatically. */
        if (getenv("OPENROUTER_API_KEY") || getenv("OPENAI_API_KEY") ||
            getenv("XAI_API_KEY") || getenv("GROK_API_KEY") ||
            getenv("GOOGLE_API_KEY") || getenv("GEMINI_API_KEY")) {
            dsc_exec_dsco_new(argc, argv, "no Anthropic credential");
        }
        fprintf(stderr, "dsc: sign in with Claude Code or set ANTHROPIC_API_KEY\n"
                        "  \033[2mhint: or set OPENROUTER_API_KEY / XAI_API_KEY / OPENAI_API_KEY and dsc will hand off to dsco-new.\033[0m\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--model") == 0 || strcmp(argv[i], "-m") == 0) && i + 1 < argc)
            g_model = argv[++i];
        else if (strcmp(argv[i], "--system") == 0 && i + 1 < argc)
            g_system = argv[++i];
        else if (strcmp(argv[i], "--thinking") == 0) {
            g_thinking = true;
            if (i + 1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9') {
                g_thinking_budget = atoi(argv[++i]);
                if (g_thinking_budget <= 0) g_thinking_budget = 10000;
            }
        }
        else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc) {
            g_max_tokens = atoi(argv[++i]);
            if (g_max_tokens <= 0) g_max_tokens = 16384;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "dsc — streaming Claude agent with markdown rendering\n"
                "usage: dsc [-m|--model MODEL] [--system PROMPT] [--thinking [BUDGET]]\n"
                "           [--max-tokens N]\n"
                "note:  non-Anthropic models (e.g. grok-4.20, gpt-5.4, x-ai/…) and\n"
                "       credit-exhausted sessions hand off to ./dsco-new automatically\n\n"
                "commands:\n"
                "  /model NAME     set model        /system PROMPT  set system prompt\n"
                "  /thinking [N]   toggle thinking   /clear          clear conversation\n"
                "  /compact        drop old context  /cost           token usage & cost\n"
                "  /help           show commands     quit            exit\n\n"
                "auth:\n"
                "  prefers Claude Code OAuth from macOS Keychain or ~/.claude/.credentials.json\n"
                "  auto-refreshes stored OAuth tokens when possible\n"
                "  falls back to ANTHROPIC_API_KEY when OAuth is unavailable\n");
            return 0;
        }
    }

    /* Set up output fd and disable all buffering */
    g_out_fd = STDOUT_FILENO;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    detect_term();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    struct sigaction sa = { .sa_handler = sigint_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    /* Banner */
    int w = term_width();
    out("\n");
    if (g_color) {
        out("\033[1;36m");
        for (int i = 0; i < w && i < 72; i++) out("━");
        out("\033[0m\n");
        outf("  \033[1mdsc\033[0m \033[2mv%s\033[0m · \033[36m%s\033[0m\n", DSC_VERSION, g_model);
        out("  tools: shell, read_file, write_file");
        if (g_thinking) outf(" · \033[33mthinking:%d\033[0m", g_thinking_budget);
        out("\n");
        out("  \033[2m/help for commands");
        if (g_iterm2) out(" · iTerm2 streaming");
        out(" · prompt caching on\033[0m\n");
        out("\033[1;36m");
        for (int i = 0; i < w && i < 72; i++) out("━");
        out("\033[0m\n\n");
    } else {
        outf("dsc v%s · %s · tools: shell, read_file, write_file\n\n", DSC_VERSION, g_model);
    }

    /* Readline setup (history only — input uses composer box) */
    rl_bind_key('\t', rl_insert);  /* literal tab, no completion */
    using_history();

    while (!g_interrupted) {
        iterm_mark_prompt_start();
        char *line = dsc_composer_read();
        iterm_mark_prompt_end();
        if (!line) break;  /* ctrl-d / ctrl-c on empty */

        /* Trim whitespace */
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        char *end = s + strlen(s);
        while (end > s && isspace((unsigned char)end[-1])) end--;
        *end = '\0';

        if (!*s) { free(line); continue; }

        add_history(s);

        if (strcmp(s, "quit") == 0 || strcmp(s, "exit") == 0) { free(line); break; }

        if (strncmp(s, "/model ", 7) == 0) {
            g_model = strdup(s + 7);
            outf("  model → \033[36m%s\033[0m\n", g_model);
            free(line); continue;
        }
        if (strcmp(s, "/clear") == 0) {
            for (int i = 0; i < g_msg_count; i++) {
                free(g_msgs[i].role); free(g_msgs[i].content);
            }
            g_msg_count = 0;
            out("  \033[2mconversation cleared\033[0m\n");
            free(line); continue;
        }
        if (strncmp(s, "/system ", 8) == 0) {
            g_system = strdup(s + 8);
            out("  \033[2msystem prompt updated\033[0m\n");
            free(line); continue;
        }
        if (strcmp(s, "/cost") == 0 || strcmp(s, "/tokens") == 0) {
            outf("  \033[2m─── session ───\033[0m\n");
            outf("  turns:       %d\n", g_turn_count);
            outf("  input:       %d tokens\n", g_total_in);
            outf("  output:      %d tokens\n", g_total_out);
            if (g_total_cache_read > 0)
                outf("  cache read:  %d tokens\n", g_total_cache_read);
            if (g_total_cache_write > 0)
                outf("  cache write: %d tokens\n", g_total_cache_write);
            outf("  total cost:  \033[1m$%.4f\033[0m\n", g_total_cost);
            free(line); continue;
        }
        if (strcmp(s, "/thinking") == 0) {
            g_thinking = !g_thinking;
            outf("  thinking: \033[36m%s\033[0m (budget: %d)\n",
                 g_thinking ? "on" : "off", g_thinking_budget);
            free(line); continue;
        }
        if (strncmp(s, "/thinking ", 10) == 0) {
            int budget = atoi(s + 10);
            if (budget > 0) { g_thinking_budget = budget; g_thinking = true; }
            else g_thinking = false;
            outf("  thinking: \033[36m%s\033[0m (budget: %d)\n",
                 g_thinking ? "on" : "off", g_thinking_budget);
            free(line); continue;
        }
        if (strcmp(s, "/compact") == 0) {
            int keep = g_msg_count / 2;
            if (keep < 4) keep = 4;
            if (keep > g_msg_count) keep = g_msg_count;
            int drop = g_msg_count - keep;
            drop = (drop / 2) * 2; /* drop pairs to keep alternation */
            if (drop > 0) {
                for (int i = 0; i < drop; i++) {
                    free(g_msgs[i].role); free(g_msgs[i].content);
                }
                memmove(g_msgs, &g_msgs[drop], sizeof(msg_t) * (size_t)(g_msg_count - drop));
                g_msg_count -= drop;
            }
            outf("  \033[2mcompacted: %d messages (dropped %d)\033[0m\n", g_msg_count, drop);
            free(line); continue;
        }
        if (strcmp(s, "/help") == 0) {
            out("  \033[1mcommands:\033[0m\n");
            out("  /model NAME     set model\n");
            out("  /system PROMPT  set system prompt\n");
            out("  /thinking [N]   toggle extended thinking (optional budget)\n");
            out("  /clear          clear conversation\n");
            out("  /compact        drop oldest context\n");
            out("  /cost           show token usage & cost\n");
            out("  /help           this help\n");
            out("  quit            exit\n");
            free(line); continue;
        }

        iterm_mark_command_start();

        conv_add_user(s);
        free(line);

        agent_turn(api_key);

        iterm_mark_command_end(0);
    }

    out("\n");
    curl_global_cleanup();
    return 0;
}
