# Claude Code Pure Adapter Map

Generated: 2026-05-17
Source checkout: `/Users/arthurcolle/claude-code-pure`
Target repo: `dsco-cli`

This document is the working map for making DSCO a near-perfect adapter for the Claude Code Pure tool/session surface. It focuses on the pieces that affect UI, multi-session management, chat usefulness, background work, and dynamic tool availability.

## Review Scope

I walked the Claude Code Pure checkout at source level, excluding dependency/build metadata (`node_modules`, `.git`, `dist`). That source walk covers 2,503 files. A non-`node_modules` walk including generated/build metadata counted 2,535 files.

Top-level source inventory:

| Area | File count | Adapter relevance |
|---|---:|---|
| `src` | 2,020 | Tool registry, UI components, session state, permissions, MCP, agents, worktrees, cron, skills. |
| `web` | 349 | Remote/control UI behavior and connector surfaces. |
| `scripts` | 22 | Build/dev support. |
| `prompts` | 17 | Prompt assets that shape tool behavior. |
| `mcp-server` | 15 | MCP-facing server support. |
| `helm`, `.github`, `docker`, `grafana`, `docs`, `tests` | 53 | Deployment, test, and operations context. |
| root config/files | 27 | Package/build/config metadata. |

The complete `src/tools` implementation tree has 184 files. Every tool file is enumerated in the appendix so DSCO can track parity by path, not by memory.

Primary files read for adapter semantics:

| File | Why it matters |
|---|---|
| `src/Tool.ts` | Defines the complete tool contract: aliases, JSON schema, permissions, concurrency, deferral, UI renderers, MCP flags, LSP flags, result mapping. |
| `src/tools.ts` | Source of truth for built-in registry, simple mode, REPL mode, MCP merge, deny filtering, feature gates, prompt-cache ordering. |
| `src/constants/tools.ts` | Agent restrictions, async-agent allowlist, coordinator-mode allowlist. |
| `src/services/mcp/client.ts` | Dynamic MCP tool creation, names, schemas, annotations, `_meta`, structuredContent, auth, reconnect. |
| `src/services/mcp/mcpStringUtils.ts` | MCP naming contract: `mcp__server__tool`. |
| `src/utils/tasks.ts` and `src/utils/todo/types.ts` | Todo V1 and Task V2 state model. |
| Tool implementations under `src/tools/*` | Exact schemas, output contracts, flags, and side effects. |

## Tool Contract To Emulate

Claude Code Pure tools are not just name/schema/call. A DSCO adapter needs these fields and behaviors:

| Contract field | Adapter implication |
|---|---|
| `name`, `aliases` | Accept the current wire name and transcript-compatible legacy aliases. Built-ins win name conflicts over MCP. |
| `inputSchema`, `inputJSONSchema`, `outputSchema` | Built-ins use Zod-derived schemas; MCP and synthetic tools can provide raw JSON Schema. DSCO should preserve both. |
| `strict` | Strict tools reject unknown fields; shims should not silently ignore fields on strict surfaces. |
| `searchHint` | Used by `ToolSearch` and UX. DSCO should store this alongside description. |
| `shouldDefer`, `alwaysLoad` | Core to dynamic loading. Deferred tools are not always in the prompt; `ToolSearch` returns `tool_reference` blocks. |
| `isEnabled` | Runtime gate, not only build gate. Depends on env, provider, connected servers, feature flags, UI channel state. |
| `isConcurrencySafe`, `isReadOnly`, `isDestructive` | Drives parallel execution, auto-permission, UI grouping, and risk handling. |
| `requiresUserInteraction` | `AskUserQuestion` and `ExitPlanMode` can hang non-terminal flows unless gated. |
| `isMcp`, `isLsp`, `mcpInfo`, `isOpenWorld` | Needed for permission matching, display, tool filtering, and remote/dynamic source grouping. |
| `getPath`, `preparePermissionMatcher`, `backfillObservableInput` | File/path permissions normalize `~`, relative paths, and wildcard matching before user hooks. |
| `mapToolResultToToolResultBlockParam` | Result blocks are not always JSON strings. `ToolSearch` returns `tool_reference`; MCP returns content arrays; `StructuredOutput` returns structured payloads. |
| UI renderers | The chat surface depends on tool summaries, progress messages, rejected messages, and result truncation. |

## Registry And Loading Semantics

`src/tools.ts` is the source of truth.

Default built-in registry order:

