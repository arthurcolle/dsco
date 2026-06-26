# DSCO vs Claude Code / Codex Demo

A 7-minute, offline-safe proof that DSCO is not merely a model wrapper. It demonstrates local runtime ownership, first-class swarm topology, persistent skill/doctrine state, and structural governance primitives.

## Run

```bash
bash demos/dsco-vs-claude-codex/run_demo.sh
open demos/dsco-vs-claude-codex/artifacts/latest.md
```

Optional live model-backed topology run:

```bash
DSCO_DEMO_LIVE=1 bash demos/dsco-vs-claude-codex/run_demo.sh
```

## Demo Thesis

Claude Code and Codex are strong model/executor products. DSCO is a different category: a local-first agentic runtime with compiled tools, persistent operating law, swarm topology, and governance as substrate.

## The Proofs

| Proof | Command / artifact | Point |
|---|---|---|
| Local binary runtime | `./dsco --version`, startup timing | The demo runs before any model call. |
| Topology substrate | `./dsco --topology-list` | Orchestration is a runtime primitive, not a prompt pattern. |
| Local capability catalog | `include/tool_embeddings.h` count | Tool/capability routing is compiled and inspectable. |
| Persistent skill/doctrine workspace | `./dsco --workspace-status` + `~/.dsco/workspace` counts | DSCO carries operational memory and doctrine across sessions. |
| Structural governance | `src/tools.c` hot-path excerpt | Safety gates live in the tool execution path. |
| Wings/Talons/Immune | source symbol counts | Coordination, pursuit, and survival controls exist as code. |

## 7-Minute Presenter Track

1. **Set frame:** “This is not a coding benchmark. Claude/Codex are excellent model frontends. This proves DSCO owns a local agent runtime substrate.”
2. **Run one command:** `bash demos/dsco-vs-claude-codex/run_demo.sh`.
3. **Show no-API startup:** version and median startup timing.
4. **Show topology list:** 60 named orchestration topologies.
5. **Show workspace status:** identity, soul, memory, skills, doctrine are local state.
6. **Show governance source excerpt:** the execution hot path checks governance/kill switch before dispatch.
7. **Close:** “Claude/Codex can be workers inside DSCO. DSCO is the operating layer.”

## Objection Handling

**“Couldn’t Claude Code inspect a repo too?”**  
Yes. The difference is not file inspection. The difference is that DSCO has the local runtime primitives being inspected: topology registry, skill/doctrine workspace, governance hot path, and persistent agent state.

**“Is this just a wrapper around Claude?”**  
No. This demo intentionally avoids provider calls. The runtime evidence is local: binary startup, source symbols, topology catalog, workspace state, and governance code.

**“Why does this matter commercially?”**  
Enterprise/on-prem buyers care about sovereignty, auditability, governance, and persistence. This demo makes those visible in under 10 minutes.
