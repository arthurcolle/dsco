/* realtime.c — OpenAI Realtime API (GA) speech-to-speech sessions with
 * realtime tool calling, bridged into the DSCO tool registry.
 *
 * Transport: hand-rolled RFC 6455 WebSocket client over mbedTLS (the same
 * stack net_server.c uses), because the system libcurl on macOS ships
 * without ws/wss protocol support. Only the main session thread touches the
 * socket; tool executions run on detached worker threads and hand their
 * function_call_output events back through a mutex-guarded outbox.
 *
 * Audio: AudioToolbox AudioQueues (Darwin), PCM16 mono @ 24kHz both ways.
 * Mic buffers land in a ring the session loop drains into
 * input_audio_buffer.append events; response.output_audio.delta payloads are
 * base64-decoded into a playback ring the output queue drains. Server-side
 * VAD (semantic_vad) owns turn taking; on input_audio_buffer.speech_started
 * we drop any queued playback so the user can barge in.
 */
#include "realtime.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config.h"
#include "crypto.h"
#include "json_util.h"
#include "provider.h"
#include "setup.h"
#include "tools.h"

#define RT_DEFAULT_MODEL "gpt-realtime-2"
#define RT_DEFAULT_VOICE "marin"
#define RT_DEFAULT_VAD   "semantic_vad"
#define RT_DEFAULT_REASONING_EFFORT "low"
#define RT_HOST          "api.openai.com"
#define RT_PORT          "443"
#define RT_PATH_FMT      "/v1/realtime?model=%s"
#define RT_SAMPLE_RATE   24000
#define RT_CHUNK_BYTES   4800 /* 100ms of PCM16 mono @ 24kHz */
#define RT_MAX_TOOLS     TOOL_REGISTER_CAP
#define RT_MAX_TOOL_OUT  12000 /* chars of tool output surfaced to the model */
#define RT_MAX_WS_MSG    (32u * 1024 * 1024)
#define RT_TOOL_CONTEXT_MAX 8192

#define RT_DEFAULT_ECHO_GUARD_MS 2000

static const char RT_DEFAULT_INSTRUCTIONS[] =
    "You are DSCO's voice interface, a fast assistant living in a terminal. "
    "Be natural and brief — one or two spoken sentences unless the user asks "
    "for depth. You have function tools from the DSCO tool registry; call "
    "them whenever they would make the answer faster or more accurate, then "
    "summarize the result in plain speech. Prefer specific DSCO tools over "
    "generic web fetches: for weather, use `weather` first when the user gives "
    "a place name, and use `nws` for US latitude/longitude, station, or state "
    "lookups once those values are known. Do not use WebFetch, WebSearch, or "
    "parallel_search for weather unless the specific weather tools fail. For "
    "local filesystem questions, use cwd, list_directory, file_tree, file_info, "
    "read_file, grep_files, or find_files; use file_tree/file_info when sizes "
    "matter. When the user explicitly asks for shell, Python, build, test, or "
    "systems-agent work and those tools are available, use bash, python, "
    "run_command, make_build, or test_run instead of claiming you cannot run "
    "them. Never invent placeholder API keys or place fake credentials in "
    "URLs/tool inputs, and do not ask the user for permission to call read-only "
    "tools. Never read raw JSON, IDs, or base64 aloud.";

static volatile sig_atomic_t g_rt_stop = 0;

static void rt_on_sigint(int sig) {
    (void)sig;
    g_rt_stop = 1;
}

static bool rt_env_truthy(const char *name);

const char *realtime_default_reasoning_effort(void) {
    return RT_DEFAULT_REASONING_EFFORT;
}

int realtime_voice_default_max_tools(void) {
    return RT_MAX_TOOLS;
}

static const char *RT_PREFERRED_TOOLS[] = {
    "calc",
    "eval",
    "date",
    "weather",
    "nws",
    "synoptic",
    "WebSearch",
    "parallel_search",
    "WebFetch",
    "cwd",
    "list_directory",
    "file_tree",
    "file_info",
    "find_files",
    "grep_files",
    "read_file",
    "Read",
    "Grep",
    "Glob",
    "TaskList",
};

static const char *RT_CONTEXTUAL_OPERATOR_TOOLS[] = {
    "bash",
    "python",
    "run_command",
    "make_build",
    "test_run",
    "compile",
    "sandbox_run",
    "git",
    "apply_patch",
    "write_file",
    "edit_file",
    "append_file",
    "Bash",
    "Write",
    "Edit",
    "MultiEdit",
    "format_code",
    "lint",
};

static int rt_preferred_rank(const char *name) {
    if (!name)
        return -1;
    for (size_t i = 0; i < sizeof(RT_PREFERRED_TOOLS) / sizeof(RT_PREFERRED_TOOLS[0]); i++)
        if (strcmp(name, RT_PREFERRED_TOOLS[i]) == 0)
            return (int)i;
    return -1;
}

static bool rt_context_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0])
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++)
        if (strncasecmp(p, needle, nlen) == 0)
            return true;
    return false;
}

static bool rt_context_wants_operator_tools(const char *context) {
    if (rt_env_truthy("DSCO_SYSTEMS_AGENT") || rt_env_truthy("DSCO_REALTIME_SYSTEM_TOOLS") ||
        rt_env_truthy("DSCO_REALTIME_OPERATOR_TOOLS"))
        return true;

    static const char *terms[] = {
        "systems-agent",
        "system agent",
        "bash",
        "shell",
        "terminal",
        "run command",
        "run a command",
        "run shell",
        "run bash",
        "run python",
        "execute",
        "exec",
        "python",
        "script",
        "compile",
        "build",
        "make ",
        "test ",
        "pytest",
        "apply patch",
        "patch file",
        "edit file",
        "write file",
    };
    for (size_t i = 0; i < sizeof(terms) / sizeof(terms[0]); i++)
        if (rt_context_contains_ci(context, terms[i]))
            return true;
    return false;
}

static bool rt_operator_ranked(const char *name) {
    if (!name)
        return false;
    for (size_t i = 0;
         i < sizeof(RT_CONTEXTUAL_OPERATOR_TOOLS) / sizeof(RT_CONTEXTUAL_OPERATOR_TOOLS[0]);
         i++)
        if (strcmp(name, RT_CONTEXTUAL_OPERATOR_TOOLS[i]) == 0)
            return true;
    return false;
}

static bool rt_tool_voice_safe_for_context(const tool_def_t *t, const char *context) {
    if (!t || !t->name || strlen(t->name) > 64 || t->is_interactive)
        return false;
    if (t->is_read_only || rt_preferred_rank(t->name) >= 0)
        return true;
    return rt_context_wants_operator_tools(context) && rt_operator_ranked(t->name);
}

static bool rt_selected_has(const tool_def_t **out, int count, const char *name) {
    if (!name)
        return false;
    for (int i = 0; i < count; i++)
        if (out[i] && out[i]->name && strcmp(out[i]->name, name) == 0)
            return true;
    return false;
}

static bool rt_add_tool_ptr(const tool_def_t **out, int *count, int max_tools,
                            const tool_def_t *tool, const char *context) {
    if (!out || !count || *count >= max_tools || !rt_tool_voice_safe_for_context(tool, context) ||
        rt_selected_has(out, *count, tool->name))
        return false;
    out[(*count)++] = tool;
    return true;
}

static void rt_add_named_tool(const tool_def_t **out, int *count, int max_tools,
                              const char *name, const char *context) {
    int total = 0;
    const tool_def_t *all = tools_get_all(&total);
    for (int i = 0; i < total && *count < max_tools; i++) {
        if (all[i].name && strcmp(all[i].name, name) == 0) {
            rt_add_tool_ptr(out, count, max_tools, &all[i], context);
            return;
        }
    }
}

static int rt_pack_tool_page(const tool_page_result_t *paged, const tool_def_t **out,
                             int count, int max_tools, const char *context) {
    if (!paged || !out || max_tools <= 0)
        return count;

    const tool_def_t **tiers[] = {paged->pinned, paged->working, paged->discovery};
    int tier_counts[] = {paged->pinned_count, paged->working_count, paged->discovery_count};
    for (int t = 0; t < 3 && count < max_tools; t++) {
        for (int i = 0; i < tier_counts[t] && count < max_tools; i++)
            rt_add_tool_ptr(out, &count, max_tools, tiers[t][i], context);
    }
    return count;
}

