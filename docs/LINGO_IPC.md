# Lingo native event IPC

Lingo can expose its observable execution through an ordered durable journal and
a live Unix socket. The event channel carries data out of the native runtime;
it does not install Lua callbacks or grant additional tool authority.

This document describes the implementation contract. Build and live verification
results belong in the corresponding run report; this document alone is not a
claim that a particular binary or provider lane passed those checks.

## Command interface

```text
dsco lingo run FILE [ARGS_JSON] --events-fd FD --events-db NEW_ABSOLUTE_PATH
dsco lingo eval SOURCE [ARGS_JSON] --events-fd FD --events-db NEW_ABSOLUTE_PATH
dsco lingo replay EVENTS_DB --events-fd FD [--after SEQUENCE]
```

The host supplies an already connected **nonblocking** `AF_UNIX`, `SOCK_STREAM`
descriptor numbered at least 3. Set `O_NONBLOCK` before launching DSCO; the native
boundary validates it and does not change the caller's status flags. Both event
flags are required for a new run. The database path must
be absolute and new; creation is exclusive, with mode `0600`. Native exec workers
join the same outbox using inherited `DSCO_EVENT_STREAM_DB` and root-process
identity. The observer socket is not passed to exec workers.

Normal stdout remains one final JSON result. Human terminal output and diagnostics
remain on stderr. Read the socket while the process runs, and drain or redirect
stdout/stderr so those separate channels cannot block it.

## Wire records and retention

The socket carries UTF-8 NDJSON. Stream reads can split or combine records: buffer
until a newline, then parse one JSON value. A record has this shape:

```json
{
  "seq": 42,
  "record": {
    "schema": "dsco.event.v1",
    "pid": 1201,
    "ppid": 1200,
    "rootpid": 1200,
    "monotonic_ns": "9812330045000",
    "source": "swarm",
    "event": "worker.output",
    "payload": {}
  }
}
```

`seq` is the shared SQLite commit sequence. It supplies the replay cursor and
deduplication key within this database. It is distinct from any Chronicle,
Lingo-world or producer sequence inside `payload`. `monotonic_ns` is an exact
decimal string; it describes the producer clock, not provider inference time.
Consumers must preserve integer cursor precision rather than round large
sequences through a JavaScript `Number`.

The outbox uses SQLite WAL with `synchronous=FULL`. Capture has no sampling or
drop policy. Each committed record is bounded to 32 MiB; capture failure is
sticky and surfaced, rather than silently truncating an event. A slow or
disconnected socket consumer leaves a retained file backlog. Live delivery is
a projection of the journal, not its source of truth.

New tool dispatch checks journal health and commits its start record before
executing the leaf operation. Capture failure interrupts active execution and
prevents subsequent dispatch. It cannot undo effects already performed.

The final Lingo JSON includes a `stream` receipt with schema
`dsco.event_stream.receipt.v1`, `db_path`, `last_seq`, `sent_seq`, `pending`,
`capture_error`, `transport_error`, `replay` and `sent_meaning`. The two error
fields are booleans. Close gives transport a bounded
two-second drain window. `sent_seq` means a complete record was written to the
socket; it is **not** acknowledgement by the consumer. Compare your own last
fully parsed sequence with `last_seq`. Close succeeds only with no capture or
transport error and no pending rows; an incomplete live delivery makes the CLI
return nonzero while preserving the journal for replay.

Replay reads records strictly after `--after`, up to the committed high-water
mark observed when replay starts. It does not mutate the database. A two-second
no-progress deadline can leave more work to replay. Reconnect from the last
record the consumer actually retained, not a guessed timestamp or a partially
received line.

The CLI cleans up owned swarm workers before closing Chronicle and the event
stream. A close-time watermark alone does not prove successful worker execution:
check worker terminal records and the normal final result as well. In particular,
`worker.reap_pending` reports that teardown did not establish an exit status.

## Observable coverage

The capture hooks expose these separate observations:

| Source | Meaning |
| --- | --- |
| `lingo` | World and object creation, reads, dependencies, calculations, cache hits, invalidation, scenario changes and tool-call attempts/results. |
| `execution` | Proposed tool input, actual leaf input before dispatch, returned result and execution identity. |
| `governance` | Emitted gate-stage assessments, including whether a decision would deny and whether it was enforced. |
| `journal` | Existing full serialized native journal envelopes, including emitted semantic callback and execution events. |
| `chronicle` | Existing full serialized Chronicle event envelopes. Inner span, actor and source sequence remain intact. |
| `llm` | Normalized `llm.response.delta` with inline `kind`, `text`, `trace_id` and `span_id`, independent of legacy Chronicle capture mode. |
| `provider` | Provider event/SSE payloads at registered parser boundaries, plus tool-argument deltas. These are not a capture of arbitrary network traffic. |
| `swarm` | Worker start/exit and every chunk read from the worker pipes, before UI throttling or the bounded collected-output buffer. |