1. `Agent`
2. `TaskOutput`
3. `Bash`
4. `Glob` and `Grep`, unless embedded search tools are available
5. `ExitPlanMode`
6. `Read`
7. `Edit`
8. `Write`
9. `NotebookEdit`
10. `WebFetch`
11. `TodoWrite`
12. `WebSearch`
13. `TaskStop`
14. `AskUserQuestion`
15. `Skill`
16. `EnterPlanMode`
17. Feature/env/runtime tools: `Config`, `Tungsten`, `SuggestBackgroundPR`, `WebBrowser`, Task V2 tools, overflow/context/terminal/LSP/worktree/team/verify/REPL/workflow/sleep/cron/remote trigger/monitor/brief/send user file/push/subscribe/PowerShell/snip/testing.
18. Special tools: `ListMcpResourcesTool`, `ReadMcpResourceTool`, and optionally `ToolSearch`.

Important registry rules:

- `CLAUDE_CODE_SIMPLE` exposes only `Bash`, `Read`, and `Edit`, except coordinator mode can also include `Agent`, `TaskStop`, and `SendMessage`.
- REPL mode hides primitive tools from direct use and exposes them through `REPL`.
- Normal `getTools()` excludes special tools `ListMcpResourcesTool`, `ReadMcpResourceTool`, and `StructuredOutput`; they are added conditionally elsewhere.
- `assembleToolPool()` sorts built-ins and MCP tools separately for prompt-cache stability, then dedupes by name with built-ins first.
- Deny rules filter tools before the model sees them. MCP server-prefix denies strip whole server tool groups.
- `ToolSearch` is optimistic: it can be available even when the actual deferral decision happens later.

## Core Tools

These are the core surfaces DSCO must speak fluently. "Current DSCO" reflects the compatibility shims currently present in `src/tools.c` plus recent session UI improvements.

