#!/usr/bin/env python3
"""Real workspace review through installed Lingo; also its read-only collector."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
SCOPES = {
    "cli": "dsco-cli", "sdk": "distributed-systems-python", "chimera": "dsco-router",
    "autobot": "dsco-autobot/ToolManagement",
    "graphsub": "dsco-graphsub-complex/dsco-graphsub",
}
EXTENSIONS = {".c", ".h", ".lua", ".lingo", ".py", ".rs", ".ts", ".tsx", ".js",
              ".jsx", ".go", ".swift", ".ex", ".exs", ".erl", ".hrl", ".sh"}
EXCLUDED = {"reports", "memory", "docs", "doctrine", "node_modules", "vendor", "target",
            "build", "dist", "third_party", "venv", "env", "__pycache__", "site-packages"}
CONFLICTS = {"DD", "AU", "UD", "UA", "DU", "AA", "UU"}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def environment():
    env = {k: os.environ[k] for k in ("PATH", "HOME", "LANG", "TMPDIR") if k in os.environ}
    env.update(GIT_OPTIONAL_LOCKS="0", GIT_TERMINAL_PROMPT="0", GIT_PAGER="cat")
    return env


def collect(root):
    rows = []
    for name, relative in SCOPES.items():
        scope = (root / relative).resolve()
        row = dict(name=name, scope=str(scope), owner="", observed_at=int(time.time()),
                   head="", status="error", error="", tracked=0, untracked=0,
                   source_changes=0, conflicts=0, source_sample=[], status_sha256="")
        try:
            def git(*args):
                return subprocess.run(
                    ["git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-c",
                     "core.untrackedCache=false", "-c", "status.relativePaths=false",
                     "-C", str(scope), *args], env=environment(), check=True,
                    capture_output=True, timeout=20).stdout

            row["owner"] = git("rev-parse", "--show-toplevel").decode().strip()
            row["head"] = git("rev-parse", "HEAD").decode().strip()
            raw = git("status", "--porcelain=v1", "-z", "--untracked-files=all", "--", ".")
            row["status_sha256"] = digest(raw)
            records, index, sources = raw.split(b"\0"), 0, set()
            while index < len(records) and records[index]:
                record = records[index]
                code = record[:2].decode("ascii")
                filename = record[3:].decode("utf-8", errors="replace")
                index += 1
                # -z gives destination then source for rename/copy entries.
                if "R" in code or "C" in code:
                    index += 1
                row["untracked" if code == "??" else "tracked"] += 1
                row["conflicts"] += code in CONFLICTS
                path = Path(filename)
                if path.suffix in EXTENSIONS and not any(
                        part.startswith(".") or part in EXCLUDED for part in path.parts):
                    sources.add(filename)
            row.update(status="ok", source_changes=len(sources), source_sample=sorted(sources)[:8])
        except (OSError, subprocess.SubprocessError) as exc:
            # Retain failure type, not possibly sensitive arbitrary stderr.
            row["error"] = type(exc).__name__
        row["observed_at"] = int(time.time())
        rows.append(row)
    return dict(schema="workspace-review-observation/v1", completed_at=int(time.time()),
                collector_sha256=digest(Path(__file__).read_bytes()),
                repositories=rows, source_extensions=sorted(EXTENSIONS),
                excluded_directory_names=sorted(EXCLUDED), exclude_hidden_paths=True,
                scope="Five explicit component directories; status scoped to each directory",
                atomic_snapshot=False)


def verify(options):
    binary = options.binary.resolve(strict=True)
    report = (options.report or ROOT / "reports" /
              f"lingo-workspace-review-{time.strftime('%Y%m%d-%H%M%S')}-{os.getpid()}").resolve()
    report.mkdir(parents=True, exist_ok=False)
    program = ROOT / "examples/lingo/workspace-review.lingo"
    snapshot_path = report / "world.json"
    env = environment()
    env.update(DSCO_ALLOW_RUN="1", DSCO_ALLOW_NET="0", DSCO_ALLOW_WRITE="1",
               DSCO_ALLOW_SECRETS="0", DSCO_ALLOW_CONTROL="0")

    def run(name, args, write=True, success=True):
        run_env = dict(env, DSCO_ALLOW_WRITE="1" if write else "0")
        result = subprocess.run([str(binary), "lingo", "run", str(program),
                                 json.dumps(args, separators=(",", ":"))],
                                cwd=ROOT, env=run_env, capture_output=True, text=True, timeout=150)
        (report / f"{name}.stderr.txt").write_text(result.stderr)
        (report / f"{name}.stdout.json").write_text(result.stdout)
        if not success:
            assert result.returncode != 0, "negative test unexpectedly succeeded"
            return result
        assert result.returncode == 0, f"{name} failed: {result.stderr[-2000:]} {result.stdout[-2000:]}"
        receipt = json.loads(result.stdout)
        assert receipt["source_sha256"] == digest(program.read_bytes())
        assert receipt["api"] == "0.2" and receipt["engine"] == "LuaJIT"
        return receipt

    capture = run("capture", dict(action="capture", root=str(options.root.resolve()),
                                  collector=str(Path(__file__).resolve()), output=str(snapshot_path)))
    observed = capture["value"]["observations"]
    assert len(observed["repositories"]) == len(SCOPES)
    assert all(row["status"] == "ok" for row in observed["repositories"])
    assert capture["tool_calls"] == 2, "expected one collector and one governed write"
    snapshot = json.loads(snapshot_path.read_text())
    reopened = run("reopen", dict(action="reopen", snapshot=snapshot), write=False)
    assert reopened["tool_calls"] == 0
    first, second = capture["value"], reopened["value"]
    for key in ("baseline", "smallest", "sdk", "expired", "restored", "address", "manifest",
                "collection", "explanation"):
        assert first[key] == second[key], f"fresh-process mismatch: {key}"
    assert first["baseline"] == first["restored"]
    assert second["before"]["state"] == "unread"
    assert second["explanation"]["state"] == "valid"
    assert second["explanation"]["evaluations"] == 1
    assert len(second["explanation"]["dependencies"]) > 0
    assert second["collection"]["collector_sha256"] == digest(Path(__file__).read_bytes())

    # Independent Python oracle, using the recorded inputs, not a second capture
    # that can race ongoing work. Agent separately verifies live scoped counts.
    rows = observed["repositories"]
    for key, focus, smallest in (("baseline", "all", False), ("smallest", "all", True),
                                  ("sdk", "sdk", False)):
        eligible = [r for r in rows if (focus == "all" or r["name"] == focus)
                    and (r["source_changes"] or r["conflicts"])]
        expected = sorted(eligible, key=lambda r: (not bool(r["conflicts"]),
                          r["source_changes"] * (1 if smallest else -1), r["name"]))
        actual = first[key]
        assert actual["status"] == ("ready" if expected else "no_work")
        assert actual["selected"] == (expected[0]["name"] if expected else None)
        assert [r["name"] for r in actual["queue"]] == [r["name"] for r in expected]
    assert first["expired"]["status"] == "needs_evidence"
    assert first["expired"]["selected"] is None
    assert len(first["expired"]["needs"]) == len(SCOPES)

    denied_path = report / "must-not-exist.json"
    denied = run("write-denied", dict(action="capture", root=str(options.root.resolve()),
                 collector=str(Path(__file__).resolve()), output=str(denied_path)),
                 write=False, success=False)
    assert not denied_path.exists()
    assert "DSCO_ALLOW_WRITE=0" in denied.stdout and "governance_block" in denied.stdout
    # Explanations must start unread after load, then be reconstructed on demand.
    verification = dict(status="PASS", scope="Real local metadata; no code quality or build claim",
        binary=str(binary), binary_sha256=digest(binary.read_bytes()),
        source_sha256=digest(program.read_bytes()), collector_sha256=digest(Path(__file__).read_bytes()),
        snapshot_sha256=digest(snapshot_path.read_bytes()),
        process_ids="Distinct subprocess launches for capture, reopen, and denied write",
        checks={"five_live_components": True, "two_governed_capture_calls": True,
          "source_bound_fresh_process_reopen": True, "reopen_has_zero_host_calls": True,
          "explanation_rebuilt_from_unread": True, "collection_rules_survive_reopen": True,
          "python_ranking_oracle": True, "scenario_restores_base": True,
          "expired_evidence_refuses_selection": True, "write_opt_out_enforced": True},
        selections={k: first[k]["selected"] for k in ("baseline", "smallest", "sdk", "expired")},
        before=second["before"], explanation=second["explanation"])
    (report / "verification.json").write_text(json.dumps(verification, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "report": str(report), "selections": verification["selections"],
                      "source_changes": {r["name"]: r["source_changes"] for r in rows},
                      "checks": len(verification["checks"]),
                      "recomputed_dependencies": len(second["explanation"]["dependencies"])}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--collect", action="store_true")
    parser.add_argument("--root", type=Path, default=ROOT.parent)
    parser.add_argument("--binary", type=Path, default=Path.home() / ".local/bin/dsco")
    parser.add_argument("--report", type=Path, help="New report directory; defaults to a unique run directory")
    options = parser.parse_args()
    if options.collect:
        print(json.dumps(collect(options.root.resolve()), separators=(",", ":")))
    else:
        verify(options)
