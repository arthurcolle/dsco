# LOOP-F1-F4: Agentic sampling loop hardening (src/agent.c)

Status: PROPOSAL — pending operator sign (per PRAXIS-gate discipline).
Scope: src/agent.c ONLY. Surgical diffs; no schema/ABI changes. Does not
touch any G2b immune-gated module; the loop-control decide interface is
used as-is, unchanged.
Origin: loop review 2026-07-01 (fable-5 session / emergency checkout).

---

## F1 — refusal stop_reason handling (HIGH — Fable 5 live 2026-07-01)

Fable 5 returns stop_reason "refusal" as HTTP 200 (not an error), reporting
which classifier declined. Today the loop classifies this as a silent
TURN_STOP_DONE: no fallback retry, no telemetry, user sees nothing.

### F1a — enum + name (agent.c ~lines 133-171)

    TURN_CONTINUE_PAUSE_RESUME,
  + TURN_CONTINUE_REFUSAL_FALLBACK,  /* refusal, retrying on fallback */
    TURN_STOP_DONE,
  + TURN_STOP_REFUSED,               /* refusal, no fallback available */

turn_transition_name() gains "refusal_fallback" and "refused".

### F1b — detection + routing (insert after pause_turn detection, ~9756)

    /* Fable-5 class refusal: HTTP 200 + stop_reason "refusal".
     * Not an error; route through the existing fallback chain once.
     * One retry only — a refusal loop is a policy signal, not noise. */
    bool refusal_turn =
        (sr.ok && sr.parsed.stop_reason &&
         strcmp(sr.parsed.stop_reason, "refusal") == 0);
    if (refusal_turn && !needs_followup_turn) {
        if (!refusal_retried && session.fallback_count > 0) {
            refusal_retried = true;
            fprintf(stderr, "  %s* model refused — retrying on fallback chain%s\n",
                    TUI_YELLOW, TUI_RESET);
            baseline_log("agent", "refusal_fallback", session.model, NULL);
            provider_failover_mark(&failed_providers,
                                   g_provider ? g_provider->name : NULL);
            needs_followup_turn = true;   /* re-enter loop body */
        } else {
            tui_warning("model refused; no fallback available — ending turn");
            baseline_log("agent", "refusal_final", session.model, NULL);
        }
    }

Declare bool refusal_retried = false; beside structured_repair_attempts
(per-prompt scope). Transition classifier gains:

    else if (refusal_turn && refusal_retried)
        transition = TURN_CONTINUE_REFUSAL_FALLBACK;
    else if (refusal_turn)
        transition = TURN_STOP_REFUSED;

Billing: refusals with no output are not billed; fallback credit refunds the
prompt-cache switch cost. No budget-accounting change needed.

## F2 — Classify the three unclassified stop paths

All three break out of the loop before transition telemetry fires. Add
baseline_log("agent","turn_transition",...) immediately before each break:

1. Budget stop (~8083): TURN_STOP_BUDGET after the tui_error line.
2. Rate-limiter/interrupt stop (~8089): TURN_STOP_INTERRUPTED when
   g_interrupted, else TURN_STOP_BUDGET (rate).
3. Stream-error break (~8548): TURN_STOP_ERROR after the stream_failed log.

## F3 — Move IPC poll before transition classification

Today: transition logged (+ completion bell fired) -> THEN ipc_poll() can
flip done=false. Telemetry says stopped; loop continues. Move the
ipc_heartbeat()/ipc_poll()/message-injection block (~9891-9920) to just
BEFORE the Phase 6 classification block (~9862). Injected [IPC] messages
then yield TURN_CONTINUE_TOOL_RESULTS and suppress the premature bell.

## F4 — Per-prompt reset of function statics

stall_streak and the si_prev_* snapshot pack are function-static: they
persist across prompts within a session and across resume_turn_loop
re-entry. Preferred fix: hoist both into a loop_prompt_state_t declared
beside turn_arena — no statics, no reset protocol. Minimal fix: explicit
resets at prompt start beside arena_init.

---

## Verification plan (pre-sign)
1. Build: make -j clean, zero new warnings.
2. Unit: mock stream_result with stop_reason="refusal"; assert fallback walk
   entered once, then TURN_STOP_REFUSED on second refusal.
3. Telemetry: run 3 prompts (normal / budget-capped / interrupted); assert
   exactly one turn_transition per prompt end in the baseline log.
4. Rollback: single-file revert of the src/agent.c hunk set.