| Claude tool | Aliases | Main schema | Behavior | Current DSCO status | Gap to close |
|---|---|---|---|---|---|
| `Agent` | `Task` | `description`, `prompt`, optional `subagent_type`, `model`, `run_in_background`, `name`, `team_name`, `mode`, `isolation`, `cwd` | Spawns synchronous, async, teammate, forked, worktree, or remote agents. Returns completed output or async metadata including `agentId` and `outputFile`. | DSCO has `Agent` and `Task` shims mapped to `spawn_agent`. | Need exact schema, `description` requirement, background state, output file, resume, team names, named agent addressing, worktree/remote isolation, and UI task rows. |
| `TaskOutput` | `AgentOutputTool`, `BashOutputTool` | `task_id`, `block`, `timeout` | Reads output from background bash, local agent, remote agent. Deprecated in favor of reading output file but still transcript-compatible. | DSCO has `agent_output`, `agent_wait`, but no Claude `TaskOutput` alias. | Add alias and unified background-task registry across shell and agents. |
| `TaskStop` | `KillShell` | `task_id` or deprecated `shell_id` | Stops running background task. | DSCO has `agent_kill` and background process tools. | Add exact `TaskStop` alias and task status validation. |
| `Bash` | none | `command`, optional `timeout` ms, `description`, `run_in_background`, `dangerouslyDisableSandbox` | Shell execution with permission classifier, sandbox selection, read-only command semantics, sed preview path, backgrounding, persisted output, images, git operation tracking. | DSCO has `Bash` shim with ms-to-seconds timeout and `cwd`. | Need `run_in_background`, task ids/output files, sandbox override semantics, sleep blocking, read/search collapse classification, persisted large output metadata. |
| `Read` | none | `file_path`, optional `offset`, `limit`; supports PDF/image/notebook details internally | Reads text with line numbers, images, PDFs, notebooks; tracks read-before-edit state; activates path-conditional skills. | DSCO has alias to `read_file` with `file_path`. | Need image/PDF/notebook parity, blocked device paths, unchanged stubs, read listeners, and read-before-edit state integration. |
| `Edit` | none | `file_path`, `old_string`, `new_string`, optional `replace_all` | Strict in-place replacement, requires prior `Read`, produces structured patch/git diff and user-modified flag. | DSCO has alias to `edit_file`. | Need strict read-before-edit/stale-file checks, diff schema, team memory secret guard, notebook redirect. |
| `Write` | none | `file_path`, `content` | Create/update with structured patch, original file content, git diff, secret guard. | DSCO has alias to `write_file`. | Need exact output schema and stale-file safeguards. |
| `Glob` | none | `pattern`, optional `path` | File glob; read-only/concurrent; returns duration, numFiles, filenames, truncated. | DSCO maps to `find_files`. | Need exact output shape, 100-result truncation, mtime/token-efficient ordering parity. |
| `Grep` | none | `pattern`, optional `path`, `glob`, `output_mode`, `-A`, `-B`, `-C`, `context`, `-n`, `-i`, `type`, `head_limit`, `offset`, `multiline` | Ripgrep wrapper. Modes: `content`, `files_with_matches`, `count`. Defaults to `files_with_matches` and head limit 250. | DSCO supports core `glob`, `output_mode`, `head_limit` shim. | Add full flag set, default limit/pagination, VCS/plugin exclusions, multiline behavior, mode-specific result text. |
| `NotebookEdit` | none | `notebook_path`, optional `cell_id`, `new_source`, optional `cell_type`, optional `edit_mode` | Edits `.ipynb` cells: replace, insert, delete. Requires notebook read first. | Missing. | Add notebook JSON parser/editor with cell IDs and read-before-edit state. |
| `WebFetch` | none | `url`, `prompt` | Fetch URL markdown then applies prompt; private/auth URL warning; domain permission rules. | DSCO maps to `web_extract`, currently `prompt` not fully equivalent. | Need prompt-applied extraction, preapproved domains, redirect handling, permission rule content `domain:<host>`. |
| `WebSearch` | none | `query`, optional `allowed_domains`, `blocked_domains` | Uses provider/server-side web search with up to 8 uses; disabled for unsupported providers/models. | DSCO maps to `parallel_search` with `num`. | Need domain filters, provider gating, result shape with search hits and text commentary. |
| `TodoWrite` | none | `todos: [{content,status,activeForm}]` | Todo V1 session checklist. Clears all-completed list. Can nudge verification agent. Disabled when Task V2 enabled. | DSCO has in-memory raw JSON shim. | Need status/activeForm validation, per-agent/session todo keys, UI status panel, verification nudge semantics. |
| `TaskCreate` | none | `subject`, `description`, optional `activeForm`, `metadata` | Task V2 create with file-backed task list and hooks. | Missing as Claude alias. | Add if DSCO wants Task V2 parity. |
| `TaskGet` | none | `taskId` | Reads a Task V2 item. | Missing. | Add read-only tool. |
| `TaskList` | none | `{}` | Lists Task V2 items when enabled. Note DSCO currently uses `TaskList` as TodoWrite companion. | Partial, but semantics mismatch. | Distinguish Todo V1 list vs Claude Task V2 `TaskList`. |
| `TaskUpdate` | none | `taskId`, optional `subject`, `description`, `activeForm`, `status`, `addBlocks`, `addBlockedBy`, `owner`, `metadata` | Mutates/completes/deletes Task V2 tasks; handles owners and verification nudge. | Missing. | Add file-backed task model and team ownership. |
| `EnterPlanMode` | none | `{}` | Switches permission context into plan mode; read-only and deferred. | DSCO has advisory state only. | Need real permission-mode transition and UI state. |
| `ExitPlanMode` | none | optional `allowedPrompts`; SDK normalized fields can include `plan`, `planFilePath` | Presents plan for approval, exits plan mode, can write/read plan file, can route teammate approval through mailbox. | DSCO has advisory exit with `plan` string. | Need approval workflow, file-backed plan, allowed prompts, teammate leader approval. |
| `AskUserQuestion` | none | `questions[1..4]`, each has `question`, `header`, `options[2..4]`, optional `multiSelect`; options can include `preview`; hidden `answers`, `annotations`, `metadata` | User-facing multiple-choice interaction; read-only/concurrent but requires user interaction. | DSCO has simple stdin question shim. | Need professional modal/list UI, multiple questions, option previews, annotations, non-TTY/channel gating. |
| `Skill` | none | `skill`, optional `args` | Invokes slash-command skills, local/plugin/MCP/remote discovered skills. Can execute inline or forked sub-agent. | Missing as Claude-compatible tool. | Add skill registry adapter or bridge to DSCO plugins/skills. |
| `Config` | none | `setting`, optional `value` | Gets/sets supported settings. Reads auto-allow; sets ask. Ant-only. | DSCO has config elsewhere, not Claude tool. | Add only if exposing ant-like config UX. |
| `Brief` | legacy SendUserMessage alias | `message`, optional `attachments`, `status` | Sends visible user message, optional file/image attachments; primary output channel in assistant/chat modes. | Missing. | Important for professional chat: attachable status/output messages. |

