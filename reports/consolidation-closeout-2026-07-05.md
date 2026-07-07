# SCO-CLI Consolidation Close-Out - 2026-07-05

## Step-0 probe outputs

### P1
```text
2ce3538 classification: sovereign L0-L3 taxonomy — doctrine + enforced memory gates
 M Makefile
 M data/constants_env_index.json
 M docs/API_REFERENCE.md
 M docs/CONSTANTS_ENV_INDEX.md
 M docs/EXTERNAL_TOOL_CATALOG.md
 M docs/REPO_COVERAGE.md
 M docs/TOOL_CATALOG.md
 M include/plan_cache.h
 M include/supervisor.h
 M include/tools.h
 M include/tui.h
 M src/agent.c
 M src/dsco_pool.c
 M src/md.c
 M src/memory_tier.c
 M src/orchestrator.c
 M src/plan_cache.c
 M src/provider_pool.c
 M src/supervisor.c
 M src/tools.c
 M src/tui.c
 M tests/test.c
?? add.c
?? graphsub-domain-modal.py
?? graphsub-next-layout.tsx
?? graphsub-next-page.tsx
?? graphsub-phosphor-sota.html
?? include/compute.h
?? include/construct.h
?? include/prompt_pool.h
?? levitate/
?? logo_crop_local.png
?? mobius
?? mobius.c
?? patches/
?? prompts/
?? proposals/context_tools_test_plan.md
?? quine
?? quine.c
?? quine_out.txt
?? reports/
?? scripts/model_resolution_sim.py
?? scripts/scenario_model.py
?? src/bus_cli.c
?? src/compute.c
?? src/construct.c
?? src/prompt_pool.c
```

### P2
```text
1452:static void tool_registry_rdlock(void) {
1462:static void tool_registry_unlock(void) {
27750:            int idx = tools_lookup_index(names[batch + i]);
27799:        int idx = tools_lookup_index(names[i]);
35406:        int ti = tools_lookup_index(g_cooc->names[top[i].ci]);
35691:            int idx = tools_lookup_index(g_hints[hi].tools[t]);
36221:    tool_registry_unlock();
36262:    tool_registry_rdlock();
36264:    tool_registry_unlock();
36268:int tools_lookup_index(const char *name) {
36321:    tool_registry_unlock();
36345:            tool_registry_unlock();
36351:        tool_registry_unlock();
36367:    tool_registry_unlock();
36379:    tool_registry_unlock();
36675:        int idx = tools_lookup_index(name);
36739:                int hot_idx = tools_lookup_index(name);
36816:        tool_registry_rdlock();
36821:        tool_registry_unlock();
38356:    tool_registry_rdlock();
38366:    tool_registry_unlock();
```

### P3
```text
38079:static bool g_locks_global_initialized = false;
38081:static void dsco_locks_init_fields(dsco_locks_t *l) {
38097:        if (!g_locks_global_initialized) {
38098:            dsco_locks_init_fields(l);
38099:            g_locks_global_initialized = true;
38104:    dsco_locks_init_fields(l);
38127:        if (g_locks_global_initialized) {
38129:            g_locks_global_initialized = false;
```

### P4
```text
src/tools.c:29731:     "Can bundle model access, web search, image generation, TTS, and browser services."},
src/tools.c:29773:    {"trajectory_research", "research", "Batch trajectory generation and compression",
```

### P5
```text
src/plan_cache.c:745:/* ── plan_cache_adapt ────────────────────────────────────────────────────── */
src/plan_cache.c:835:char *plan_cache_adapt(const plan_cache_entry_t *entry, const char *new_task) {
include/plan_cache.h:31: *       char *adapted = plan_cache_adapt(e, new_task);
include/plan_cache.h:122:char *plan_cache_adapt(const plan_cache_entry_t *entry, const char *new_task);
```

### P6
```text
136:static void pool_limits_save(void) {
278:        pool_limits_save();
305:            pool_limits_save();
343:    pool_limits_save();
357:        pool_limits_save();
375:        pool_limits_save();
424:            pool_limits_save();
```

### P7
```text
src/llm.c:2585: * DSCO_TOOL_FREEZE=0 to restore volatile per-turn paging for A/B tests.
src/llm.c:2595:    const char *v = getenv("DSCO_TOOL_FREEZE");
```