static int rt_select_paged_tools(const char *context, const tool_def_t **out, int max_tools) {
    if (!out || max_tools <= 0)
        return 0;

    int count = 0;
    for (size_t i = 0;
         i < sizeof(RT_PREFERRED_TOOLS) / sizeof(RT_PREFERRED_TOOLS[0]) && count < max_tools;
         i++)
        rt_add_named_tool(out, &count, max_tools, RT_PREFERRED_TOOLS[i], context);

    if (rt_context_wants_operator_tools(context)) {
        for (size_t i = 0;
             i < sizeof(RT_CONTEXTUAL_OPERATOR_TOOLS) / sizeof(RT_CONTEXTUAL_OPERATOR_TOOLS[0]) &&
             count < max_tools;
             i++)
            rt_add_named_tool(out, &count, max_tools, RT_CONTEXTUAL_OPERATOR_TOOLS[i], context);
    }

    tool_page_result_t paged = tools_get_paged(context, max_tools, 1.0f);
    count = rt_pack_tool_page(&paged, out, count, max_tools, context);
    tool_page_result_free(&paged);
    return count;
}

int realtime_voice_select_tool_names_for_context(
    const char *context,
    char names[][DSCO_REALTIME_TOOL_NAME_MAX],
    int max_tools) {
    if (!names || max_tools <= 0)
        return 0;

    const tool_def_t **selected = calloc((size_t)max_tools, sizeof(*selected));
    if (!selected)
        return 0;

    int count = rt_select_paged_tools(context, selected, max_tools);
    for (int i = 0; i < count; i++)
        snprintf(names[i], DSCO_REALTIME_TOOL_NAME_MAX, "%s",
                 selected[i] && selected[i]->name ? selected[i]->name : "");
    free(selected);
    return count;
}

/* ════════════════════════════════════════════════════════════════════════
 * CLI entry — shared by every build; the engine below is HAVE_MBEDTLS-gated.
 * ════════════════════════════════════════════════════════════════════════ */

static void rt_usage(FILE *out) {
    fprintf(out,
            "usage: dsco voice [options]\n"
            "  Speech-to-speech session against the OpenAI Realtime API with live\n"
            "  tool calling into the DSCO tool registry.\n\n"
            "options:\n"
            "  --model <id>          realtime model (default %s, env DSCO_REALTIME_MODEL)\n"
            "  --voice <name>        output voice (default %s)\n"
            "  --vad <mode>          semantic | server (default semantic)\n"
            "  --instructions <str>  override the built-in voice system prompt\n"
            "  --text                text-only session (type turns; no mic/speaker)\n"
            "  --half-duplex         mute mic while DSCO is speaking (default speaker mode)\n"
            "  --full-duplex         keep mic live while DSCO speaks (headphones/barge-in)\n"
            "  --no-tools            do not expose DSCO tools to the session\n"
            "  --systems-agent       enable systems-agent env posture and operator tools\n"
            "  -h, --help            show this help\n\n"
            "environment:\n"
            "  OPENAI_API_KEY            API key (also resolved via provider config)\n"
            "  DSCO_REALTIME_MODEL       default model override\n"
            "  DSCO_REALTIME_REASONING_EFFORT  Realtime 2 effort (default low; none to omit)\n"
            "  DSCO_REALTIME_TRANSCRIBE  input transcription model (default whisper-1)\n"
            "  DSCO_REALTIME_MAX_TOOLS   tool count cap in session.update (default %d)\n"
            "  DSCO_REALTIME_SYSTEM_TOOLS  1 to always expose operator tools\n"
            "  DSCO_REALTIME_ECHO_GUARD_MS  mic mute duration in half-duplex mode (default %d)\n"
            "  DSCO_REALTIME_HALF_DUPLEX    1 to force mic mute while DSCO speaks\n"
            "  DSCO_CA_BUNDLE            PEM bundle for TLS verification\n",
            RT_DEFAULT_MODEL, RT_DEFAULT_VOICE, RT_MAX_TOOLS, RT_DEFAULT_ECHO_GUARD_MS);
}

static void rt_enable_systems_agent_mode(void) {
    bool already = rt_env_truthy("DSCO_SYSTEMS_AGENT") || rt_env_truthy("DSCO_GOV_BYPASS");
    setenv("DSCO_GOV_MODEL", "none", 1);
    setenv("DSCO_GOV_BYPASS", "1", 1);
    setenv("DSCO_TRUST_TIER", "trusted", 1);
    setenv("DSCO_APPROVAL_MODE", "never", 1);
    setenv("DSCO_APPROVAL_NEVER", "1", 1);
    setenv("DSCO_NO_APPROVAL_PROMPTS", "1", 1);
    setenv("DSCO_SYSTEMS_AGENT", "1", 1);
    if (!getenv("DSCO_DHT_SWARM"))
        setenv("DSCO_DHT_SWARM", "dsco-systems", 1);
    setenv("DSCO_NET_FORCE", "1", 1);
    if (!getenv("DSCO_FLEET_CONCURRENCY"))
        setenv("DSCO_FLEET_CONCURRENCY", "64", 1);
    if (!already) {
        fprintf(stderr,
                "\x1b[1;31m[systems-agent] GOVERNANCE DISABLED - unbounded "
                "permissions; ungoverned control arm.\x1b[0m\n");
        fprintf(stderr,
                "\x1b[36m[systems-agent] native networking hooks active: "
                "mesh P2P + DHT overlay + TLS server + fleet fanout.\x1b[0m\n");
    }
}

int realtime_voice_cli(int argc, char **argv) {
    realtime_opts_t o = {.half_duplex = true}; /* speaker-safe unless --full-duplex is requested */
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            rt_usage(stdout);
            return 0;
        } else if (strcmp(a, "--text") == 0) {
            o.text_only = true;
        } else if (strcmp(a, "--half-duplex") == 0) {
            o.half_duplex = true;
        } else if (strcmp(a, "--full-duplex") == 0) {
            o.half_duplex = false;
        } else if (strcmp(a, "--no-tools") == 0) {
            o.no_tools = true;
        } else if (strcmp(a, "--systems-agent") == 0) {
            rt_enable_systems_agent_mode();
        } else if (strcmp(a, "--model") == 0 && i + 1 < argc) {
            o.model = argv[++i];
        } else if (strcmp(a, "--voice") == 0 && i + 1 < argc) {
            o.voice = argv[++i];
        } else if (strcmp(a, "--instructions") == 0 && i + 1 < argc) {
            o.instructions = argv[++i];
        } else if (strcmp(a, "--vad") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            o.vad = (strncmp(v, "server", 6) == 0) ? "server_vad" : "semantic_vad";
        } else {
            fprintf(stderr, "dsco voice: unknown option '%s'\n\n", a);
            rt_usage(stderr);
            return 2;
        }
    }
    return realtime_voice_run(&o);
}

#ifndef HAVE_MBEDTLS

int realtime_voice_run(const realtime_opts_t *opts) {
    (void)opts;
    fprintf(stderr,
            "dsco voice: this build lacks mbedTLS (HAVE_MBEDTLS), which the "
            "realtime WebSocket transport requires.\n"
            "Rebuild with mbedTLS available (brew install mbedtls@3).\n");
    return 1;
}

#else /* HAVE_MBEDTLS — the actual engine */

#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#define RT_HAVE_AUDIO 1
#endif

/* ── byte ring (mic → uplink, downlink → speaker) ─────────────────────── */

typedef struct {
    pthread_mutex_t mu;
    uint8_t        *data;
    size_t          len, cap;
} rt_ring_t;

static void rt_ring_init(rt_ring_t *r) {
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->mu, NULL);
}

static void rt_ring_free(rt_ring_t *r) {
    pthread_mutex_destroy(&r->mu);
    free(r->data);
    r->data = NULL;
    r->len = r->cap = 0;
}

static void rt_ring_push(rt_ring_t *r, const void *src, size_t n) {
    if (n == 0)
        return;
    pthread_mutex_lock(&r->mu);
    /* Backstop: never hold more than ~16MB (~6min of PCM16@24k) */
    if (r->len + n <= 16u * 1024 * 1024) {
        if (r->len + n > r->cap) {
            size_t cap = r->cap ? r->cap : 65536;
            while (cap < r->len + n)
                cap *= 2;
            uint8_t *nd = realloc(r->data, cap);
            if (nd) {
                r->data = nd;
                r->cap = cap;
            }
        }
        if (r->len + n <= r->cap) {
            memcpy(r->data + r->len, src, n);
            r->len += n;
        }
    }
    pthread_mutex_unlock(&r->mu);
}

static size_t rt_ring_pop(rt_ring_t *r, void *dst, size_t max) {
    pthread_mutex_lock(&r->mu);
    size_t n = r->len < max ? r->len : max;
    if (n) {
        memcpy(dst, r->data, n);
        memmove(r->data, r->data + n, r->len - n);
        r->len -= n;
    }
    pthread_mutex_unlock(&r->mu);
    return n;
}