## Dynamic And Feature-Gated Tools

These tools do dynamic work or alter session/tool state. Some are present in this checkout; some are referenced by feature-gated imports but the implementation directory is not present in this source tree. DSCO should reserve names and gates even when implementation is intentionally absent.

| Tool | Gate/source | Dynamic behavior | DSCO adapter note |
|---|---|---|---|
| `ToolSearch` | `isToolSearchEnabledOptimistic()` | Searches deferred tools by name/description/searchHint. Supports `select:A,B`. Returns `tool_reference` blocks. Includes pending MCP servers when no match. | Implement exact tool-search loader. This is central for many MCP-heavy sessions. |
| `ListMcpResourcesTool` | special built-in | Lists resources from connected MCP servers, optional `server` filter. | DSCO currently lists MCP tools; add resource list parity. |
| `ReadMcpResourceTool` | special built-in | Reads MCP resource by `server` and `uri`, persists binary blobs to disk. | Add resource read and blob persistence. |
| Dynamic MCP tools | MCP `tools/list` | Tool names are `mcp__<normalized_server>__<normalized_tool>` unless SDK no-prefix is enabled. Uses MCP JSON Schema, annotations, `_meta`, `structuredContent`, open-world/read-only/destructive hints, reconnect retry, OAuth elicitation. | DSCO external registry exists but needs exact naming, annotations, deferral, auth pseudo-tool, structured content, and permission suggestions. |
| `mcp__server__authenticate` | needs-auth MCP state | Pseudo-tool starts OAuth flow and swaps in real server tools after callback. | Add if DSCO wants Claude config compatibility for OAuth MCP servers. |
| `StructuredOutput` | non-interactive synthetic output | Dynamic schema tool created from requested JSON Schema. Validates final output. | Required for SDK compatibility and structured final responses. |
| `REPL` | ant CLI REPL mode | Hides primitives (`Read`, `Write`, `Edit`, `Glob`, `Grep`, `Bash`, `NotebookEdit`, `Agent`) and exposes them inside VM context. | DSCO can skip initially unless adopting REPL bridge, but adapter must not expose conflicting primitives when emulating REPL sessions. |
| `LSP` | `ENABLE_LSP_TOOL` and connected LSP | Operations: `goToDefinition`, `findReferences`, `hover`, `documentSymbol`, `workspaceSymbol`, `goToImplementation`, `prepareCallHierarchy`, `incomingCalls`, `outgoingCalls`. | Strong code UI improvement target. Add as read-only code intelligence tool, not ad hoc grep. |
| `EnterWorktree` | worktree mode enabled | Creates isolated git worktree, switches cwd/session state, clears memory/prompt caches. | Important for multi-session safety. DSCO should make worktree sessions first-class. |
| `ExitWorktree` | worktree mode enabled | Keeps or removes worktree; removal requires `discard_changes` when dirty/unmerged. | Need fail-closed cleanup with visible session state. |
| `SendMessage` | team/coordinator contexts | Sends to teammate, broadcast `*`, structured shutdown/plan approval messages, UDS/bridge peers when enabled. | Map to DSCO IPC/session inbox. This is key for multiple sessions. |
| `TeamCreate` | agent swarms | Creates team file, task list, lead agent ID, colors, teammate context. | DSCO already has swarms; need Claude-compatible team metadata and UI. |
| `TeamDelete` | agent swarms | Cleans team directories/worktrees only when non-lead members inactive. | Add safe cleanup/leader workflow if adopting swarms. |
| `PowerShell` | PowerShell detection | Windows shell parity with read-only classifier, sandbox policy gate, backgrounding, persisted output. | Needed for cross-platform adapter; schema mirrors Bash with PowerShell semantics. |
| `CronCreate` | `AGENT_TRIGGERS` | Schedules prompt by 5-field cron, session-only or durable `.claude/scheduled_tasks.json`, recurring/one-shot. | DSCO has crontab/tooling but not Claude prompt scheduler. Add if assistant mode matters. |
| `CronDelete` | `AGENT_TRIGGERS` | Cancels scheduled prompt, teammate ownership rules. | Same. |
| `CronList` | `AGENT_TRIGGERS` | Lists active scheduled prompts, filtered by teammate. | Same. |
| `RemoteTrigger` | `AGENT_TRIGGERS_REMOTE` | Calls claude.ai trigger API: list/get/create/update/run. Requires OAuth and policy. | Probably later, but reserve schema/action names. |
| `Sleep` | `PROACTIVE` or `KAIROS` | Non-shell sleep/wait tool, concurrency-safe. This checkout includes prompt file but implementation import points to `SleepTool.js`. | Reserve name; avoid forcing model into `Bash(sleep ...)`. |
| `TestingPermissionTool` | `NODE_ENV=test` | Permission test surface. | Do not expose in production. |
| Referenced but absent here | feature-gated imports | `SuggestBackgroundPR`, `Tungsten`, `WebBrowser`, `OverflowTest`, `CtxInspect`, `TerminalCapture`, `ListPeers`, `Workflow`, `Monitor`, `SendUserFile`, `PushNotification`, `SubscribePR`, `Snip`, `VerifyPlanExecution`. | Treat as reserved optional names. Do not collide with future Claude sessions/transcripts. |