Swarm `worker.output` payloads contain `id`, `pid`, `group_id`, `provider`,
`model`, `status`, `exit_code`, `stream`, `encoding`, `byte_length` and
`data_base64`. `encoding` is `base64`; native workers use `stream="merged"`
because stdout and stderr share a pipe. `exit_code` is null until an observed
exit. Decoding yields the exact captured bytes, potentially including terminal
escapes, diagnostics and model output. Render them as data, not executable
terminal control sequences or HTML.

Provider events, normalized LLM deltas, Chronicle blob references and worker
output can describe overlapping activity. Do not concatenate all four into one
answer or count them as separate model calls. Likewise, an event's generic
`status="ok"` is not proof of a capability grant; use the explicit execution and
governance records and their correlation identifiers.

This exposes instrumented runtime mechanisms and decisions. It does not expose
unreported model internals, hidden reasoning, or causal relationships merely
because two events are adjacent. The event sources and event names define the
coverage being claimed.

## Minimal Python receiver

Save the following as `receive.py`, then supply the absolute or relative path
of the Lingo program to execute. That program retains responsibility for its
own tool, worker, time and cost limits. This example retains the database and
final result in a fresh temporary directory and uses temporary files for both
process output channels to avoid pipe deadlocks.

```python
import json
import pathlib
import socket
import subprocess
import sys
import tempfile

program = pathlib.Path(sys.argv[1]).resolve()
run_dir = pathlib.Path(tempfile.mkdtemp(prefix="lingo-events-"))
database = run_dir / "events.sqlite"
receiver, sender = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
receiver.settimeout(0.1)
sender.setblocking(False)  # Required on the descriptor passed to DSCO.
pending = bytearray()
last_received = 0

with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
    process = subprocess.Popen(
        ["dsco", "lingo", "run", str(program),
         "--events-fd", str(sender.fileno()), "--events-db", str(database)],
        stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
        pass_fds=(sender.fileno(),),
    )
    sender.close()
    while True:
        try:
            chunk = receiver.recv(65536)
        except socket.timeout:
            if process.poll() is not None:
                break
            continue
        if not chunk:
            break
        pending.extend(chunk)
        while b"\n" in pending:
            line, _, rest = pending.partition(b"\n")
            pending = bytearray(rest)
            row = json.loads(line)
            if row["seq"] != last_received + 1:
                raise RuntimeError("Unexpected cursor; replay from retained cursor")
            last_received = row["seq"]
            # Update a view here. Never print decoded worker bytes blindly.
        if len(pending) > 32 * 1024 * 1024 + 1024:
            raise RuntimeError("Event framing limit exceeded")
    process.wait()
    receiver.close()
    stdout.seek(0)
    final = json.load(stdout)
    stderr.seek(0)
    (run_dir / "stderr.log").write_bytes(stderr.read())

(run_dir / "final.json").write_text(json.dumps(final, indent=2) + "\n")
print(json.dumps({"directory": str(run_dir), "received_through": last_received,
                  "exit_code": process.returncode, "result": final}, indent=2))
stream = final["stream"]
if pending or last_received != stream["last_seq"]:
    raise SystemExit("Delivery incomplete: replay the retained database")
if process.returncode:
    raise SystemExit("Execution/capture failed; inspect the retained result")
```

For replay, create another socket pair and use
`dsco lingo replay DATABASE --events-fd FD --after LAST_RETAINED_SEQUENCE` with
the same framing loop. A partially received trailing line is not retained as an
event; replay it in full.

Measure producer-to-consumer delivery delay separately from provider time to
first output. Worker startup, authentication, model processing, terminal
formatting and socket delivery are different intervals. Raw stdout arrival
alone is not provider TTFT.

## WASM boundary

The current [WASM core](WASM.md) contains model metadata, simple route explanation,
transcript state and a few pure tools. Its build does not include Lingo or native
swarm execution. A browser can first consume these JSON events and replay pages
through an explicitly authorized native bridge. The native host retains process
ownership, credentials, capability checks and effect receipts.

A later pure WASM observer can share a bounded event projection with the native
UI without embedding a Lua interpreter. A browser-local Lingo evaluator is a
separate step: stock LuaJIT's [supported architectures](https://luajit.org/status.html)
do not include WASM, even though DSCO currently disables its JIT compiler.
The [C Lua implementation](https://www.lua.org/manual/5.1/manual.html) offers a
candidate interpreter boundary, with compatibility work still required.

Current Lingo snapshots and ValueAddresses pin compiler/runtime identities and
function hashes derived from LuaJIT's [deterministic bytecode dump](https://luajit.org/extensions.html).
A different engine must identify itself honestly and reject incompatible
executable snapshots; importing observations is not equivalent to executing
the same definitions. Browser-hosted effects also need an explicit asynchronous
boundary; [Emscripten Asyncify](https://emscripten.org/docs/porting/asyncify.html)
is one available mechanism, not something supplied by the current build.

Provider HTTP tracing also retains each actual attempt, retry decision, and full response-body bytes before SSE parsing (base64, with byte offsets). Normalized deltas and parsed protocol events are separate projections of these same bytes. The endpoint label excludes credentials, query strings, and headers.

One-shot streaming rejects `DSCO_SWARM_PRESERVE_CHILDREN`: its stream must close after owned workers are retired. An unobserved worker exit records `worker.reap_pending` and marks capture incomplete.
