#include "arena_alloc.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ── Global arenas ─────────────────────────────────────────────────── */

static arena_t g_scratch;
static arena_t g_session;

/* ── Stats counters ────────────────────────────────────────────────── */

static size_t s_scratch_resets = 0;
static size_t s_temp_scopes = 0;

/* ── Accessors ─────────────────────────────────────────────────────── */

arena_t *arena_scratch(void) {
    return &g_scratch;
}
arena_t *arena_session(void) {
    return &g_session;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void arena_subsystem_init(void) {
    arena_init(&g_scratch);
    arena_init(&g_session);
    s_scratch_resets = 0;
    s_temp_scopes = 0;
}

void arena_subsystem_shutdown(void) {
    arena_free(&g_scratch);
    arena_free(&g_session);
}

void arena_scratch_reset(void) {
    arena_reset(&g_scratch);
    s_scratch_resets++;
}

/* ── Scratch convenience ───────────────────────────────────────────── */

void *scratch_alloc(size_t size) {
    return arena_alloc(&g_scratch, size);
}

char *scratch_strdup(const char *s) {
    if (!s)
        return NULL;
    return arena_strdup(&g_scratch, s);
}

char *scratch_sprintf(const char *fmt, ...) {
    /* Two-pass: measure then allocate */
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (n < 0) {
        va_end(args2);
        return NULL;
    }
    char *buf = arena_alloc(&g_scratch, (size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, args2);
    va_end(args2);
    return buf;
}

/* ── Session convenience ───────────────────────────────────────────── */

void *session_alloc(size_t size) {
    return arena_alloc(&g_session, size);
}

char *session_strdup(const char *s) {
    if (!s)
        return NULL;
    return arena_strdup(&g_session, s);
}

char *session_sprintf(const char *fmt, ...) {
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (n < 0) {
        va_end(args2);
        return NULL;
    }
    char *buf = arena_alloc(&g_session, (size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, args2);
    va_end(args2);
    return buf;
}

/* ── Temp scope ────────────────────────────────────────────────────── */

arena_temp_t arena_temp_begin(void) {
    arena_temp_t t;
    t.saved_used = g_scratch.head ? g_scratch.head->used : 0;
    s_temp_scopes++;
    return t;
}

void arena_temp_end(arena_temp_t mark) {
    /* Rewind head chunk to saved offset.
       Note: this is a lightweight rewind — doesn't free chunks,
       just resets the used pointer. Only valid for the head chunk. */
    if (g_scratch.head) {
        g_scratch.head->used = mark.saved_used;
    }
}

/* ── Stats ─────────────────────────────────────────────────────────── */

arena_stats_t arena_get_stats(void) {
    arena_stats_t st;
    memset(&st, 0, sizeof(st));
    st.scratch_bytes_allocated = g_scratch.total_allocated;
    st.session_bytes_allocated = g_session.total_allocated;
    st.scratch_resets = s_scratch_resets;
    st.temp_scopes = s_temp_scopes;
    return st;
}
