# @distributed.systems/dsco-sdk

Typed, zero-dependency client SDK for the [dsco runtime](https://github.com/arthurcolle/dsco).

Two planes, one binary:

| Plane | Transport | Use it for |
|---|---|---|
| **MCP** (`McpClient`) | `dsco mcp serve` — NDJSON JSON-RPC 2.0 over stdio | calling governed tools: fs, bash, python, AST/code intelligence, swarms, markets |
| **ACP** (`AcpClient`) | `dsco acp serve` — same framing | agentic sessions: initialize → session → streamed `session/update` → prompt/stop |

## Install

```sh
npm install @distributed.systems/dsco @distributed.systems/dsco-sdk
```

The `dsco` package downloads the platform binary at install time (arm64/x64 darwin+linux tarballs from GitHub releases, sha256-verified). The SDK resolves it automatically; override with `DSCO_BIN=/path/to/dsco`.

## Tool plane (MCP)

```js
import { McpClient } from '@distributed.systems/dsco-sdk';

const dsco = new McpClient();            // toolsets: 'core' default; also 'ast','swarm','market','crypto','all'
await dsco.initialize();

const tools = await dsco.listTools();
const out = await dsco.callTool('grep_files', { pattern: 'TODO', path: 'src' });

// Control-plane tools are toolset-scoped AND capability-gated:
//   killswitch status (read-only)  -> needs --toolsets all
//   killswitch trigger (mutating)  -> hard denial citing DSCO_ALLOW_CONTROL, non-escalatable
await dsco.close();
```

Errors surface as `TransportError` with the exact RPC code from the gate — SDK code can branch on governance denials programmatically instead of parsing stderr.

## Session plane (ACP)

```js
import { AcpClient } from '@distributed.systems/dsco-sdk';

const agent = new AcpClient({
  onUpdate: (u) => process.stdout.write(u.update?.content ?? ''), // stream
});
await agent.initialize();
const { sessionId } = await agent.createSession(process.cwd());
const stop = await agent.prompt(sessionId, 'summarize this repo');
await agent.close();
```

## Design notes

- **NDJSON, not LSP framing** — dsco's C servers emit one JSON object per line on stdio; the SDK matches that exactly and tolerates stray non-JSON chatter without corrupting request correlation.
- **Response correlation survives timeouts** — a timed-out call leaves late responses unclaimed but keeps subsequent calls correct.
- **Governed by default** — the SDK never weakens tier or grants; it exposes them so host applications make capability choices explicitly.
- Zero dependencies; hand-written `.d.ts`; works on Node ≥18.

## Tests

```sh
npm run test:unit          # framing/correlation vs fake server (no binary needed)
npm run test:e2e           # live round-trips against real ./dsco
DSCO_LIVE_SESSIONS=1 npm run test:e2e   # + ACP session prompts (needs provider config)
```
