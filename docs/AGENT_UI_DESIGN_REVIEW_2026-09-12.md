# Agent UI design review — 2026-09-12

## Conclusion

The strongest pattern is separating conversation, ongoing activity, detailed execution evidence, and durable project state. An agent's event log is not its user interface. DSCO's next major improvement should be a semantic presentation layer, not additional border/font tweaks.

Method: current official documentation and selected public source, fetched read-only. No competitor installed or launched; no hands-on latency, accessibility, reliability or scale claims. This is a representative comparison, not an exhaustive inventory of every agentic CLI. Recommendations below are DSCO proposals, not claims that every competitor implements them.

## Products and directly supported observations

### Claude Code
Source: https://code.claude.com/docs/en/interactive-mode

Ctrl+O opens detailed transcript inspection. Documentation explicitly describes default-collapsed MCP activity such as 'Called slack 3 times' and one-line previews of messages from other sessions. Ctrl+B backgrounds running work; /tasks exposes running shells/subagents, separately from the Ctrl+T planning checklist. It supports queued steering, prompt stashing, and a short return-to-session recap. /diff has a narrow-window fallback rather than always squeezing a side panel in.

Takeaway: summarize low-level activity, preserve drill-down, keep the draft usable, distinguish planned tasks from actual processes. Do not copy automatic side-panel opening into DSCO: Arthur's opt-in preference takes precedence.

### OpenAI Codex CLI
Sources:
- https://learn.chatgpt.com/docs/codex/cli
- https://github.com/openai/codex/blob/main/codex-rs/tui/src/exec_cell/render.rs
- https://github.com/openai/codex/blob/main/codex-rs/tui/src/history_cell/mcp.rs
- https://github.com/openai/codex/blob/main/codex-rs/tui/src/status_indicator_widget.rs

Current public source distinguishes display_lines from transcript_lines, groups exploration rendering, and bounds tool-output previews. TOOL_CALL_MAX_LINES is 5, while explicitly user-run shell output has a separate value of 50; this does not mean every tool card is exactly five lines, because the formatter can include head, tail and omission markers. MCP rendering likewise has compact versus transcript modes and raw-line access. Status is a separate widget with a default maximum of three detail lines and reduced-motion behavior.

Takeaway: one underlying operation, multiple faithful views; tailor the preview to intent rather than dumping identical output for agent searches and user-requested shell commands. Source from main is mutable, not a guarantee about every installed release.

### Grok Build — terminal coding agent
Sources:
- https://docs.x.ai/build/overview
- https://docs.x.ai/build/keyboard-shortcuts
- https://docs.x.ai/build/features/background-tasks
- https://docs.x.ai/build/features/dashboard
- https://docs.x.ai/build/features/status-line

Fullscreen mouse-interactive TUI, entry-level expand/collapse, fullscreen inspection, copying and raw Markdown. A tasks pane is separate from todos. Its optional session dashboard groups Needs input, Working, Idle, Inactive, Completed and Failed, with peek/attach and inline replies. The resource status line is off by default. Background monitors can inject each printed line into the conversation; documentation explicitly warns to keep scripts selective.

Takeaway: reveal detail on demand, group by whether human attention is needed, keep metrics optional, and filter event streams before they become interruptions. Do not copy key bindings blindly: Grok's Ctrl+O toggles always-approve, unlike Claude's transcript shortcut. Presentation controls must never accidentally change DSCO authority.

### Grok Bot — persistent teammates, not the CLI
Source: https://docs.x.ai/grok-bot/overview (updated September 11, 2026).

Named Bots retain role context, preferences and memory, operate through messaging, hand off to one another, and use skills/routines. They run on a persistent cloud computer, so closing the user's laptop does not stop work. The documentation says Bots in an account share that computer's files/browser sessions/logins, while conversations and learned context remain per Bot. Approval and human-required steps return to the user.

Takeaway: identity and task ownership persist beyond a chat; the user should not route every worker message. Shared workspace is also a security boundary: do not mistake several Bot names for isolated credentials or files.

### Cursor Projects
Source: https://cursor.com/changelog/projects (September 10, 2026).

Projects is a beta rollout for larger bodies of work: features, migrations, or apps. Cursor says the coordinator maintains context over months, delegates to potentially thousands of subagents, and handles recurring work. The coordinator itself does not write code; it plans, delegates implementation and brings results back for review. A Project has a cloud computer; local agents handle work requiring the user's machine. Shared files sync across agent machines and accumulate research, artifacts, project knowledge and workflow preferences. Subscriptions can react to Slack, schedules and PR activity.

These are vendor-documented capabilities, not independently verified scale/reliability results. 'Months of context' should not be interpreted as a single infinitely growing model context window. Persistent shared artifacts and selective retrieval are the useful architectural lesson; undocumented compaction internals remain unknown.

### OpenCode
Source: https://opencode.ai/docs/tui/

