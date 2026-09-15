# Context branches

Native, local-first versioned prompt **content**, plus a private browser/API adapter.
Proposed hosted origin: **https://context.distributed.systems**. No DNS, TLS,
public deployment, paid add-on, or external content upload is performed by setup.

## Data model

- A document starts with one immutable main content chunk.
- Content is stored/pinned through `ctx_put` in DSCO's existing context fabric:
  `ck:blob:<SHA256>`. Embeddings and provider calls are disabled for these writes.
- A revision hashes canonical metadata containing document, parent, optional
  merge parent, ctxkey, author and message. Timestamps live outside that hash.
- Named branches are SQLite head references, not copies of the original text.
- Agent writes are autonomous on `agent.*` branches through the API. Main only
  changes through an explicit owner promotion. Source and destination heads
  must both match the revisions supplied by the caller.
- History and original chunks remain retrievable. Rollback means forking a
  historical revision, then explicitly promoting it, preserving the lineage.
- Audit events are inserted in the same SQLite transaction as branch updates.
  Hash checks detect accidental content/metadata corruption; hashes do not
  establish authorship or protect against the OS principal rewriting the DB.

This does **not** rewrite system/developer prompts, AGENTS.md, identity,
capability grants, evaluators, or currently running model sessions. To use a
branch as task input, read its captured revision through the API or CLI and pass
its `content` as task data via the normal governed agent path. Returned content
is never automatically treated as higher-priority instructions. This service
stores revisions; it does not autonomously launch models or tools.

## Native CLI

`dsco prompt-branch` reads one bounded JSON object from stdin and emits one JSON
response. Exit 0 means success; exit 1 returns `ok:false`, `code`, and `error`.
The early CLI path does not load saved provider credentials, attach MCP, or
start background model refresh. Local CLI authorization is the OS principal;
agent/owner separation is enforced by the authenticated HTTP adapter, not by a
caller-supplied `author` field. Agents invoking the CLI use their normal governed
shell tool; there is no new bypass tool or altered capability gate.

```json
{"action":"init","document":"main-content","content":"Your main content"}
{"action":"list","document":"main-content"}
{"action":"get","document":"main-content","branch":"main"}
{"action":"fork","document":"main-content","branch":"agent.experiment","from":"main","expected":"<main revision>"}
{"action":"commit","document":"main-content","branch":"agent.experiment","expected":"<branch revision>","content":"Candidate content","message":"Reason for edit"}
{"action":"history","document":"main-content","branch":"agent.experiment"}
{"action":"promote","document":"main-content","from":"agent.experiment","expected":"<main revision>","source_expected":"<branch revision>","message":"Owner-reviewed promotion"}
```

`get` accepts `revision` instead of resolving a head. `fork` accepts an optional
historical `revision` from the same document, while `expected` still protects the
source branch from races. History returns up to 100 first-parent revisions and
marks truncation; merge parents remain explicit revision references.

Defaults: `~/.dsco/context/prompt-branches.db` and the existing context-fabric DB.
Override with `DSCO_PROMPT_BRANCH_DB` and `DSCO_CONTEXT_DB`, using separate files
with existing private parent directories. HTTP uses its own explicitly selected
data directory, never the operator's default context store or credentials.

## Private workbench/API

```sh
python3 web/prompt_branches/run_local.py --binary /absolute/path/to/dsco \
  --data-dir /absolute/private/path --port 8791
```

The launcher creates separate random owner/agent bearer tokens in a mode-0600
`access.json` inside the private data directory, without printing them. Load the
owner token into the browser password field at `http://127.0.0.1:8791`. Provision
only the agent token to trusted native workers. The page stores neither tokens
nor prompts in localStorage/cookies; drafts and tokens are lost on reload.
Do not put either token into URLs, process arguments, logs, commits or prompts.

API: `POST /api`, `Content-Type: application/json`,
`Authorization: Bearer <token>`, with the same JSON action bodies. Agent token
can read documents and fork/commit `agent.*` branches; owner token can also
initialize and promote. The server stamps authenticated role attribution.

The adapter binds loopback only, rejects foreign Origin/Host headers, serves
only three fixed assets, enforces body/time bounds, and invokes only the native
`prompt-branch` entrypoint with a minimal environment and content on stdin. No
shell interpolation, arbitrary command endpoint, third-party JS or provider
execution. HTTP 409 means stale state: reread before deciding whether to retry.
HTTP 504 means outcome unknown: inspect the head before retrying a write.

## Hosting at context.distributed.systems

`Caddyfile.example` terminates HTTPS and proxies to the loopback adapter. Start
with `--public-origin https://context.distributed.systems` when using that proxy.
A Linux host requires a Linux build of DSCO; do not deploy the macOS binary.

Before public activation, approve the destination host and DNS change, provision
private storage and tokens on that host, apply an authenticated edge/access
policy and rate limits, and verify origin/TLS/private data access end-to-end.
This prototype is **single-principal**, not multi-tenant. Both tokens can read
all documents; agent tokens share the agent.* namespace. There is no SSO,
per-agent document ACL, token rotation endpoint, or public sharing endpoint.

Stop the service before copying **both complete databases** for a backup; include
WAL/SHM state or use SQLite's backup API. Context storage and the branch index use
separate databases: a failed write can leave an unreferenced pinned blob, but
must not publish a missing content pointer. There is no garbage collector yet.
The fabric's existing WAL durability policy is unchanged; process-restart tests
are not a power-loss certification.

## Verification

`python3 tests/test_prompt_branches.py --binary /absolute/path/to/dsco`
uses temporary real stores and a real loopback HTTP server. Tests cover immutable
roots, native ctxkeys, independent forks, stale writes, concurrent compare-and-
swap, explicit promotion, rollback, history, malformed inputs, integrity,
private files, HTTP authentication/roles/origin/bounds and static UI delivery.
`node --check web/prompt_branches/app.js` validates JS syntax.
`node tests/test_prompt_branches_browser.mjs /absolute/path/to/dsco` exercises the
real UI in headless Chrome (Node >=22; `CHROME_BIN` overrides the macOS default).
It checks init, fork, edits, commit, stale-draft preservation, promotion, history,
HTML non-execution, and absence of browser localStorage. The test terminates its
Chrome and server processes and removes its private temporary state. This is
functional browser verification, not a visual/layout audit.
No test calls a provider, deploys remotely, or changes runtime governance.
