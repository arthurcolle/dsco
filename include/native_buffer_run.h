#ifndef DSCO_NATIVE_BUFFER_RUN_H
#define DSCO_NATIVE_BUFFER_RUN_H

#include "native_windows.h"
#include <stdbool.h>
#include <stddef.h>

/* A deliberately small seam for focused tests and coordinator integration.
 * Production defaults call the governed DSCO tools and native window model. */
typedef bool (*native_buffer_run_execute_fn)(const char *name,
    const char *input_json, const char *tier, char *result, size_t result_len);
typedef void (*native_buffer_run_snapshot_fn)(native_windows_snapshot_t *out);
typedef bool (*native_buffer_run_bind_fn)(uint64_t id, const char *title,
    const char *text, const char *buffer_id, const char *revision,
    const char *workspace, bool sensitive, char *result, size_t result_len);
typedef bool (*native_buffer_run_cwd_fn)(char *out, size_t out_len);

typedef struct {
    native_buffer_run_execute_fn execute;
    native_buffer_run_snapshot_fn snapshot;
    native_buffer_run_bind_fn bind_buffer;
    native_buffer_run_cwd_fn getcwd;
} native_buffer_run_hooks_t;

/* Optional test/coordinator seams. A NULL member restores its production path.
 * Hooks are copied under the adapter mutex and invoked only after it is
 * released. They do not grant authority: execute must remain governed. */
void native_buffer_run_set_hooks(const native_buffer_run_hooks_t *hooks);

/* Clears an idle adapter. It refuses to clear a launch, collection, or running
 * handle, so tests cannot accidentally discard a live cancellation handle. */
bool native_buffer_run_reset(void);

/* Ctrl-R (0x12) starts the focused non-empty selection, Ctrl-O (0x0f)
 * collects and presents a requested result, and Ctrl-K (0x0b) cancels through
 * KillShell and retains the partial receipt. */
bool native_buffer_run_key(int key);

#endif