### P8
```text
542:static void append_cache_control_to_last_block(jbuf_t *b);
2351:static bool content_block_allows_cache_control(const msg_content_t *mc) {
2354:    /* Empty text blocks cannot carry cache_control (API rejects them). */
2367:            if (content_block_is_sendable(mc) && content_block_allows_cache_control(mc))
2378:        if (content_block_allows_cache_control(mc))
2380:        if (cache_mark_last && content_block_allows_cache_control(mc) && remaining_cacheable == 0)
2381:            append_cache_control_to_last_block(b);
2650:/* The cache_control JSON fragment (no leading/trailing separator). */
2651:static const char *cache_control_json(void) {
2652:    return anthropic_cache_ttl_1h() ? "\"cache_control\":{\"type\":\"ephemeral\",\"ttl\":\"1h\"}"
2653:                                    : "\"cache_control\":{\"type\":\"ephemeral\"}";
2656:/* Append a cache_control breakpoint to the content block just emitted. */
2657:static void append_cache_control_to_last_block(jbuf_t *b) {
2663:    jbuf_append(b, cache_control_json());
2680:        jbuf_append(b, cache_control_json());
2875:     * cache_control. The final mark advances the cache each turn; the
3162:    jbuf_append(&b, cache_control_json());
3247:    jbuf_append(&b, cache_control_json());
src/llm.c:2971:                    jbuf_append(b, "}]}");
src/llm.c:2975:                    jbuf_append(b, "}]}");
src/llm.c:2985:        jbuf_append(b, "}]}");
```

### P9
```text
dsco_dht.h
memory_tier.h
session_memory.h
vecstore.h
vecstore_metal.h
```

### P10
```text
29:typedef struct dsco_dht dsco_dht_t;
31:typedef struct {
37:typedef struct {
```

### P11
```text
     362 src/construct.c
     555 src/prompt_pool.c
     917 total
==> src/construct.c <==
#include "construct.h"
#include "tools.h"
#include "json_util.h"
#include "env_config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Contextual Construct — process-lifecycle control plane ────────────────
 *
 * Implementation notes:
 *  - Policy table is a small fixed array guarded by a mutex. Policies are
 *    keyed by tool name; insert-or-update semantics via construct_protect.
 *  - The supervisor thread wakes every DSCO_CONSTRUCT_TICK_MS (default 1000ms)
 *    and runs construct_tick(): snapshot in-flight watchdogs, and for each
 *    protected tool whose remaining deadline has dropped below its low-water
 *    mark, renew via watchdog_renew_by_name — unless the estimated total
 *    lifetime would exceed the policy's max_lifetime_s cap.
 *  - Lifetime estimation: the snapshot exposes timeout_s and renew_count but
 *    not started_at, so the cap check uses the conservative estimate
 *    timeout_s + renew_count * renew_quantum_s. watchdog_renew additionally
 *    honors the per-watchdog max_lifetime_s field as a hard backstop.
 *  - Priority raises the effective low-water mark (renew earlier), so under
 *    scheduling jitter CRITICAL work is renewed with the most margin:
 *      effective_low_water = low_water_s * (2 + priority) / 2
 *  - Env gates: DSCO_CONSTRUCT=0 disables the supervisor thread entirely;
 *    DSCO_CONSTRUCT_TICK_MS tunes cadence (50..60000).
 */

#define CONSTRUCT_MAX_POLICIES 64
#define CONSTRUCT_SNAPSHOT_MAX 128

static construct_policy_t s_policies[CONSTRUCT_MAX_POLICIES];
static int s_policy_count = 0;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t s_thread;
static volatile int s_running = 0;
static int s_started = 0;

static volatile unsigned long s_ticks = 0;
static volatile unsigned long s_total_renewals = 0;

/* ── Priority helpers ─────────────────────────────────────────────────── */

static const char *prio_name(construct_priority_t p) {
    switch (p) {
    case CONSTRUCT_PRIO_IDLE: return "idle";
    case CONSTRUCT_PRIO_LOW: return "low";
    case CONSTRUCT_PRIO_NORMAL: return "normal";
    case CONSTRUCT_PRIO_HIGH: return "high";
    case CONSTRUCT_PRIO_CRITICAL: return "critical";
    default: return "unknown";
    }
}

static construct_priority_t prio_parse(const char *s) {

==> src/prompt_pool.c <==
#include "prompt_pool.h"
#include "tools.h"
#include "json_util.h"
#include "env_config.h"
#include "http_pool.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Prompt Pool — autonomous, ever-growing prompt suggestion cache ──────── */

#define POOL_MAX 65536
#define POOL_SEED_FLOOR 1000
#define POOL_PROMPT_MAX 512
#define POOL_HASH_BUCKETS 131072 /* power of two; open-addressed FNV set */

static char **s_prompts = NULL;
static int s_count = 0;
static unsigned long long *s_hashes = NULL; /* 0 = empty slot */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t s_thread;
static volatile int s_running = 0;
static int s_started = 0;
static int s_initialized = 0;

static volatile unsigned long s_news_added = 0;
static volatile unsigned long s_refreshes = 0;
static volatile time_t s_last_refresh = 0;
```

