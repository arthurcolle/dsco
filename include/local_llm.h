#ifndef DSCO_LOCAL_LLM_H
#define DSCO_LOCAL_LLM_H

/* ── Local LLM discovery (Ollama / LM Studio / MLX) ───────────────────────
 *
 * Probes the well-known OpenAI-compatible local inference servers, lists the
 * models each one currently serves, and resolves a sensible context window.
 * Used by the `/models` command, `/setup report`, and `/model <server>:<name>`
 * context-window resolution.
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char server[16];     /* "ollama" | "lmstudio" | "mlx" */
    char model[128];     /* model id as served */
    char qualified[160]; /* "<server>:<model>" — use with /model */
    int  context_window; /* tokens (best-effort; 0 if unknown) */
} local_model_t;

typedef struct {
    char server[16];
    char base_url[64]; /* OpenAI-compatible base, e.g. http://localhost:11434/v1 */
    bool up;
    int  model_count;
} local_server_t;

/* Probe all known local servers. Fills up to max entries, returns the count. */
int local_llm_probe_servers(local_server_t *out, int max);

/* List models across all reachable local servers. Returns count written. */
int local_llm_list_models(local_model_t *out, int max);

/* True if the given local server ("ollama"/"lmstudio"/"mlx") answers. */
bool local_llm_server_up(const char *server);

/* Resolve the OpenAI-compatible base URL for a server name (honors
 * <SERVER>_API_BASE overrides). Returns false if the name is unknown. */
bool local_llm_base_url(const char *server, char *out, size_t out_len);

/* Best-effort context window for "<server>:<model>" or a bare local model.
 * Returns 0 when it cannot be determined. */
int local_llm_context_window(const char *server, const char *model);

/* True when `name` looks like a local model reference: has an "ollama:",
 * "lmstudio:", "mlx:", or "local:" prefix. */
bool local_llm_is_local_ref(const char *name);

#endif /* DSCO_LOCAL_LLM_H */
