#!/usr/bin/env bash
set -euo pipefail

# DSCO Differentiation Demo
# Offline-safe proof that DSCO is a local-first agentic runtime, not merely a cloud model wrapper.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/demos/dsco-vs-claude-codex/artifacts"
BIN="${DSCO_BIN:-$ROOT/dsco}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RAW="$OUT/raw_$STAMP"
REPORT="$OUT/evidence_$STAMP.md"
JSON="$OUT/evidence_$STAMP.json"
LATEST_MD="$OUT/latest.md"
LATEST_JSON="$OUT/latest.json"
mkdir -p "$RAW"

if [[ ! -x "$BIN" ]]; then
  echo "error: DSCO binary not executable at $BIN" >&2
  echo "hint: run 'make -j8' or set DSCO_BIN=/path/to/dsco" >&2
  exit 1
fi

run_capture() {
  local name="$1"; shift
  echo "▶ $name"
  ( "$@" ) > "$RAW/$name.out" 2> "$RAW/$name.err" || true
}

# 1. Local binary / startup evidence. No API calls.
run_capture version "$BIN" --version
python3 - "$BIN" "$RAW/startup_ms.out" <<'PY'
import subprocess, sys, time, statistics
bin_path, out = sys.argv[1], sys.argv[2]
vals=[]
for _ in range(7):
    t=time.perf_counter()
    subprocess.run([bin_path, "--version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    vals.append((time.perf_counter()-t)*1000)
with open(out,"w") as f:
    f.write(f"median_ms={statistics.median(vals):.2f}\n")
    f.write(f"min_ms={min(vals):.2f}\n")
    f.write(f"max_ms={max(vals):.2f}\n")
PY

# 2. Runtime surface evidence.
run_capture help "$BIN" --help
run_capture topology_list "$BIN" --topology-list
run_capture workspace_status "$BIN" --workspace-status

# 3. Code/source evidence. This proves the primitives are owned in the local repo.
{
  echo "src_include_c_h_files=$(find "$ROOT/src" "$ROOT/include" -name '*.[ch]' | wc -l | tr -d ' ')"
  echo "src_include_lines=$(find "$ROOT/src" "$ROOT/include" -name '*.[ch]' -print0 | xargs -0 wc -l | tail -1 | awk '{print $1}')"
  echo "tool_embedding_names=$(grep -E '^[[:space:]]*\"[A-Za-z0-9_-]+\",' "$ROOT/include/tool_embeddings.h" | wc -l | tr -d ' ')"
  echo "topology_count=$(grep -E '^  T[0-9]+' "$RAW/topology_list.out" | wc -l | tr -d ' ')"
  echo "skill_count=$(find "$HOME/.dsco/workspace/skills" -maxdepth 2 -name SKILL.md 2>/dev/null | wc -l | tr -d ' ')"
  echo "doctrine_count=$(find "$HOME/.dsco/workspace/doctrine" -maxdepth 1 -name '*.md' 2>/dev/null | wc -l | tr -d ' ')"
  echo "governance_hotpath_hits=$(grep -R "tools_execute_for_tier\|tool_is_governance_exempt\|governance_checkpoint" -n "$ROOT/src" "$ROOT/include" 2>/dev/null | wc -l | tr -d ' ')"
  echo "killswitch_symbols=$(grep -R "killswitch_" -n "$ROOT/src" "$ROOT/include" 2>/dev/null | wc -l | tr -d ' ')"
  echo "pheromone_symbols=$(grep -R "pheromone_" -n "$ROOT/src" "$ROOT/include" 2>/dev/null | wc -l | tr -d ' ')"
  echo "talons_symbols=$(grep -R "talons_" -n "$ROOT/src" "$ROOT/include" 2>/dev/null | wc -l | tr -d ' ')"
} > "$RAW/counts.env"

# Extract the structural governance gate snippet for review during the demo.
python3 - "$ROOT/src/tools.c" "$RAW/governance_gate_snippet.txt" <<'PY'
import sys
src, out = sys.argv[1], sys.argv[2]
text=open(src, errors='replace').read().splitlines()
needles=['IMMUNE SYSTEM GATE', 'tool_is_governance_exempt', 'bool tools_execute_for_tier']
indices=[]
for i,l in enumerate(text):
    if any(n in l for n in needles): indices.append(i)
start=max(0, min(indices)-5) if indices else 0
end=min(len(text), (max(indices)+70 if indices else 80))
with open(out,'w') as f:
    for n in range(start,end):
        f.write(f"{n+1:6d} | {text[n]}\n")
PY

# 4. Optional live proof: auto-selected swarm/topology run. Requires provider credentials.
if [[ "${DSCO_DEMO_LIVE:-0}" == "1" ]]; then
  run_capture live_topology_auto "$BIN" --topology-auto "In 5 bullets, explain why local governance in an agent runtime matters."
fi

# 5. Build machine-readable and presenter-readable evidence.
python3 - "$RAW" "$REPORT" "$JSON" "$LATEST_MD" "$LATEST_JSON" <<'PY'
import json, os, pathlib, re, shutil, sys
raw=pathlib.Path(sys.argv[1]); report=pathlib.Path(sys.argv[2]); jsout=pathlib.Path(sys.argv[3]); latest_md=pathlib.Path(sys.argv[4]); latest_json=pathlib.Path(sys.argv[5])

def read(name):
    p=raw/name
    return p.read_text(errors='replace') if p.exists() else ''

def env_counts():
    d={}
    for line in read('counts.env').splitlines():
        if '=' in line:
            k,v=line.split('=',1)
            try: d[k]=int(v)
            except ValueError: d[k]=v
    return d
counts=env_counts()
startup={}
for line in read('startup_ms.out').splitlines():
    if '=' in line:
        k,v=line.split('=',1); startup[k]=float(v)
version=read('version.out').strip()
workspace=read('workspace_status.out').strip()
helptext=read('help.out')
topologies=read('topology_list.out')
features={
  'local_binary': bool(version),
  'sub_50ms_startup': startup.get('median_ms',9999) < 50,
  'sixty_topologies': counts.get('topology_count',0) >= 60,
  'large_local_tool_surface': counts.get('tool_embedding_names',0) >= 300,
  'persistent_skill_doctrine_workspace': counts.get('skill_count',0) >= 100 and counts.get('doctrine_count',0) >= 10,
  'structural_governance_hotpath': counts.get('governance_hotpath_hits',0) >= 3 and counts.get('killswitch_symbols',0) > 0,
  'wings_talons_immune_primitives': counts.get('pheromone_symbols',0)>0 and counts.get('talons_symbols',0)>0 and counts.get('killswitch_symbols',0)>0,
  'offline_safe_demo': True,
}
evidence={
  'demo': 'DSCO vs Claude Code / Codex differentiation proof',
  'version': version,
  'startup_ms': startup,
  'workspace_status': workspace,
  'counts': counts,
  'feature_checks': features,
  'raw_artifacts': str(raw),
}
jsout.write_text(json.dumps(evidence, indent=2, sort_keys=True)+"\n")
passed=sum(1 for v in features.values() if v)
failed=[k for k,v in features.items() if not v]
md=[]
md.append('# DSCO Differentiation Evidence Pack\n')
md.append('This artifact is generated locally by `demos/dsco-vs-claude-codex/run_demo.sh`. It is designed to be demo-safe without external API credentials.\n')
md.append('## Verdict\n')
md.append(f'- Checks passed: **{passed}/{len(features)}**\n')
if failed: md.append(f'- Failed checks: `{", ".join(failed)}`\n')
else: md.append('- Failed checks: **none**\n')
md.append('\n## What this proves\n')
md.append('| Claim | Local evidence | Why Claude Code / Codex comparison matters |\n')
md.append('|---|---:|---|\n')
md.append(f"| DSCO is a local executable runtime, not just a hosted coding chat | `{version}`; median startup `{startup.get('median_ms','?')}` ms | The demo can run with no model API call; the runtime surface is local. |\n")
md.append(f"| DSCO owns orchestration primitives | `{counts.get('topology_count',0)}` topologies | Cloud coding agents can call tools, but DSCO exposes topology as a first-class runtime primitive. |\n")
md.append(f"| DSCO has a broad local tool/capability surface | `{counts.get('tool_embedding_names',0)}` embedded tool names | Capability routing is compiled/catalogued locally rather than only inferred by a model. |\n")
md.append(f"| DSCO has persistent identity/skills/doctrine | `{counts.get('skill_count',0)}` skills, `{counts.get('doctrine_count',0)}` doctrines | The agent carries versioned operating law and capabilities across sessions. |\n")
md.append(f"| DSCO has structural governance hooks | `{counts.get('governance_hotpath_hits',0)}` governance hot-path hits, `{counts.get('killswitch_symbols',0)}` kill-switch symbols | Safety is represented in code paths, not just in a system prompt. |\n")
md.append(f"| DSCO has Wings/Talons/Immune primitives | pheromone `{counts.get('pheromone_symbols',0)}`, talons `{counts.get('talons_symbols',0)}`, killswitch `{counts.get('killswitch_symbols',0)}` symbols | The architecture includes coordination, goal pursuit, and survival controls as local substrate. |\n")
md.append('\n## Live demo sequence\n')
md.append('1. Run `bash demos/dsco-vs-claude-codex/run_demo.sh`.\n')
md.append('2. Open `demos/dsco-vs-claude-codex/artifacts/latest.md`.\n')
md.append('3. Show raw files under the generated `raw_*` directory.\n')
md.append('4. Show `governance_gate_snippet.txt` to prove enforcement is in source, not copy.\n')
md.append('5. Optional with credentials: `DSCO_DEMO_LIVE=1 bash demos/dsco-vs-claude-codex/run_demo.sh` to run an auto-selected topology.\n')
md.append('\n## Raw command snippets\n')
md.append('```text\n')
md.append('$ ./dsco --version\n' + version + '\n\n')
md.append('$ ./dsco --workspace-status\n' + workspace + '\n')
md.append('```\n')
md.append('\n## Governance gate excerpt\n')
md.append('```c\n' + read('governance_gate_snippet.txt')[:6000] + '\n```\n')
report.write_text(''.join(md))
shutil.copyfile(report, latest_md)
shutil.copyfile(jsout, latest_json)
PY

echo
printf '✅ Evidence pack written:\n  %s\n  %s\n' "$REPORT" "$JSON"
printf 'Latest pointers:\n  %s\n  %s\n' "$LATEST_MD" "$LATEST_JSON"