static size_t rt_ring_size(rt_ring_t *r) {
    pthread_mutex_lock(&r->mu);
    size_t n = r->len;
    pthread_mutex_unlock(&r->mu);
    return n;
}

static void rt_ring_clear(rt_ring_t *r) {
    pthread_mutex_lock(&r->mu);
    r->len = 0;
    pthread_mutex_unlock(&r->mu);
}

/* ── WebSocket client (RFC 6455) over mbedTLS ─────────────────────────── */

typedef struct {
    mbedtls_net_context     net;
    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt        cacert;
    bool                    connected;
    bool                    closed;
    uint8_t                *rbuf; /* raw frame accumulation */
    size_t                  rlen, rcap;
    jbuf_t                  frag; /* fragmented-message assembly */
    bool                    frag_active;
} rt_ws_t;

static void rt_ws_init(rt_ws_t *ws) {
    memset(ws, 0, sizeof(*ws));
    mbedtls_net_init(&ws->net);
    mbedtls_ssl_init(&ws->ssl);
    mbedtls_ssl_config_init(&ws->conf);
    mbedtls_entropy_init(&ws->entropy);
    mbedtls_ctr_drbg_init(&ws->drbg);
    mbedtls_x509_crt_init(&ws->cacert);
    jbuf_init(&ws->frag, 4096);
}

static void rt_ws_free(rt_ws_t *ws) {
    if (ws->connected)
        mbedtls_ssl_close_notify(&ws->ssl);
    mbedtls_ssl_free(&ws->ssl);
    mbedtls_ssl_config_free(&ws->conf);
    mbedtls_net_free(&ws->net);
    mbedtls_x509_crt_free(&ws->cacert);
    mbedtls_ctr_drbg_free(&ws->drbg);
    mbedtls_entropy_free(&ws->entropy);
    jbuf_free(&ws->frag);
    free(ws->rbuf);
    ws->rbuf = NULL;
}

static bool rt_load_ca(rt_ws_t *ws) {
    const char *cands[] = {getenv("DSCO_CA_BUNDLE"), "/etc/ssl/cert.pem",
                           "/private/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt",
                           "/opt/homebrew/etc/ca-certificates/cert.pem",
                           "/usr/local/etc/ca-certificates/cert.pem"};
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        if (cands[i] && mbedtls_x509_crt_parse_file(&ws->cacert, cands[i]) >= 0)
            return true;
    }
    return false;
}

/* Case-insensitive lookup of an HTTP response header value. */
static bool rt_http_header(const char *hdrs, const char *name, char *out, size_t outlen) {
    size_t nlen = strlen(name);
    for (const char *p = hdrs; p && *p; p = strstr(p, "\r\n"), p = p ? p + 2 : NULL) {
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t i = 0;
            while (p[i] && p[i] != '\r' && i + 1 < outlen) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            return true;
        }
    }
    return false;
}

/* Write the full buffer, waiting out WANT_READ/WANT_WRITE on the socket. */
static bool rt_ws_write_all(rt_ws_t *ws, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        int ret = mbedtls_ssl_write(&ws->ssl, buf + off, n - off);
        if (ret > 0) {
            off += (size_t)ret;
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            fd_set rfds, wfds;
            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            FD_SET(ws->net.fd, &rfds);
            FD_SET(ws->net.fd, &wfds);
            struct timeval tv = {5, 0};
            if (select(ws->net.fd + 1, &rfds, &wfds, NULL, &tv) < 0 && errno != EINTR)
                break;
            if (g_rt_stop)
                break;
            continue;
        }
        break;
    }
    if (off < n)
        ws->closed = true;
    return off == n;
}

static bool rt_ws_send(rt_ws_t *ws, uint8_t opcode, const void *payload, size_t n) {
    if (ws->closed)
        return false;
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x80u | (opcode & 0x0f);
    if (n < 126) {
        hdr[h++] = 0x80u | (uint8_t)n;
    } else if (n <= 0xffff) {
        hdr[h++] = 0x80u | 126;
        hdr[h++] = (uint8_t)(n >> 8);
        hdr[h++] = (uint8_t)n;
    } else {
        hdr[h++] = 0x80u | 127;
        for (int i = 7; i >= 0; i--)
            hdr[h++] = (uint8_t)((uint64_t)n >> (8 * i));
    }
    uint8_t mask[4];
    if (mbedtls_ctr_drbg_random(&ws->drbg, mask, 4) != 0) {
        mask[0] = 0x5d;
        mask[1] = 0xc0;
        mask[2] = 0x11;
        mask[3] = 0xe5;
    }
    memcpy(hdr + h, mask, 4);
    h += 4;

    uint8_t *frame = malloc(h + n);
    if (!frame)
        return false;
    memcpy(frame, hdr, h);
    const uint8_t *src = payload;
    for (size_t i = 0; i < n; i++)
        frame[h + i] = src[i] ^ mask[i & 3];
    bool ok = rt_ws_write_all(ws, frame, h + n);
    free(frame);
    return ok;
}

static bool rt_ws_send_text(rt_ws_t *ws, const char *json) {
    return rt_ws_send(ws, 0x1, json, strlen(json));
}

static void rt_rbuf_append(rt_ws_t *ws, const uint8_t *src, size_t n) {
    if (ws->rlen + n > ws->rcap) {
        size_t cap = ws->rcap ? ws->rcap : 65536;
        while (cap < ws->rlen + n)
            cap *= 2;
        uint8_t *nd = realloc(ws->rbuf, cap);
        if (!nd)
            return;
        ws->rbuf = nd;
        ws->rcap = cap;
    }
    memcpy(ws->rbuf + ws->rlen, src, n);
    ws->rlen += n;
}

/* Read whatever the socket has (waiting ≤ timeout_ms for the first byte).
 * Returns bytes read, 0 on timeout, -1 once the peer is gone. */
static int rt_ws_fill(rt_ws_t *ws, int timeout_ms) {
    if (ws->closed)
        return -1;
    if (!mbedtls_ssl_check_pending(&ws->ssl)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ws->net.fd, &rfds);
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int sr = select(ws->net.fd + 1, &rfds, NULL, NULL, &tv);
        if (sr <= 0)
            return 0; /* timeout or EINTR — caller re-checks g_rt_stop */
    }
    int total = 0;
    uint8_t tmp[16384];
    for (;;) {
        int ret = mbedtls_ssl_read(&ws->ssl, tmp, sizeof(tmp));
        if (ret > 0) {
            rt_rbuf_append(ws, tmp, (size_t)ret);
            total += ret;
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            break;
        ws->closed = true; /* 0 = EOF, PEER_CLOSE_NOTIFY, or hard error */
        return total > 0 ? total : -1;
    }
    return total;
}

/* Pop one complete data message from the accumulation buffer, handling
 * control frames inline. NULL when more bytes are needed. Caller frees. */
static char *rt_ws_next_msg(rt_ws_t *ws) {
    for (;;) {
        if (ws->rlen < 2)
            return NULL;
        const uint8_t *b = ws->rbuf;
        bool fin = (b[0] & 0x80) != 0;
        uint8_t op = b[0] & 0x0f;
        bool masked = (b[1] & 0x80) != 0;
        uint64_t plen = b[1] & 0x7f;
        size_t h = 2;
        if (plen == 126) {
            if (ws->rlen < 4)
                return NULL;
            plen = ((uint64_t)b[2] << 8) | b[3];
            h = 4;
        } else if (plen == 127) {
            if (ws->rlen < 10)
                return NULL;
            plen = 0;
            for (int i = 0; i < 8; i++)
                plen = (plen << 8) | b[2 + i];
            h = 10;
        }
        if (plen > RT_MAX_WS_MSG) {
            ws->closed = true;
            return NULL;
        }
        const uint8_t *maskkey = NULL;
        if (masked) {
            if (ws->rlen < h + 4)
                return NULL;
            maskkey = b + h;
            h += 4;
        }
        if (ws->rlen < h + plen)
            return NULL;

        uint8_t *payload = ws->rbuf + h;
        if (maskkey)
            for (uint64_t i = 0; i < plen; i++)
                payload[i] ^= maskkey[i & 3];

        char *out = NULL;
        if (op == 0x9) { /* ping → pong (payload ≤ 125 by spec) */
            rt_ws_send(ws, 0xA, payload, (size_t)plen);
        } else if (op == 0xA) {
            /* pong — ignore */
        } else if (op == 0x8) { /* close */
            rt_ws_send(ws, 0x8, payload, plen >= 2 ? 2 : 0);
            ws->closed = true;
        } else { /* text/binary/continuation */
            if (op != 0x0) {
                jbuf_reset(&ws->frag);
                ws->frag_active = true;
            }
            if (ws->frag_active && ws->frag.len + plen <= RT_MAX_WS_MSG)
                jbuf_append_len(&ws->frag, (const char *)payload, (size_t)plen);
            if (fin && ws->frag_active) {
                out = malloc(ws->frag.len + 1);
                if (out) {
                    memcpy(out, ws->frag.data, ws->frag.len);
                    out[ws->frag.len] = '\0';
                }
                jbuf_reset(&ws->frag);
                ws->frag_active = false;
            }
        }

        size_t consumed = h + (size_t)plen;
        memmove(ws->rbuf, ws->rbuf + consumed, ws->rlen - consumed);
        ws->rlen -= consumed;

        if (out)
            return out;
        if (ws->closed)
            return NULL;
    }
}

