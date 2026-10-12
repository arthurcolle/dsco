# Native local workbench

`dsco --native-workbench` is a separate, **no-model** UI entrypoint. It returns into the workbench before agent/provider/network-server initialization. It does not use a conversation as its event loop.

## Working vertical slice

- Editable native task windows with **Run / Input / Result / Clear** controls.
- **Ctrl-R** runs the current input; **Esc** returns to the local command line.
- Three compiled pure executors: JSON inspection, SHA-256, text byte/newline/tab statistics.
- Up to eight immutable factory descriptors, including three defaults. New descriptors are aliases over approved compiled executors, not uploaded code or arbitrary tools.
- Four bounded background C workers; up to twelve pending input snapshots. Repeated requests for unchanged input coalesce. Updated requests replace older queued work for that application.
- Standalone workbench applications own their input, compiled executor descriptor and result independently of views. Results are fenced by workspace epoch, application ID, request sequence and input generation. Ordinary conversation task windows retain their original session-local semantics.
- Run clicks and shortcuts do not enter the Continue/Revise/Retry conversation-action queue.

The workbench is not a general autonomous-agent desktop. It does not yet provide arbitrary field schemas, remote execution, an event-replay log, multiple simultaneous views per application, or model-generated native plugins. None of those privileges were silently enabled.

## Use

```sh
dsco --native-workbench
```

Enter `/quit` in the local command line to exit. Do not pass model/provider flags: this mode has no model backend. In the standalone workbench, closing a task view retains its input and result; queued/running pure local work can complete while detached. Hiding views also preserves work. Existing ordinary buffer/draft behavior in the conversation UI is unchanged. Ctrl-S does not save task fields to canonical buffers, and Ctrl-B canonical-buffer creation remains disabled in this mode. Existing user-triggered native clipboard operations remain distinct from task execution.

The standard `--native` tool surface can also create factories/task windows on a newly launched build. These are local operations after bootstrap, not model tool calls for every click.

## Durable applications (v1)

The local `dsco-workbench` launcher can point to an independently installed build at `~/.local/lib/dsco-workbench/dsco`, leaving the main `~/.local/bin/dsco` and already-running sessions unchanged. A newly built checkout also exposes the feature through `./dsco --native-workbench`.

Default state: `~/.dsco/native-workbench.json`. Set `DSCO_NATIVE_WORKBENCH_STATE` to an absolute path in an existing owner-controlled directory for a separate workspace, or to `:memory:` to disable disk persistence. The parent must exist. Files and locks are private (0600); this is local plaintext storage, not encryption or a secret store. Explicit `DSCO_ALLOW_READ=0` or `DSCO_ALLOW_WRITE=0` refuses persistent startup.

Local commands (also accepted on the existing same-user socket):

```json
{"action":"apps"}
{"action":"reopen","app_id":1}
{"action":"checkpoint"}
```

`apps` returns full application state; window replies expose `app_id` separately from their session-local `id`. `reopen` attaches a new view or focuses the existing one; it never duplicates or automatically reruns a job. `checkpoint` reports `durable:true` only after successful file and directory synchronization. In memory-only mode it reports `durable:false`.

State uses a versioned strict JSON snapshot, exclusive process lock and atomic same-directory replacement. Checkpoints are coalesced on the control loop (normally checked every 100 ms) and at normal shutdown. This is **not** a per-keystroke transaction guarantee or a hard save deadline: blocked I/O or a client request can delay the loop. A crash restores the last successful checkpoint. Saved running/queued jobs become **interrupted**, requiring explicit Ctrl-R. Completed results retain their input hash. Damaged/unknown snapshots fail startup without being overwritten; no automatic salvage or destructive reset occurs.

Bounds: 32 application records independent of 12 view slots, 8191-byte UTF-8 inputs and 511-byte outputs. There is no delete/archive command yet; use a distinct state path for another workspace after reaching the cap. Editor undo/selection history is not serialized. An application's immutable custom factory descriptor survives restart and can rerun, but unused factory aliases are not persisted as a global registry. No model, remote execution, generic shell action, automatic retry or new agent-tool authority is introduced.