### P12
```text
Makefile:144:ASAN_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -fsanitize=address
Makefile:146:ASAN_LDFLAGS = -fsanitize=address
Makefile:147:UBSAN_CFLAGS = $(BASE_CFLAGS) -O0 -g -fno-omit-frame-pointer -fno-inline -fsanitize=undefined -fno-sanitize-recover=all
Makefile:149:UBSAN_LDFLAGS = -fsanitize=undefined -fno-sanitize-recover=all
Makefile:151:PROFILE_COVERAGE_FLAGS = -finstrument-functions -fsanitize-coverage=trace-pc-guard,trace-cmp,indirect-calls,trace-div
Makefile:488:$(OBJ_DIR)/instrumenter.o: CFLAGS := $(filter-out $(PROFILE_COVERAGE_FLAGS),$(CFLAGS)) -fsanitize-coverage=0
```

## Per-WP status

- WP1 DONE - Makefile sanitizer targets added: `test_plan_cache_tsan`, `test_runner_tsan`, `test_runner_asan`. Sanitizer flags filter `_FORTIFY_SOURCE=2` to avoid clang sanitizer macro redefinition warnings. Builds complete; `test_runner_tsan` runtime blocked by `output_guard` finding.
- WP2 DONE - `src/tools.c`, `include/tools.h`, `tests/test.c`. Added registry-wide external callback in-flight drain/generation and reset stress test. `./test_runner`: 5059/5059; ASan/UBSan: 5059/5059; TSan reached and passed the new stress test before reporting unrelated `output_guard`.
- WP3 DONE - `include/tools.h`, `src/tools.c`, `src/provider.c`, `src/llm.c`, `src/realtime.c`, `src/agent.c`. External registry public borrows converted to count/snapshot/copy-out APIs.
- WP4 DONE - `include/plan_cache.h`, `src/plan_cache.c`, `tests/test_plan_cache.c`. `plan_cache_adapt` now validates expected task hash under lock. `./test_plan_cache`: 629/629; `./test_plan_cache_tsan`: 629/629.
- WP5 DONE - `src/provider_pool.c`, `tests/test.c`. Pool limit persistence now snapshots under `g_pool_mu` and writes outside it; shutdown flushes synchronously.
- WP6 DONE - `src/tools.c`. Kept global lock-bundle flag pattern and documented it because `src/agent.c` destroys the global bundle; `pthread_once` cannot express re-init after destroy.
- WP7 ALREADY-PRESENT - Freeze default, advisory hot-marking, advancing history breakpoint, and cache-write cost accounting verified. Live Anthropic proof: request 1 read=0/write=24365; request 2 read=24365/write=21; request 3 read=24386/write=21.
- WP8 DONE - `include/dsco_swim.h`, `src/dsco_swim.c`, `include/sequence_state.h`, `src/sequence_state.c`, `include/dsco_dht.h`, `src/dsco_dht.c`, `Makefile`, `tests/test.c`. Added SWIM state machine, DHT k-bucket internals, sequence state record, and tests.
- WP9 SPEC-ONLY - `docs/fisher-ewc-spec.md`. The repo has `memory_entry_t` but no 64-byte Node/propose_mutation substrate, so no compile-time implementation was added.
- WP10 DONE - `src/construct.c`, `src/prompt_pool.c`. Fixed unsynchronized lifecycle/stat counters with atomics and moved prompt-pool JSONL appends outside `s_lock`. Both files compile standalone.

## Final gate evidence

- `make -j4 dsco`: PASS. Existing warning remains: macOS deprecation for `posix_spawn_file_actions_addchdir_np`.
- `git diff --check`: PASS.
- `./test_plan_cache`: PASS, 629/629 tests, 4835 checks.
- `./test_runner`: PASS, 5059/5059 tests.
- `./test_plan_cache_tsan`: PASS, 629/629 tests, 4835 checks.
- `./test_runner_tsan`: FAILED-GATE for out-of-scope `output_guard` race. Report captured in `/tmp/test_runner_tsan_report.log` and summarized in `FINDINGS.md`.
- `./test_runner_asan`: PASS, 5059/5059 tests with `ASAN_OPTIONS='detect_leaks=0:halt_on_error=1' UBSAN_OPTIONS='halt_on_error=1'`.
- WP7 live proof: PASS, cache reads strictly increased and cache writes were positive on requests 2 and 3.