## Agent And Session Rules

Claude Code Pure treats agents as session/task objects, not just subprocesses.

Agent input fields:

- Required: `description`, `prompt`.
- Optional: `subagent_type`, `model` (`sonnet`, `opus`, `haiku`), `run_in_background`.
- Multi-agent extras: `name`, `team_name`, `mode`.
- Isolation extras: `isolation` (`worktree`, sometimes `remote`), `cwd`.

Agent output variants:

- Synchronous completion: `status: "completed"`, final agent result, prompt.
- Async launch: `status: "async_launched"`, `agentId`, `description`, `prompt`, `outputFile`, optional `canReadOutputFile`.
- Teammate spawn: internal `teammate_spawned` shape with agent ID, name, model, tmux session/window/pane, team, splitpane, plan-mode requirement.

DSCO UI should model these explicitly:

- A session navigator must show main sessions, child agents, background shells, remote sessions, and worktree sessions in one place.
- Background shell and agent tasks should share task IDs and output-file retrieval.
- Chat should show a compact tool activity row plus drill-down raw output, not raw JSON blobs by default.
- Named agents and teams need addressable inboxes (`SendMessage`) rather than only fire-and-forget spawn.
- Worktree-backed sessions need path/branch badges and a cleanup action that refuses to delete dirty work without explicit confirmation.

## Permissions And Safety

Claude compatibility is mostly permission compatibility.

Required rules:

- Support allow/deny/ask matching by tool name and, when applicable, `ruleContent`.
- File tools normalize path inputs before hooks and permission matching.
- MCP permission matching uses fully qualified names even when SDK no-prefix mode exposes a shorter model-facing name.
- `Bash` and `PowerShell` derive read-only status from command semantics, not from tool name.
- `WebFetch` permission rules are domain-based (`domain:<hostname>`).
- `Skill` supports exact and prefix rules such as `review:*`.
- `ExitPlanMode` can request semantic Bash permissions using `allowedPrompts`.
- `requiresUserInteraction()` tools must be disabled or rerouted when no terminal/user channel can answer.

## Current DSCO Compatibility Surface

DSCO currently has useful shims but they are thinner than Claude Code Pure:

| Surface | Present now | Why it is insufficient |
|---|---|---|
| `Read`, `Write`, `Edit`, `Bash`, `Glob`, `Grep`, `WebFetch`, `WebSearch` | Registered in `src/tools.c` with Claude-style names. | Schemas and results are simplified; some stateful constraints are missing. |
| `Agent` and `Task` | Both map to `spawn_agent`. | Claude Pure primary name in this checkout is `Agent`, legacy alias is `Task`; DSCO needs richer agent/task state and output files. |
| `TodoWrite` and `TaskList` | In-memory todo JSON. | Does not match per-agent todo keys, Task V2, UI, validation, or completion behavior. |
| `EnterPlanMode`, `ExitPlanMode` | Advisory boolean/string. | Does not mutate permission context or approval flow. |
| `AskUserQuestion` | Simple terminal prompt. | Does not support multi-question choice UI, previews, annotations, or channel gating. |
| MCP external registry | Exists via `tools_register_external()`. | Needs Claude MCP naming/annotations/resource/auth/tool-search semantics. |
| Session commands | `/sessions`, `/resume`, `/new`, `/rename`, `/slot list` recently improved. | Need deeper integration with background tasks, agent state, worktrees, and chat transcript UI. |

## Adapter Priorities

### P0 - Wire Compatibility

