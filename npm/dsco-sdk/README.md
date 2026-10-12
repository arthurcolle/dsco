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

## Autonomous organizations

`Org` composes the tool plane into a self-directing group of agents operating under
a shared treasury, a budget policy, and a goal loop. This is the code shape of an
autonomous organization: members earn and spend against a ledger, every action is
authorized against capability + budget policy before it touches the runtime, and the
loop senses → decides → acts → verifies → records.

```js
import { Org, Policy, Treasury } from '@distributed.systems/dsco-sdk';

const org = new Org({
  name: 'field-ops',
  treasury: new Treasury({ path: './treasury.json' }),   // persistent ledger, 1 unit = 1¢
  policy: new Policy({ toolsets: 'core',                  // governed tool surface
                       maxCentsPerTask: 500,              // per-action budget cap
                       maxCentsTotal: 10000,              // org-wide budget cap
                       denyTools: ['delete_file'] }),     // capability denylist
});
await org.init();

const scout   = org.hire('scout', 'reconnaissance', 100); // seed 100¢
const builder = org.hire('builder', 'construction', 50);

// Every action is governed: authorized against policy, then charged to the treasury.
await scout.act('read_file', { path: 'README.md' }, 1);
await builder.act('delete_file', { path: '/etc/passwd' }, 1); // → denied by policy

org.reward(scout, 25, 'recon complete');                 // pay for verified work
org.treasury.transfer('builder', 'scout', 10, 'contract');

console.log(org.status());                                // agents, treasury, spend, goals
await org.close();
```

**What an autonomous organization is, in code:**

| Piece | Class | Role |
|---|---|---|
| Treasury | `Treasury` | ledger — agents earn/spend, file-backed, 1 unit = 1¢ |
| Policy | `Policy` | budget caps + capability allow/deny — authorizes every action |
| Member | `OrgAgent` | a named agent with a role; acts through `org.execute` |
| Organization | `Org` | hires members, holds goals, runs the bounded loop, reports status |

The org never weakens governance — `Policy.authorize` runs before any tool call, and
`denyTools` blocks a capability before the runtime is touched. Spend is metered per
action and per org. This is the same governed-autonomy law as the runtime, lifted to
the organization level.

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