/details controls execution detail visibility. /thinking changes display, not reasoning capability. TUI configuration is separate from runtime configuration, with adaptive/stacked diffs, scroll controls, session switching and a mouse-off option that preserves native terminal selection. Attention notifications/sounds are disabled by default.

Takeaway: presentation preferences are independent of execution, and inspection must not cost task continuity or normal text handling.

### Gemini CLI
Source: https://geminicli.com/docs/reference/keyboard-shortcuts

Ctrl+O expands/collapses content outside alternate-buffer mode; F12 opens debug details. Double Tab selects minimal/full UI and persists the preference. Dedicated background-shell controls, prompt queuing, mouse mode and copy mode distinguish conversation interaction from process interaction.

Takeaway: provide a persistent minimal view and make copying a first-class behavior. Document platform/context differences instead of promising a universal key chord.

### Aider and Crush
Sources:
- https://aider.chat/docs/usage/modes.html
- https://aider.chat/docs/usage/watch.html
- https://github.com/charmbracelet/crush/blob/main/README.md

Aider visibly distinguishes ask/code/architect modes and can accept explicit instructions embedded in watched files; this allows intent to originate beside the artifact rather than in another chat. Its architect/editor workflow has explicit additional-request cost. Crush documents per-project sessions, context-preserving model changes and shared daemon-backed workspace state; IsBusy and AttachedClients distinguish executing work from attached viewers.

Takeaway: preserve artifact-centric work and distinguish task lifetime from client attachment. Neither is evidence that every visually polished CLI solves long-lived project coordination.

## Proposed DSCO interaction contract

Default view: human conversation, final results, one bounded live activity summary. Successful routine tool calls do not become independent chat messages. Opening details reveals a chronological task/operation view; raw logs remain another deliberate level deeper. Errors, approval requests, exhausted budgets, verified completion and material changes in scope must stay discoverable and appropriately salient.

Example default:

    Improve compositor readability
    Working · simplifying transcript · 2 tasks active
    [Activity] [Changes] [Pause]

    You: Keep the text large.
    DSCO: The current text size will stay unchanged.

    [Your draft remains here]

Example completion:

    Header and transcript changes ready for review
    3 files changed · checks passed · installed, not yet active in this session
    [Review changes] [Evidence]

Never manufacture progress percentages or success summaries. A running process, recent heartbeat and meaningful task progress are distinct states. Show a stalled/unknown state honestly rather than animating forever. Keep worker chatter inside the relevant task except for decisions needing Arthur.

### Engineering shape

    durable execution events
        -> reducer keyed by task_id / operation_id
        -> task and operation state
        -> conversation / activity / evidence projections
        -> retained compositor

Correlate start, progress, retry and finish by the same operation ID. Treat discovery/loading/invocation wrappers as implementation detail under the meaningful target operation, while retaining the whole causal chain in evidence. Coalesce bursts of read/search work into an honest summary. Keep raw stdout/stderr attached to its operation rather than classifying arbitrary text as an assistant message. Render live status in place; append durable conversation messages only for semantic updates. Preserve viewport anchor, selection and draft during all updates. Filtering display must never change audit retention, tool authorization, cancellation or the model's available evidence.

## Persistent-project model for DSCO

Project should outlive turns, contexts and UI processes. Retain:
- goal, acceptance, authority and budget;
- task dependencies, owners, leases and running locations;
- versioned decisions and shared knowledge with provenance;
- artifacts, checks and review status;
- subscriptions with explicit scope, deduplication and backpressure;
- a concise 'since you last visited' recap and a needs-attention inbox.

Use local-first durable state and a headless service. A local laptop cannot execute while asleep; continued work then requires an explicitly configured always-on or remote executor. Shared knowledge is not blanket shared secrets. Project learning needs provenance, staleness handling and reversible promotion; don't turn every worker note into authoritative memory.

DSCO already has pieces named goal_queue, durable_agents, IPC, buffers and event storage. Their existence does not establish this integrated product experience. The missing deliverable is coherent ownership and continuity presented to Arthur, not an always-growing transcript.

## Priority order and acceptance

1. Conversation-first semantic projection. A 100-tool task must not produce 100 default-visible chat messages; failures still surface and every tool has evidence drill-down.
2. Stable, selectable, readable text. Draft/selection/scroll anchoring survives updates; semantic zoom is separate from backing DPI; narrow layouts reflow. No autoplay panels or unsolicited windows.
3. Explicit Activity and Changes views inside the existing surface. Hidden activity continues; closing a view does not cancel work. Pause and cancel have distinct documented effects.
4. Durable Project home, recap and review inbox. Reattach after UI restart without duplicate execution; distinguish queued/running/blocked/needs review/verified complete.
5. Authorized subscriptions and remote continuation. Test event deduplication, budget exhaustion, stale knowledge, worker loss and destination-specific authority before unattended rollout.

Recommendation: borrow Claude's compact activity, Codex's semantic display-versus-evidence separation, Grok Build's attention states, and Cursor/Grok Bot's persistent ownership. Do not copy their chrome or expand authority to obtain a quieter UI.