## Decisions

- WP1: used dedicated sanitizer object directories and filtered `_FORTIFY_SOURCE=2` from sanitizer CFLAGS because clang sanitizers predefine it.
- WP2: used registry-wide in-flight drain, not per-slot refcounts, because reset frequency is low and the brief explicitly selected registry-wide drain as sufficient.
- WP3: left `tools_get_all` as a borrowed pointer API because it returns immutable built-in `s_tools`, not resettable external registry storage.
- WP5: wrote provider-pool state by snapshotting persistable slots under lock and doing all filesystem work after unlock.
- WP6: kept flag-based global lock initialization because `src/agent.c` calls `dsco_locks_destroy(&g_locks)`.
- WP7: no code patch applied because all three economics claims were already true in this tree.
- WP8: implemented only the three NEW/COMPOSE items; did not add refused structures.
- WP9: delivered spec-only because the 64-byte Node/propose_mutation substrate is not present in this repo.
- WP10: applied minimal invariant fixes to untracked modules and did not wire them into the build.
- Commits: no commits were created. The initial dirty tree had overlapping tracked files in the requested work areas, so staging WP-by-WP would have captured unrelated user changes.

## WP3 ownership table

| Function/surface | Old contract | New contract |
| --- | --- | --- |
| `tools_schema_for_name` | Static helper returned borrowed schema pointer from built-in/external storage after unlock | Replaced by `tools_schema_copy_for_name`, returning owned heap copy; callers free |
| `g_external_tools`, `g_external_tool_count` | Public externs allowed direct borrowed reads of resettable registry storage | Hidden behind `DSCO_INTERNAL_TESTS`; production uses `tools_external_count` / `tools_external_snapshot` |
| external snapshot call sites | Direct borrowed pointer iteration | Deep-copy snapshot; `input_schema_json` duplicated; cb/ctx omitted from snapshot |
| `g_tool_map` teardown | Public extern/free from `agent.c` | `tools_registry_map_free()` owns teardown under registry write lock |
| `tools_get_all` | Borrowed built-in tool definitions | Unchanged; data is immutable static `s_tools`, not resettable external registry storage |

## WP2 rdlock-region enumeration

- Tool-map alias/name lookup: rdlock protects lookup only; no post-unlock borrowed referents are used.
- External dispatch: rdlock resolves slot and snapshots `cb`/`cb_ctx` while incrementing `g_ext_inflight`; callback runs after unlock; reset drains before freeing/reusing referents.
- Schema lookup: previously borrowed external schema after unlock; WP3 converted to copy-out.
- Reset/build paths: write lock publishes/unpublishes registry state; no external callbacks or file I/O are invoked under registry lock.

## WP10 audit

- `construct.c`: process-lifecycle control plane for watchdog renewal policies. Shared state: policy table/count under `s_lock`, supervisor thread lifecycle, counters. Fixed unsynchronized `s_running`, `s_started`, `s_ticks`, and `s_total_renewals` with atomics and serialized start/stop.
- `prompt_pool.c`: persistent prompt suggestion cache. Shared state: prompt pointer array, count, hash set under `s_lock`, refresher lifecycle and stats. Prompts are content-addressed by FNV hash and immutable post-registration. Fixed atomics for lifecycle/stats and moved append-only JSONL writes outside `s_lock`.

## Deltas

No scoped commit diff exists because commits were not created. Scoped implementation paths from this pass:

```text
Makefile
FINDINGS.md
docs/fisher-ewc-spec.md
include/dsco_dht.h
include/dsco_swim.h
include/plan_cache.h
include/sequence_state.h
include/tools.h
reports/consolidation-closeout-2026-07-05.md
src/agent.c
src/construct.c
src/dsco_dht.c
src/dsco_swim.c
src/llm.c
src/plan_cache.c
src/prompt_pool.c
src/provider.c
src/provider_pool.c
src/realtime.c
src/sequence_state.c
src/tools.c
tests/test.c
tests/test_plan_cache.c
```

Unrelated dirty files were not intentionally changed; initial dirty state was preserved.