- Keep accepting current DSCO-native names and add exact Claude names/aliases.
- Add missing Claude aliases: `TaskOutput`, `TaskStop`, `KillShell`, `NotebookEdit`, `ToolSearch`, `ListMcpResourcesTool`, `ReadMcpResourceTool`, `StructuredOutput`, `Skill`, `Brief`.
- Normalize schemas around Claude input names (`file_path`, `notebook_path`, `task_id`, `taskId`, `allowed_domains`, `blocked_domains`).
- Preserve unknown dynamic MCP JSON Schema instead of reducing to string input.
- Return Claude-shaped tool results where clients/transcripts expect them.

### P1 - Session And Task State

- Introduce one DSCO task/session registry for main session, background shell, sub-agent, remote agent, worktree session, and team member.
- Store output files for background work and make `TaskOutput` a compatibility wrapper over the same store.
- Make `TaskStop` stop shell and agent tasks through one path.
- Persist todos/tasks by session/agent key instead of one global in-memory JSON.
- Show the task registry in the TUI as a professional session manager: status, owner, cwd/worktree, output bytes, elapsed time, controls.

### P2 - Dynamic Tool Loading

- Implement `ToolSearch` with deferred tool metadata, `select:` support, and a DSCO-equivalent of `tool_reference`.
- Extend MCP registry with normalized `mcp__server__tool` names, `searchHint`, `alwaysLoad`, `readOnlyHint`, `destructiveHint`, `openWorldHint`, and `structuredContent`.
- Add MCP resources and OAuth pseudo-tools.
- Respect deny rules before tools enter the prompt.

### P3 - UI Quality

- `AskUserQuestion` should render as a choice dialog with headers, option descriptions, optional previews, multi-select, and notes.
- Plan mode should have a visible banner/state and a real approve/reject transition.
- Tool results should render as concise summaries with expandable detail.
- Agent/team/worktree/session views should be navigable without relying on slash-command memory.
- Chat messages from `Brief`/SendUserMessage should support attachments and status type.

### P4 - Advanced Parity

- Add Notebook editing, LSP, PowerShell, worktree isolation, cron prompt scheduler, and skill execution.
- Add Task V2 (`TaskCreate`, `TaskGet`, `TaskUpdate`, `TaskList`) while keeping Todo V1 compatibility.
- Reserve feature-gated tool names that are referenced but absent in this checkout so resumed transcripts do not collide.

## Acceptance Tests For A Perfect Adapter

Minimum smoke suite:

1. `Read`, `Write`, `Edit`, `Bash`, `Glob`, `Grep`, `WebFetch`, `WebSearch` accept Claude-shaped inputs and return useful Claude-shaped outputs.
2. `Agent` and `Task` both work; `Agent` is canonical, `Task` is alias-compatible.
3. Background `Bash({run_in_background:true})` returns a task ID/output path; `TaskOutput` reads it; `TaskStop` stops it.
4. `TodoWrite` validates `content/status/activeForm`, persists by session/agent, and clears completed lists the Claude way.
5. `AskUserQuestion` handles multiple questions and options without corrupting the transcript.
6. `EnterPlanMode` and `ExitPlanMode` mutate visible session permission state.
7. `ToolSearch({query:"select:NotebookEdit"})` returns a reference/load result, not plain prose.
8. MCP tool `mcp__example__doThing` preserves JSON Schema and permission name.
9. `ListMcpResourcesTool` and `ReadMcpResourceTool` work for a test MCP server.
10. `StructuredOutput` validates a supplied JSON Schema and returns structured output.

## Appendix: Complete Tool Implementation File List

The list below is every file under `claude-code-pure/src/tools` in this checkout.