static char *rt_ws_poll(rt_ws_t *ws, int timeout_ms) {
    char *m = rt_ws_next_msg(ws);
    if (m)
        return m;
    if (ws->closed)
        return NULL;
    if (rt_ws_fill(ws, timeout_ms) <= 0)
        return NULL;
    return rt_ws_next_msg(ws);
}

static bool rt_ws_connect(rt_ws_t *ws, const char *host, const char *port, const char *path,
                          const char *bearer, char *err, size_t errlen) {
    snprintf(err, errlen, "connection failed");
    if (mbedtls_ctr_drbg_seed(&ws->drbg, mbedtls_entropy_func, &ws->entropy,
                              (const unsigned char *)"dsco-realtime", 13) != 0) {
        snprintf(err, errlen, "rng seed failed");
        return false;
    }
    if (mbedtls_net_connect(&ws->net, host, port, MBEDTLS_NET_PROTO_TCP) != 0) {
        snprintf(err, errlen, "TCP connect to %s:%s failed", host, port);
        return false;
    }
    if (mbedtls_ssl_config_defaults(&ws->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        snprintf(err, errlen, "TLS config failed");
        return false;
    }
    mbedtls_ssl_conf_rng(&ws->conf, mbedtls_ctr_drbg_random, &ws->drbg);
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    mbedtls_ssl_conf_min_tls_version(&ws->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#else
    mbedtls_ssl_conf_min_version(&ws->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3); /* TLS 1.2 */
#endif
    if (getenv("DSCO_TLS_INSECURE")) {
        mbedtls_ssl_conf_authmode(&ws->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else if (rt_load_ca(ws)) {
        mbedtls_ssl_conf_ca_chain(&ws->conf, &ws->cacert, NULL);
        mbedtls_ssl_conf_authmode(&ws->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        fprintf(stderr,
                "dsco voice: warning: no CA bundle found (set DSCO_CA_BUNDLE); "
                "TLS certificate NOT verified\n");
        mbedtls_ssl_conf_authmode(&ws->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_read_timeout(&ws->conf, 15000); /* handshake+upgrade phase */
    if (mbedtls_ssl_setup(&ws->ssl, &ws->conf) != 0 ||
        mbedtls_ssl_set_hostname(&ws->ssl, host) != 0) {
        snprintf(err, errlen, "TLS setup failed");
        return false;
    }
    mbedtls_ssl_set_bio(&ws->ssl, &ws->net, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    int ret;
    while ((ret = mbedtls_ssl_handshake(&ws->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            snprintf(err, errlen, "TLS handshake failed (-0x%04x)", (unsigned)-ret);
            return false;
        }
    }
    ws->connected = true;

    /* HTTP/1.1 Upgrade */
    uint8_t nonce[16];
    mbedtls_ctr_drbg_random(&ws->drbg, nonce, sizeof(nonce));
    char key[32];
    base64_encode(nonce, sizeof(nonce), key, sizeof(key));

    char req[2048];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: dsco-cli\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: %s\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "Authorization: Bearer %s\r\n"
                      "\r\n",
                      path, host, key, bearer);
    if (rl <= 0 || rl >= (int)sizeof(req) || !rt_ws_write_all(ws, (uint8_t *)req, (size_t)rl)) {
        snprintf(err, errlen, "upgrade request send failed");
        return false;
    }

    /* Read response headers; keep any trailing bytes (first frames). */
    char hdrs[16384];
    size_t hl = 0;
    char *body = NULL;
    while (hl + 1 < sizeof(hdrs)) {
        int n = mbedtls_ssl_read(&ws->ssl, (unsigned char *)hdrs + hl, sizeof(hdrs) - 1 - hl);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (n <= 0) {
            snprintf(err, errlen, "upgrade response read failed (-0x%04x)", (unsigned)-n);
            return false;
        }
        hl += (size_t)n;
        hdrs[hl] = '\0';
        if ((body = strstr(hdrs, "\r\n\r\n")) != NULL)
            break;
    }
    if (!body) {
        snprintf(err, errlen, "no HTTP response terminator from server");
        return false;
    }
    body += 4;

    if (strncmp(hdrs, "HTTP/1.1 101", 12) != 0) {
        /* Surface the status line and any JSON error body the API returned. */
        char line[256];
        size_t i = 0;
        while (hdrs[i] && hdrs[i] != '\r' && i < sizeof(line) - 1) {
            line[i] = hdrs[i];
            i++;
        }
        line[i] = '\0';
        snprintf(err, errlen, "upgrade refused: %s%s%.512s", line, *body ? " — " : "", body);
        return false;
    }

    char accept_hdr[64];
    if (rt_http_header(hdrs, "Sec-WebSocket-Accept", accept_hdr, sizeof(accept_hdr))) {
        char joined[80];
        snprintf(joined, sizeof(joined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        unsigned char sha[20];
        char expect[32] = {0};
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
        if (mbedtls_sha1((const unsigned char *)joined, strlen(joined), sha) == 0)
#else
        if (mbedtls_sha1_ret((const unsigned char *)joined, strlen(joined), sha) == 0)
#endif
            base64_encode(sha, sizeof(sha), expect, sizeof(expect));
        if (strcmp(accept_hdr, expect) != 0) {
            snprintf(err, errlen, "Sec-WebSocket-Accept mismatch");
            return false;
        }
    }

    /* Bytes past the headers are already WebSocket frames. */
    size_t extra = hl - (size_t)(body - hdrs);
    if (extra)
        rt_rbuf_append(ws, (const uint8_t *)body, extra);

    mbedtls_ssl_conf_read_timeout(&ws->conf, 0);
    mbedtls_ssl_set_bio(&ws->ssl, &ws->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    mbedtls_net_set_nonblock(&ws->net);
    return true;
}

/* ── session state ────────────────────────────────────────────────────── */

typedef struct rt_session {
    rt_ws_t   ws;
    rt_ring_t mic, spk;
    bool      text_only;
    bool      stdin_text;
    bool      stdin_closed;
    bool      no_tools;
    bool      audio_started;
    bool      ready;          /* session.updated seen */
    bool      response_active;
    bool      pending_audio_response;
    bool      assistant_audio_active;
    bool      full_duplex;
    bool      assistant_open; /* streaming transcript line in progress */
    bool      color;
    bool      tool_context_dirty;
    int       echo_guard_ms;
    long long echo_guard_until_ms;
    const realtime_opts_t *opts;
    const char            *model;
    char      tool_context[RT_TOOL_CONTEXT_MAX];

    /* outbox: tool worker threads → session thread (sole socket writer) */
    pthread_mutex_t outbox_mu;
    char          **outbox;
    size_t          outbox_n, outbox_cap;
    int             tools_inflight;

    /* call_id → tool name (from response.output_item.added) */
    struct {
        char call_id[80];
        char name[128];
    } calls[16];
    int calls_next;

#ifdef RT_HAVE_AUDIO
    AudioQueueRef in_q, out_q;
#endif
} rt_session_t;

static long long rt_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

static bool rt_env_truthy(const char *name) {
    const char *v = getenv(name);
    return v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
                 strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0);
}

static int rt_env_int_clamped(const char *name, int def, int min, int max) {
    const char *v = getenv(name);
    if (!v || !v[0])
        return def;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v)
        return def;
    if (n < min)
        return min;
    if (n > max)
        return max;
    return (int)n;
}

static bool rt_model_supports_reasoning(const char *model) {
    return model && (strcmp(model, "gpt-realtime-2") == 0 ||
                     strncmp(model, "gpt-realtime-2-", 15) == 0);
}

static void rt_status(rt_session_t *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s->assistant_open) {
        fputc('\n', stdout);
        fflush(stdout);
        s->assistant_open = false;
    }
    fprintf(stderr, "%s", s->color ? "\x1b[2m" : "");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "%s\n", s->color ? "\x1b[0m" : "");
    va_end(ap);
}

/* ── outbox ───────────────────────────────────────────────────────────── */

static void rt_outbox_push_locked(rt_session_t *s, char *msg) {
    if (s->outbox_n == s->outbox_cap) {
        size_t cap = s->outbox_cap ? s->outbox_cap * 2 : 8;
        char **nb = realloc(s->outbox, cap * sizeof(char *));
        if (!nb) {
            free(msg);
            return;
        }
        s->outbox = nb;
        s->outbox_cap = cap;
    }
    s->outbox[s->outbox_n++] = msg;
}

/* Queue a function_call_output; once the last in-flight tool reports back,
 * chase it with response.create so the model resumes with all results. */
static void rt_outbox_push_tool_result(rt_session_t *s, char *item_json) {
    pthread_mutex_lock(&s->outbox_mu);
    rt_outbox_push_locked(s, item_json);
    if (s->tools_inflight > 0)
        s->tools_inflight--;
    if (s->tools_inflight == 0)
        rt_outbox_push_locked(s, safe_strdup("{\"type\":\"response.create\"}"));
    pthread_mutex_unlock(&s->outbox_mu);
}

static void rt_outbox_flush(rt_session_t *s) {
    for (;;) {
        pthread_mutex_lock(&s->outbox_mu);
        char *msg = NULL;
        if (s->outbox_n) {
            msg = s->outbox[0];
            memmove(s->outbox, s->outbox + 1, --s->outbox_n * sizeof(char *));
        }
        pthread_mutex_unlock(&s->outbox_mu);
        if (!msg)
            return;
        if (strstr(msg, "\"type\":\"response.create\"") != NULL)
            s->response_active = true;
        rt_ws_send_text(&s->ws, msg);
        free(msg);
    }
}

/* ── tool bridge ──────────────────────────────────────────────────────── */

typedef struct {
    rt_session_t *s;
    char          name[128];
    char          call_id[80];
    char         *args; /* owned */
} rt_toolcall_t;

static void *rt_tool_thread(void *arg) {
    rt_toolcall_t *tc = arg;
    rt_session_t *s = tc->s;

    char *result = malloc(MAX_TOOL_RESULT);
    bool ok = false;
    if (result) {
        result[0] = '\0';
        ok = tools_execute(tc->name, tc->args && tc->args[0] ? tc->args : "{}", result,
                           MAX_TOOL_RESULT);
    }
    const char *body = result ? result : "tool execution failed: out of memory";
    char trunc_note[64] = "";
    size_t blen = strlen(body);
    char *clipped = NULL;
    if (blen > RT_MAX_TOOL_OUT) {
        clipped = malloc(RT_MAX_TOOL_OUT + 1);
        if (clipped) {
            memcpy(clipped, body, RT_MAX_TOOL_OUT);
            clipped[RT_MAX_TOOL_OUT] = '\0';
            body = clipped;
            snprintf(trunc_note, sizeof(trunc_note), "\n…[truncated %zu bytes]",
                     blen - (size_t)RT_MAX_TOOL_OUT);
        }
    }

    jbuf_t b;
    jbuf_init(&b, 2048);
    jbuf_append(&b, "{\"type\":\"conversation.item.create\",\"item\":"
                    "{\"type\":\"function_call_output\",\"call_id\":");
    jbuf_append_json_str(&b, tc->call_id);
    jbuf_append(&b, ",\"output\":");
    if (ok && trunc_note[0] == '\0') {
        jbuf_append_json_str(&b, body);
    } else {
        jbuf_t ob;
        jbuf_init(&ob, blen + 128);
        if (!ok)
            jbuf_append(&ob, "ERROR: ");
        jbuf_append(&ob, body);
        jbuf_append(&ob, trunc_note);
        jbuf_append_json_str(&b, ob.data);
        jbuf_free(&ob);
    }
    jbuf_append(&b, "}}");

    fprintf(stderr, "\n%s⚙ %s → %s (%zu bytes)%s\n", s->color ? "\x1b[2m" : "", tc->name,
            ok ? "ok" : "error", strlen(body), s->color ? "\x1b[0m" : "");
    rt_outbox_push_tool_result(s, b.data); /* jbuf data ownership moves to outbox */

    free(clipped);
    free(result);
    free(tc->args);
    free(tc);
    return NULL;
}

static void rt_call_map_put(rt_session_t *s, const char *call_id, const char *name) {
    int i = s->calls_next++ % 16;
    snprintf(s->calls[i].call_id, sizeof(s->calls[i].call_id), "%s", call_id ? call_id : "");
    snprintf(s->calls[i].name, sizeof(s->calls[i].name), "%s", name ? name : "");
}

static const char *rt_call_map_get(rt_session_t *s, const char *call_id) {
    if (!call_id)
        return NULL;
    for (int i = 0; i < 16; i++)
        if (strcmp(s->calls[i].call_id, call_id) == 0 && s->calls[i].name[0])
            return s->calls[i].name;
    return NULL;
}

static void rt_dispatch_tool_call(rt_session_t *s, const char *evt) {
    char *call_id = json_get_str(evt, "call_id");
    char *name = json_get_str(evt, "name");
    char *args = json_get_str(evt, "arguments"); /* JSON object as a string */
    const char *resolved = name && name[0] ? name : rt_call_map_get(s, call_id);

    if (!call_id || !resolved) {
        rt_status(s, "⚙ dropped tool call (missing %s)", call_id ? "name" : "call_id");
        free(call_id);
        free(name);
        free(args);
        return;
    }
    rt_status(s, "⚙ %s(%.100s%s)", resolved, args ? args : "{}",
              args && strlen(args) > 100 ? "…" : "");

    rt_toolcall_t *tc = calloc(1, sizeof(*tc));
    if (!tc) {
        free(call_id);
        free(name);
        free(args);
        return;
    }
    tc->s = s;
    snprintf(tc->name, sizeof(tc->name), "%s", resolved);
    snprintf(tc->call_id, sizeof(tc->call_id), "%s", call_id);
    tc->args = args; /* ownership moves */

    pthread_mutex_lock(&s->outbox_mu);
    s->tools_inflight++;
    pthread_mutex_unlock(&s->outbox_mu);

    pthread_t tid;
    if (pthread_create(&tid, NULL, rt_tool_thread, tc) == 0) {
        pthread_detach(tid);
    } else {
        pthread_mutex_lock(&s->outbox_mu);
        s->tools_inflight--;
        pthread_mutex_unlock(&s->outbox_mu);
        free(tc->args);
        free(tc);
    }
    free(call_id);
    free(name);
}

/* ── audio (Darwin AudioQueue) ────────────────────────────────────────── */

#ifdef RT_HAVE_AUDIO

static void rt_audio_in_cb(void *ud, AudioQueueRef q, AudioQueueBufferRef buf,
                           const AudioTimeStamp *ts, UInt32 npkt,
                           const AudioStreamPacketDescription *pd) {
    (void)ts;
    (void)npkt;
    (void)pd;
    rt_session_t *s = ud;
    if (buf->mAudioDataByteSize)
        rt_ring_push(&s->mic, buf->mAudioData, buf->mAudioDataByteSize);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static void rt_audio_out_cb(void *ud, AudioQueueRef q, AudioQueueBufferRef buf) {
    rt_session_t *s = ud;
    size_t want = buf->mAudioDataBytesCapacity;
    size_t got = rt_ring_pop(&s->spk, buf->mAudioData, want);
    if (got < want)
        memset((uint8_t *)buf->mAudioData + got, 0, want - got); /* pad with silence */
    buf->mAudioDataByteSize = (UInt32)want;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static bool rt_audio_start(rt_session_t *s, char *err, size_t errlen) {
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate = RT_SAMPLE_RATE;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = 2;
    fmt.mBytesPerPacket = 2;

    OSStatus st = noErr;
    if (!s->stdin_text) {
        st = AudioQueueNewInput(&fmt, rt_audio_in_cb, s, NULL, NULL, 0, &s->in_q);
        if (st != noErr) {
            snprintf(err, errlen, "AudioQueueNewInput failed (%d) — mic permission?", (int)st);
            return false;
        }
        for (int i = 0; i < 3; i++) {
            AudioQueueBufferRef b;
            if (AudioQueueAllocateBuffer(s->in_q, RT_CHUNK_BYTES, &b) == noErr)
                AudioQueueEnqueueBuffer(s->in_q, b, 0, NULL);
        }
        st = AudioQueueStart(s->in_q, NULL);
        if (st != noErr) {
            snprintf(err, errlen, "AudioQueueStart(input) failed (%d) — mic permission?", (int)st);
            return false;
        }
    }

    st = AudioQueueNewOutput(&fmt, rt_audio_out_cb, s, NULL, NULL, 0, &s->out_q);
    if (st != noErr) {
        snprintf(err, errlen, "AudioQueueNewOutput failed (%d)", (int)st);
        return false;
    }
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(s->out_q, RT_CHUNK_BYTES, &b) == noErr) {
            memset(b->mAudioData, 0, RT_CHUNK_BYTES);
            b->mAudioDataByteSize = RT_CHUNK_BYTES;
            AudioQueueEnqueueBuffer(s->out_q, b, 0, NULL);
        }
    }
    st = AudioQueueStart(s->out_q, NULL);
    if (st != noErr) {
        snprintf(err, errlen, "AudioQueueStart(output) failed (%d)", (int)st);
        return false;
    }
    return true;
}

static void rt_audio_stop(rt_session_t *s) {
    if (s->in_q) {
        AudioQueueStop(s->in_q, true);
        AudioQueueDispose(s->in_q, true);
        s->in_q = NULL;
    }
    if (s->out_q) {
        AudioQueueStop(s->out_q, true);
        AudioQueueDispose(s->out_q, true);
        s->out_q = NULL;
    }
}

#endif /* RT_HAVE_AUDIO */

/* ── session.update / event handling ──────────────────────────────────── */

static int rt_select_tools(rt_session_t *s, const tool_def_t **out, int max_tools) {
    return rt_select_paged_tools(s ? s->tool_context : NULL, out, max_tools);
}

static void rt_tool_context_append(rt_session_t *s, const char *text) {
    if (!s || s->no_tools || !text || !text[0])
        return;

    char entry[1024];
    snprintf(entry, sizeof(entry), "user: %.1000s\n", text);
    size_t cur = strlen(s->tool_context);
    size_t add = strlen(entry);
    if (add >= sizeof(s->tool_context)) {
        const char *tail = entry + add - sizeof(s->tool_context) + 1;
        snprintf(s->tool_context, sizeof(s->tool_context), "%s", tail);
    } else {
        if (cur + add + 1 > sizeof(s->tool_context)) {
            size_t drop = cur + add + 1 - sizeof(s->tool_context);
            if (drop < cur) {
                char *nl = memchr(s->tool_context + drop, '\n', cur - drop);
                if (nl)
                    drop = (size_t)(nl + 1 - s->tool_context);
            }
            if (drop > cur)
                drop = cur;
            memmove(s->tool_context, s->tool_context + drop, cur - drop + 1);
            cur -= drop;
        }
        memcpy(s->tool_context + cur, entry, add + 1);
    }
    s->tool_context_dirty = true;
}

static bool rt_append_one_tool(jbuf_t *b, const tool_def_t *t, int *count) {
    if (!b || !t || !t->name || !count)
        return false;
    if (*count)
        jbuf_append_char(b, ',');
    jbuf_append(b, "{\"type\":\"function\",\"name\":");
    jbuf_append_json_str(b, t->name);
    jbuf_append(b, ",\"description\":");
    char desc[2048];
    snprintf(desc, sizeof(desc), "%s", t->description ? t->description : "");
    jbuf_append_json_str(b, desc);
    jbuf_append(b, ",\"parameters\":");
    jbuf_append(b, t->input_schema_json && t->input_schema_json[0]
                       ? t->input_schema_json
                       : "{\"type\":\"object\",\"properties\":{}}");
    jbuf_append_char(b, '}');
    (*count)++;
    return true;
}

static bool rt_append_one_external_tool(jbuf_t *b, const external_tool_t *t, int *count) {
    if (!b || !t || !t->name[0] || !count)
        return false;
    if (*count)
        jbuf_append_char(b, ',');
    jbuf_append(b, "{\"type\":\"function\",\"name\":");
    jbuf_append_json_str(b, t->name);
    jbuf_append(b, ",\"description\":");
    jbuf_append_json_str(b, t->description);
    jbuf_append(b, ",\"parameters\":");
    jbuf_append(b, t->input_schema_json && t->input_schema_json[0]
                       ? t->input_schema_json
                       : "{\"type\":\"object\",\"properties\":{}}");
    jbuf_append_char(b, '}');
    (*count)++;
    return true;
}

static void rt_append_external_tools(jbuf_t *b, int *count, int max_tools) {
    if (!b || !count || max_tools <= 0)
        return;

    external_tool_snapshot_t ext = tools_external_snapshot();
    int *ext_order = ext.count > 0 ? safe_malloc((size_t)ext.count * sizeof(*ext_order)) : NULL;
    int ext_order_count = ext_order ? tools_rank_external_snapshot(&ext, NULL, ext_order, ext.count) : 0;
    int ext_budget = max_tools - *count;
    int loaded_ext_count = 0;
    for (int i = 0; i < ext.count; i++)
        if (ext.items[i].loaded)
            loaded_ext_count++;

    if (loaded_ext_count > ext_budget)
        ext_budget = loaded_ext_count;
    if (ext_budget > 24)
        ext_budget = 24;
    if (ext_budget < 0)
        ext_budget = 0;

    int ext_written = 0;
    for (int pass = 0; pass < 2 && ext_written < ext_budget; pass++) {
        bool want_loaded = (pass == 0);
        for (int oi = 0; oi < ext_order_count && ext_written < ext_budget; oi++) {
            int i = ext_order[oi];
            if (i < 0 || i >= ext.count)
                continue;
            if ((bool)ext.items[i].loaded != want_loaded)
                continue;
            if (rt_append_one_external_tool(b, &ext.items[i], count))
                ext_written++;
        }
    }
    free(ext_order);
    tools_external_snapshot_free(&ext);
}

static int rt_append_tools(rt_session_t *s, jbuf_t *b) {
    int max_tools = RT_MAX_TOOLS;
    const char *cap_env = getenv("DSCO_REALTIME_MAX_TOOLS");
    if (cap_env && atoi(cap_env) > 0)
        max_tools = atoi(cap_env);
    if (max_tools > 64)
        max_tools = 64;

    const tool_def_t **selected = calloc((size_t)max_tools, sizeof(*selected));
    if (!selected)
        return 0;

    int count = 0;
    int selected_count = rt_select_tools(s, selected, max_tools);
    for (int i = 0; i < selected_count && count < max_tools; i++)
        rt_append_one_tool(b, selected[i], &count);
    rt_append_external_tools(b, &count, max_tools);
    free(selected);
    return count;
}

static bool rt_echo_guard_active(rt_session_t *s, long long now_ms) {
    if (s->text_only || s->stdin_text || s->full_duplex || s->echo_guard_ms <= 0)
        return false;
    if (s->assistant_audio_active)
        return true;
    if (s->echo_guard_until_ms > now_ms)
        return true;
    return rt_ring_size(&s->spk) > 0;
}

static bool rt_suppress_audio_input(rt_session_t *s, long long now_ms) {
    if (s->text_only || s->stdin_text || s->full_duplex)
        return false;
    return s->response_active || rt_echo_guard_active(s, now_ms);
}

static void rt_echo_guard_output_started(rt_session_t *s) {
    if (s->text_only || s->stdin_text || s->full_duplex || s->echo_guard_ms <= 0)
        return;
    bool first_audio = !s->assistant_audio_active;
    s->assistant_audio_active = true;
    s->echo_guard_until_ms = rt_now_ms() + s->echo_guard_ms;
    if (first_audio) {
        rt_ring_clear(&s->mic);
        rt_ws_send_text(&s->ws, "{\"type\":\"input_audio_buffer.clear\"}");
    }
}

static void rt_echo_guard_output_done(rt_session_t *s) {
    if (s->text_only || s->stdin_text || s->full_duplex || s->echo_guard_ms <= 0)
        return;
    s->assistant_audio_active = false;
    s->echo_guard_until_ms = rt_now_ms() + s->echo_guard_ms;
}

static int rt_send_session_update(rt_session_t *s) {
    const realtime_opts_t *o = s->opts;
    const char *transcribe = getenv("DSCO_REALTIME_TRANSCRIBE");
    if (!transcribe || !transcribe[0])
        transcribe = "whisper-1";
    const char *reasoning_effort = getenv("DSCO_REALTIME_REASONING_EFFORT");
    if (!reasoning_effort || !reasoning_effort[0])
        reasoning_effort = rt_model_supports_reasoning(s->model)
                               ? realtime_default_reasoning_effort()
                               : NULL;

    jbuf_t b;
    jbuf_init(&b, 8192);
    jbuf_append(&b, "{\"type\":\"session.update\",\"session\":{\"type\":\"realtime\","
                    "\"output_modalities\":[");
    jbuf_append(&b, s->text_only ? "\"text\"" : "\"audio\"");
    jbuf_append(&b, "],\"instructions\":");
    jbuf_append_json_str(&b, o->instructions ? o->instructions : RT_DEFAULT_INSTRUCTIONS);

    if (reasoning_effort && reasoning_effort[0] && strcasecmp(reasoning_effort, "none") != 0 &&
        strcasecmp(reasoning_effort, "off") != 0 && strcmp(reasoning_effort, "0") != 0) {
        jbuf_append(&b, ",\"reasoning\":{\"effort\":");
        jbuf_append_json_str(&b, reasoning_effort);
        jbuf_append(&b, "}");
    }

    if (!s->text_only) {
        jbuf_append(&b, ",\"audio\":{\"input\":{\"format\":{\"type\":\"audio/pcm\","
                        "\"rate\":24000},\"transcription\":{\"model\":");
        jbuf_append_json_str(&b, transcribe);
        jbuf_append(&b, "},\"turn_detection\":{\"type\":");
        jbuf_append_json_str(&b, o->vad ? o->vad : RT_DEFAULT_VAD);
        jbuf_append(&b, ",\"create_response\":false,\"interrupt_response\":false");
        jbuf_append(&b, "}},\"output\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},"
                        "\"voice\":");
        jbuf_append_json_str(&b, o->voice ? o->voice : RT_DEFAULT_VOICE);
        jbuf_append(&b, "}}");
    }

    int ntools = 0;
    jbuf_append(&b, ",\"tools\":[");
    if (!s->no_tools)
        ntools = rt_append_tools(s, &b);
    jbuf_append(&b, "],\"tool_choice\":\"auto\"}}");

    bool ok = rt_ws_send_text(&s->ws, b.data);
    if (ok)
        s->tool_context_dirty = false;
    jbuf_free(&b);
    return ok ? ntools : -1;
}

static int rt_send_tools_update(rt_session_t *s) {
    if (!s || s->no_tools)
        return 0;

    jbuf_t b;
    jbuf_init(&b, 8192);
    jbuf_append(&b, "{\"type\":\"session.update\",\"session\":{\"type\":\"realtime\",\"tools\":[");
    int ntools = rt_append_tools(s, &b);
    jbuf_append(&b, "],\"tool_choice\":\"auto\"}}");

    bool ok = rt_ws_send_text(&s->ws, b.data);
    if (ok)
        s->tool_context_dirty = false;
    jbuf_free(&b);
    return ok ? ntools : -1;
}

static void rt_send_response_create(rt_session_t *s) {
    if (s->tool_context_dirty && !s->no_tools) {
        int n = rt_send_tools_update(s);
        if (n >= 0)
            rt_status(s, "· session.tools updated (%d tools)", n);
    }
    s->response_active = true;
    rt_ws_send_text(&s->ws, "{\"type\":\"response.create\"}");
}

static void rt_send_mic(rt_session_t *s) {
    if (s->text_only || s->stdin_text || !s->audio_started)
        return;
    if (rt_echo_guard_active(s, rt_now_ms())) {
        rt_ring_clear(&s->mic);
        return;
    }
    static uint8_t pcm[RT_CHUNK_BYTES * 5];
    while (rt_ring_size(&s->mic) >= RT_CHUNK_BYTES / 2) {
        size_t n = rt_ring_pop(&s->mic, pcm, sizeof(pcm));
        if (!n)
            break;
        size_t b64cap = 4 * ((n + 2) / 3) + 8;
        char *b64 = malloc(b64cap);
        if (!b64)
            break;
        size_t olen = base64_encode(pcm, n, b64, b64cap);
        if (olen > 0) {
            jbuf_t b;
            jbuf_init(&b, olen + 64);
            jbuf_append(&b, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"");
            jbuf_append(&b, b64);
            jbuf_append(&b, "\"}");
            rt_ws_send_text(&s->ws, b.data);
            jbuf_free(&b);
        }
        free(b64);
    }
}

static void rt_assistant_delta(rt_session_t *s, const char *evt) {
    char *delta = json_get_str(evt, "delta");
    if (!delta)
        return;
    if (!s->assistant_open) {
        fprintf(stdout, "%sdsco ▸%s ", s->color ? "\x1b[36m" : "", s->color ? "\x1b[0m" : "");
        s->assistant_open = true;
    }
    fputs(delta, stdout);
    fflush(stdout);
    free(delta);
}

static void rt_close_assistant_line(rt_session_t *s) {
    if (s->assistant_open) {
        fputc('\n', stdout);
        fflush(stdout);
        s->assistant_open = false;
    }
}

static void rt_text_prompt(rt_session_t *s) {
    if (s->text_only) {
        fprintf(stdout, "%syou ▸%s ", s->color ? "\x1b[32m" : "", s->color ? "\x1b[0m" : "");
        fflush(stdout);
    }
}

static void rt_delete_conversation_item(rt_session_t *s, const char *item_id) {
    if (!item_id || !item_id[0])
        return;
    jbuf_t b;
    jbuf_init(&b, 128);
    jbuf_append(&b, "{\"type\":\"conversation.item.delete\",\"item_id\":");
    jbuf_append_json_str(&b, item_id);
    jbuf_append(&b, "}");
    rt_ws_send_text(&s->ws, b.data);
    jbuf_free(&b);
}

static void rt_dispatch(rt_session_t *s, const char *evt) {
    char *type = json_get_str(evt, "type");
    if (!type)
        return;

    if (strcmp(type, "session.created") == 0) {
        int n = rt_send_session_update(s);
        if (n >= 0)
            rt_status(s, "· session.update sent (%d tools)", n);
    } else if (strcmp(type, "session.updated") == 0) {
        if (!s->ready) {
            s->ready = true;
            const char *mode = s->text_only ? " (text mode)"
                               : s->stdin_text
                                   ? " (voice output, stdin text input)"
                                   : ", speak when ready";
            rt_status(s, "· ready — %s%s. Ctrl-C to hang up.", s->model, mode);
#ifdef RT_HAVE_AUDIO
            if (!s->text_only && !s->audio_started) {
                char aerr[160];
                if (rt_audio_start(s, aerr, sizeof(aerr))) {
                    s->audio_started = true;
                } else {
                    rt_status(s, "audio init failed: %s", aerr);
                    g_rt_stop = 1;
                }
            }
#endif
            rt_text_prompt(s);
        }
    } else if (strcmp(type, "response.created") == 0) {
        s->response_active = true;
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
        if (rt_suppress_audio_input(s, rt_now_ms())) {
            rt_ring_clear(&s->mic);
            rt_ws_send_text(&s->ws, "{\"type\":\"input_audio_buffer.clear\"}");
        } else {
            rt_ring_clear(&s->spk); /* barge-in: stop queued playback immediately */
            if (s->full_duplex && s->response_active)
                rt_ws_send_text(&s->ws, "{\"type\":\"response.cancel\"}");
            rt_close_assistant_line(s);
        }
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        char *tr = json_get_str(evt, "transcript");
        bool suppress_echo = rt_suppress_audio_input(s, rt_now_ms());
        if (suppress_echo) {
            char *item_id = json_get_str(evt, "item_id");
            rt_delete_conversation_item(s, item_id);
            free(item_id);
        } else if (tr && tr[0]) {
            rt_close_assistant_line(s);
            fprintf(stdout, "%syou ▸%s %s\n", s->color ? "\x1b[32m" : "",
                    s->color ? "\x1b[0m" : "", tr);
            fflush(stdout);
            rt_tool_context_append(s, tr);
            if (s->response_active) {
                s->pending_audio_response = true;
            } else {
                rt_send_response_create(s);
            }
        }
        free(tr);
    } else if (strcmp(type, "response.output_audio.delta") == 0) {
        char *b64 = json_get_str(evt, "delta");
        if (b64) {
            s->response_active = true;
            rt_echo_guard_output_started(s);
            size_t bl = strlen(b64);
            uint8_t *pcm = malloc(3 * bl / 4 + 8);
            if (pcm) {
                size_t n = base64_decode(b64, bl, pcm, 3 * bl / 4 + 8);
                if (n)
                    rt_ring_push(&s->spk, pcm, n);
                free(pcm);
            }
            free(b64);
        }
    } else if (strcmp(type, "response.output_audio.done") == 0) {
        rt_echo_guard_output_done(s);
    } else if (strcmp(type, "response.output_audio_transcript.delta") == 0 ||
               strcmp(type, "response.output_text.delta") == 0) {
        s->response_active = true;
        rt_assistant_delta(s, evt);
    } else if (strcmp(type, "response.output_item.added") == 0) {
        char *item = json_get_raw(evt, "item");
        if (item) {
            char *itype = json_get_str(item, "type");
            if (itype && strcmp(itype, "function_call") == 0) {
                char *cid = json_get_str(item, "call_id");
                char *nm = json_get_str(item, "name");
                if (cid && nm)
                    rt_call_map_put(s, cid, nm);
                free(cid);
                free(nm);
            }
            free(itype);
            free(item);
        }
    } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
        rt_dispatch_tool_call(s, evt);
    } else if (strcmp(type, "response.done") == 0) {
        s->response_active = false;
        rt_echo_guard_output_done(s);
        rt_close_assistant_line(s);
        char *resp = json_get_raw(evt, "response");
        if (resp) {
            char *status = json_get_str(resp, "status");
            if (status && strcmp(status, "failed") == 0)
                rt_status(s, "response failed: %.400s", resp);
            free(status);
            free(resp);
        }
        pthread_mutex_lock(&s->outbox_mu);
        bool idle = s->tools_inflight == 0 && s->outbox_n == 0;
        pthread_mutex_unlock(&s->outbox_mu);
        if (s->pending_audio_response) {
            s->pending_audio_response = false;
            rt_send_response_create(s);
        } else if (idle) {
            rt_text_prompt(s);
        }
    } else if (strcmp(type, "error") == 0) {
        char *e = json_get_raw(evt, "error");
        char *msg = e ? json_get_str(e, "message") : NULL;
        rt_status(s, "api error: %s", msg ? msg : (e ? e : evt));
        free(msg);
        free(e);
    }
    free(type);
}

/* ── text-mode stdin turns ────────────────────────────────────────────── */

static void rt_poll_stdin(rt_session_t *s) {
    if (s->stdin_closed)
        return;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0)
        return;
    char line[8192];
    if (!fgets(line, sizeof(line), stdin)) {
        s->stdin_closed = true; /* EOF / Ctrl-D; session loop exits once idle. */
        return;
    }
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (!n) {
        rt_text_prompt(s);
        return;
    }
    if (s->stdin_text) {
        rt_close_assistant_line(s);
        fprintf(stdout, "%syou ▸%s %s\n", s->color ? "\x1b[32m" : "",
                s->color ? "\x1b[0m" : "", line);
        fflush(stdout);
    }
    rt_tool_context_append(s, line);
    jbuf_t b;
    jbuf_init(&b, n + 160);
    jbuf_append(&b, "{\"type\":\"conversation.item.create\",\"item\":{\"type\":\"message\","
                    "\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":");
    jbuf_append_json_str(&b, line);
    jbuf_append(&b, "}]}}");
    rt_ws_send_text(&s->ws, b.data);
    jbuf_free(&b);
    rt_send_response_create(s);
}

/* ── main session loop ────────────────────────────────────────────────── */

int realtime_voice_run(const realtime_opts_t *opts) {
    realtime_opts_t local = opts ? *opts : (realtime_opts_t){0};

#ifndef RT_HAVE_AUDIO
    if (!local.text_only) {
        fprintf(stderr, "dsco voice: no audio backend in this build — use `dsco voice --text`\n");
        return 1;
    }
#endif

    const char *model = local.model;
    if (!model || !model[0])
        model = getenv("DSCO_REALTIME_MODEL");
    if (!model || !model[0])
        model = RT_DEFAULT_MODEL;

    const char *key = getenv("OPENAI_API_KEY");
    if (key && !key[0]) {
        /* A set-but-empty export masks the saved env file (its loader never
         * overwrites an existing var, even an empty one). Clear it first. */
        unsetenv("OPENAI_API_KEY");
        key = NULL;
    }
    if (!key) {
        dsco_setup_load_saved_env(); /* `dsco voice` dispatches before main's env load */
        key = getenv("OPENAI_API_KEY");
    }
    if (!key || !key[0])
        key = provider_resolve_api_key("openai");
    if (!key || !key[0]) {
        fprintf(stderr, "dsco voice: no OpenAI API key (set OPENAI_API_KEY or configure the "
                        "openai provider)\n");
        return 1;
    }

    if (!local.no_tools)
        tools_init();

    /* Heap-allocated: detached tool threads hold this pointer, so it must
     * survive them (leaked deliberately if any are still running at exit). */
    rt_session_t *s = calloc(1, sizeof(*s));
    if (!s)
        return 1;
    s->opts = &local;
    s->model = model;
    s->text_only = local.text_only;
    s->stdin_text = !local.text_only && !isatty(STDIN_FILENO);
    s->no_tools = local.no_tools;
    s->color = isatty(STDOUT_FILENO);
    s->full_duplex = !(local.half_duplex || rt_env_truthy("DSCO_REALTIME_HALF_DUPLEX"));
    s->echo_guard_ms = rt_env_int_clamped("DSCO_REALTIME_ECHO_GUARD_MS",
                                          RT_DEFAULT_ECHO_GUARD_MS, 0, 5000);
    rt_ws_init(&s->ws);
    rt_ring_init(&s->mic);
    rt_ring_init(&s->spk);
    pthread_mutex_init(&s->outbox_mu, NULL);

    struct sigaction sa = {0}, old_sa;
    sa.sa_handler = rt_on_sigint; /* no SA_RESTART: select must EINTR */
    sigaction(SIGINT, &sa, &old_sa);
    g_rt_stop = 0;

    char path[512];
    snprintf(path, sizeof(path), RT_PATH_FMT, model);
    rt_status(s, "· connecting wss://%s%s", RT_HOST, path);

    int rc = 1;
    char err[1024];
    if (!rt_ws_connect(&s->ws, RT_HOST, RT_PORT, path, key, err, sizeof(err))) {
        fprintf(stderr, "dsco voice: %s\n", err);
        goto out;
    }
    if (s->stdin_text)
        rt_status(s, "· connected — non-tty stdin will be sent as text; microphone disabled");
    else if (!s->text_only)
        rt_status(s, "· connected — macOS may prompt for microphone access on first use");

    while (!g_rt_stop && !s->ws.closed) {
        rt_outbox_flush(s);
        rt_send_mic(s);
        char *msg = rt_ws_poll(&s->ws, 20);
        while (msg) {
            rt_dispatch(s, msg);
            free(msg);
            msg = rt_ws_poll(&s->ws, 0);
        }
        if ((s->text_only || s->stdin_text) && s->ready)
            rt_poll_stdin(s);
        if (s->stdin_closed) {
            pthread_mutex_lock(&s->outbox_mu);
            bool idle = !s->response_active && s->tools_inflight == 0 && s->outbox_n == 0;
            pthread_mutex_unlock(&s->outbox_mu);
            if (idle)
                g_rt_stop = 1;
        }
    }
    rc = 0;
    rt_close_assistant_line(s);
    if (s->ws.closed && !g_rt_stop) {
        rt_status(s, "· server closed the session");
    } else {
        /* Give in-flight tools a moment to land, then hang up cleanly. */
        for (int i = 0; i < 50; i++) {
            pthread_mutex_lock(&s->outbox_mu);
            bool busy = s->tools_inflight > 0;
            pthread_mutex_unlock(&s->outbox_mu);
            if (!busy)
                break;
            usleep(100 * 1000);
        }
        rt_outbox_flush(s);
        rt_ws_send(&s->ws, 0x8, "\x03\xe8", 2); /* 1000 normal closure */
        rt_status(s, "· hung up");
    }

out:
#ifdef RT_HAVE_AUDIO
    rt_audio_stop(s); /* synchronous: no callback touches the rings after this */
#endif
    sigaction(SIGINT, &old_sa, NULL);
    rt_ws_free(&s->ws); /* tool threads never touch the socket */
    pthread_mutex_lock(&s->outbox_mu);
    bool busy = s->tools_inflight > 0;
    for (size_t i = 0; i < s->outbox_n; i++)
        free(s->outbox[i]);
    free(s->outbox);
    s->outbox = NULL;
    s->outbox_n = s->outbox_cap = 0;
    pthread_mutex_unlock(&s->outbox_mu);
    if (!busy) {
        rt_ring_free(&s->mic);
        rt_ring_free(&s->spk);
        free(s);
    } /* else: leak s — a detached tool thread still holds it and the process
         is about to exit anyway */
    return rc;
}

#endif /* HAVE_MBEDTLS */