Verification: `make test_native_apps` covers state/view lifetime, detached completion, stale generations, restore, immutable alias binding, malformed snapshots, permissions, locking and failed-write safety. `python3 tests/test_native_app_pty.py --binary ./dsco --output build/native-app-pty.json` drives actual renderer/input in owned PTYs, no desktop windows. For an installed launcher, add `--launcher --binary ~/.local/bin/dsco-workbench`. These checks establish local behavior, not physical display acknowledgement or general generative-UI completeness.

## Dynamic factory examples

The local command line accepts JSON:

```json
{"action":"define_factory","factory":"payload-check","title":"Payload check","executor":"json"}
{"action":"factory","factory":"payload-check","text":"{\"ready\":true}"}
{"action":"factory","factory":"sha256","text":"abc"}
{"action":"run_task","id":1}
```

A factory name is bounded ASCII letters/digits/underscore/hyphen. Definitions cannot overwrite existing names. `bash`, `curl`, model providers and arbitrary callbacks are not valid executors. JSON inspection is syntactic inspection, not arbitrary JSON-Schema validation; the `values` count is the parser's value-node count, including object keys.

## Local control without model turns

Each process exposes `/tmp/dsco-workbench-UID-PID.sock`, mode **0600**, with same-user peer checking on macOS. One bounded JSON request per connection, newline terminated; JSON response followed by close. This is Unix IPC, not a TCP/HTTP endpoint.

Allowed operations: factory definition/creation, run, input/title update, focus, move, resize, tile, cascade, close, hide/show, list and read-only `stats`. Requests cannot create general agent workflows, bind filesystem buffers, select a model, execute a shell or grant capabilities.

`stats` reports queue outcomes, cache counts and submitted-frame timing. The same-user socket can read task text; **it is not a cross-agent or multi-tenant security boundary**. Do not use it as a secret store. Normal shutdown drains bounded native jobs and removes the socket; SIGKILL cannot guarantee cleanup.

## Rendering direction: retain content, move surfaces

The compositor now retains up to four window rasters with a 16-MiB conservative charged-byte budget. Translation reuses the raster instead of rerunning text/geometry drawing. Input, selection, results, size, focus, DPI and theme changes invalidate the key. Cached and uncached output are checked byte-for-byte across three backing scales.

- `DSCO_NATIVE_WINDOW_CACHE=0` disables retention for comparison/rollback.
- This is a cache budget, not a cap on the existing canvas pool or total process RSS.
- Time-dependent decorations must remain outside retained static content or enter the cache key. The change does not promise arbitrary animations at 60 fps.
- Worker execution runs outside the input/render lock. Completions only update matching window state and notify the compositor; they never request a language-model turn.

## Verified evidence

`.workspace/native-local-workbench-v1/` retains source snapshots, test output and owned-PTY measurements.

- 164 task/factory checks, including duplicate requests, stale edit/reset completions, hidden views, closed views, malformed JSON and forbidden executor names.
- The task tests also passed under ASan + UBSan.
- 2,160 existing window-model checks and 672 tool/clipboard/buffer-adapter checks passed.
- 35 raster-cache checks, including cached/uncached pixel parity at scales 1, 1.5 and 2, translation, input/result/selection changes and budget rejection.
- Real owned-PTY Ctrl-R input, Unix RPC, typed factory creation, native execution, invalid-input recovery, hidden-view completion and clean shutdown passed.
- A 180-command geometry-motion test at a target 60 updates/s, with periodic input/result changes, reported frame p95 **11.004 ms without cache / 8.750 ms with cache**. Cache run: 255 hits, 4 misses, 12 local task completions, zero failed frames, about **36.5 MiB observed process RSS**. Single paired observations, not statistical guarantees.

Frame timing is local rendering/submission, not physical display acknowledgement. The old conversation/card trace is a different workload and is not used to claim a comparable speedup. No extra model workers or paid provider experiments were used for this implementation's tests.