```text
src/tools/AgentTool/AgentTool.tsx
src/tools/AgentTool/UI.tsx
src/tools/AgentTool/agentColorManager.ts
src/tools/AgentTool/agentDisplay.ts
src/tools/AgentTool/agentMemory.ts
src/tools/AgentTool/agentMemorySnapshot.ts
src/tools/AgentTool/agentToolUtils.ts
src/tools/AgentTool/built-in/claudeCodeGuideAgent.ts
src/tools/AgentTool/built-in/exploreAgent.ts
src/tools/AgentTool/built-in/generalPurposeAgent.ts
src/tools/AgentTool/built-in/planAgent.ts
src/tools/AgentTool/built-in/statuslineSetup.ts
src/tools/AgentTool/built-in/verificationAgent.ts
src/tools/AgentTool/builtInAgents.ts
src/tools/AgentTool/constants.ts
src/tools/AgentTool/forkSubagent.ts
src/tools/AgentTool/loadAgentsDir.ts
src/tools/AgentTool/prompt.ts
src/tools/AgentTool/resumeAgent.ts
src/tools/AgentTool/runAgent.ts
src/tools/AskUserQuestionTool/AskUserQuestionTool.tsx
src/tools/AskUserQuestionTool/prompt.ts
src/tools/BashTool/BashTool.tsx
src/tools/BashTool/BashToolResultMessage.tsx
src/tools/BashTool/UI.tsx
src/tools/BashTool/bashCommandHelpers.ts
src/tools/BashTool/bashPermissions.ts
src/tools/BashTool/bashSecurity.ts
src/tools/BashTool/commandSemantics.ts
src/tools/BashTool/commentLabel.ts
src/tools/BashTool/destructiveCommandWarning.ts
src/tools/BashTool/modeValidation.ts
src/tools/BashTool/pathValidation.ts
src/tools/BashTool/prompt.ts
src/tools/BashTool/readOnlyValidation.ts
src/tools/BashTool/sedEditParser.ts
src/tools/BashTool/sedValidation.ts
src/tools/BashTool/shouldUseSandbox.ts
src/tools/BashTool/toolName.ts
src/tools/BashTool/utils.ts
src/tools/BriefTool/BriefTool.ts
src/tools/BriefTool/UI.tsx
src/tools/BriefTool/attachments.ts
src/tools/BriefTool/prompt.ts
src/tools/BriefTool/upload.ts
src/tools/ConfigTool/ConfigTool.ts
src/tools/ConfigTool/UI.tsx
src/tools/ConfigTool/constants.ts
src/tools/ConfigTool/prompt.ts
src/tools/ConfigTool/supportedSettings.ts
src/tools/EnterPlanModeTool/EnterPlanModeTool.ts
src/tools/EnterPlanModeTool/UI.tsx
src/tools/EnterPlanModeTool/constants.ts
src/tools/EnterPlanModeTool/prompt.ts
src/tools/EnterWorktreeTool/EnterWorktreeTool.ts
src/tools/EnterWorktreeTool/UI.tsx
src/tools/EnterWorktreeTool/constants.ts
src/tools/EnterWorktreeTool/prompt.ts
src/tools/ExitPlanModeTool/ExitPlanModeV2Tool.ts
src/tools/ExitPlanModeTool/UI.tsx
src/tools/ExitPlanModeTool/constants.ts
src/tools/ExitPlanModeTool/prompt.ts
src/tools/ExitWorktreeTool/ExitWorktreeTool.ts
src/tools/ExitWorktreeTool/UI.tsx
src/tools/ExitWorktreeTool/constants.ts
src/tools/ExitWorktreeTool/prompt.ts
src/tools/FileEditTool/FileEditTool.ts
src/tools/FileEditTool/UI.tsx
src/tools/FileEditTool/constants.ts
src/tools/FileEditTool/prompt.ts
src/tools/FileEditTool/types.ts
src/tools/FileEditTool/utils.ts
src/tools/FileReadTool/FileReadTool.ts
src/tools/FileReadTool/UI.tsx
src/tools/FileReadTool/imageProcessor.ts
src/tools/FileReadTool/limits.ts
src/tools/FileReadTool/prompt.ts
src/tools/FileWriteTool/FileWriteTool.ts
src/tools/FileWriteTool/UI.tsx
src/tools/FileWriteTool/prompt.ts
src/tools/GlobTool/GlobTool.ts
src/tools/GlobTool/UI.tsx
src/tools/GlobTool/prompt.ts
src/tools/GrepTool/GrepTool.ts
src/tools/GrepTool/UI.tsx
src/tools/GrepTool/prompt.ts
src/tools/LSPTool/LSPTool.ts
src/tools/LSPTool/UI.tsx
src/tools/LSPTool/formatters.ts
src/tools/LSPTool/prompt.ts
src/tools/LSPTool/schemas.ts
src/tools/LSPTool/symbolContext.ts
src/tools/ListMcpResourcesTool/ListMcpResourcesTool.ts
src/tools/ListMcpResourcesTool/UI.tsx
src/tools/ListMcpResourcesTool/prompt.ts
src/tools/MCPTool/MCPTool.ts
src/tools/MCPTool/UI.tsx
src/tools/MCPTool/classifyForCollapse.ts
src/tools/MCPTool/prompt.ts
src/tools/McpAuthTool/McpAuthTool.ts
src/tools/NotebookEditTool/NotebookEditTool.ts
src/tools/NotebookEditTool/UI.tsx
src/tools/NotebookEditTool/constants.ts
src/tools/NotebookEditTool/prompt.ts
src/tools/PowerShellTool/PowerShellTool.tsx
src/tools/PowerShellTool/UI.tsx
src/tools/PowerShellTool/clmTypes.ts
src/tools/PowerShellTool/commandSemantics.ts
src/tools/PowerShellTool/commonParameters.ts
src/tools/PowerShellTool/destructiveCommandWarning.ts
src/tools/PowerShellTool/gitSafety.ts
src/tools/PowerShellTool/modeValidation.ts
src/tools/PowerShellTool/pathValidation.ts
src/tools/PowerShellTool/powershellPermissions.ts
src/tools/PowerShellTool/powershellSecurity.ts
src/tools/PowerShellTool/prompt.ts
src/tools/PowerShellTool/readOnlyValidation.ts
src/tools/PowerShellTool/toolName.ts
src/tools/REPLTool/constants.ts
src/tools/REPLTool/primitiveTools.ts
src/tools/ReadMcpResourceTool/ReadMcpResourceTool.ts
src/tools/ReadMcpResourceTool/UI.tsx
src/tools/ReadMcpResourceTool/prompt.ts
src/tools/RemoteTriggerTool/RemoteTriggerTool.ts
src/tools/RemoteTriggerTool/UI.tsx
src/tools/RemoteTriggerTool/prompt.ts
src/tools/ScheduleCronTool/CronCreateTool.ts
src/tools/ScheduleCronTool/CronDeleteTool.ts
src/tools/ScheduleCronTool/CronListTool.ts
src/tools/ScheduleCronTool/UI.tsx
src/tools/ScheduleCronTool/prompt.ts
src/tools/SendMessageTool/SendMessageTool.ts
src/tools/SendMessageTool/UI.tsx
src/tools/SendMessageTool/constants.ts
src/tools/SendMessageTool/prompt.ts
src/tools/SkillTool/SkillTool.ts
src/tools/SkillTool/UI.tsx
src/tools/SkillTool/constants.ts
src/tools/SkillTool/prompt.ts
src/tools/SleepTool/prompt.ts
src/tools/SyntheticOutputTool/SyntheticOutputTool.ts
src/tools/TaskCreateTool/TaskCreateTool.ts
src/tools/TaskCreateTool/constants.ts
src/tools/TaskCreateTool/prompt.ts
src/tools/TaskGetTool/TaskGetTool.ts
src/tools/TaskGetTool/constants.ts
src/tools/TaskGetTool/prompt.ts
src/tools/TaskListTool/TaskListTool.ts
src/tools/TaskListTool/constants.ts
src/tools/TaskListTool/prompt.ts
src/tools/TaskOutputTool/TaskOutputTool.tsx
src/tools/TaskOutputTool/constants.ts
src/tools/TaskStopTool/TaskStopTool.ts
src/tools/TaskStopTool/UI.tsx
src/tools/TaskStopTool/prompt.ts
src/tools/TaskUpdateTool/TaskUpdateTool.ts
src/tools/TaskUpdateTool/constants.ts
src/tools/TaskUpdateTool/prompt.ts
src/tools/TeamCreateTool/TeamCreateTool.ts
src/tools/TeamCreateTool/UI.tsx
src/tools/TeamCreateTool/constants.ts
src/tools/TeamCreateTool/prompt.ts
src/tools/TeamDeleteTool/TeamDeleteTool.ts
src/tools/TeamDeleteTool/UI.tsx
src/tools/TeamDeleteTool/constants.ts
src/tools/TeamDeleteTool/prompt.ts
src/tools/TodoWriteTool/TodoWriteTool.ts
src/tools/TodoWriteTool/constants.ts
src/tools/TodoWriteTool/prompt.ts
src/tools/ToolSearchTool/ToolSearchTool.ts
src/tools/ToolSearchTool/constants.ts
src/tools/ToolSearchTool/prompt.ts
src/tools/WebFetchTool/UI.tsx
src/tools/WebFetchTool/WebFetchTool.ts
src/tools/WebFetchTool/preapproved.ts
src/tools/WebFetchTool/prompt.ts
src/tools/WebFetchTool/utils.ts
src/tools/WebSearchTool/UI.tsx
src/tools/WebSearchTool/WebSearchTool.ts
src/tools/WebSearchTool/prompt.ts
src/tools/shared/gitOperationTracking.ts
src/tools/shared/spawnMultiAgent.ts
src/tools/testing/TestingPermissionTool.tsx
src/tools/utils.ts
```
