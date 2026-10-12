#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def invoke(binary: Path, args: list[str], manifest: Path) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["DSCO_SUBSCRIPTION_PATHS"] = str(manifest)
    env["DSCO_SECURE_STORE_NO_PROMPT"] = "1"
    return subprocess.run(
        [str(binary), *args], text=True, capture_output=True, env=env, timeout=20,
        check=False,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    with tempfile.TemporaryDirectory(prefix="dsco-subscription-paths-") as temp:
        manifest = Path(temp) / "paths.json"
        manifest.write_text(json.dumps({
            "schema": "dsco.subscription-paths.v1",
            "paths": [{
                "id": "fixture-cat-plan",
                "provider": "fixture-vendor",
                "product": "Fixture Unlimited",
                "principal_profile": "team-a",
                "transport": "external_process",
                "billing": "included",
                "auth_mode": "fixture-cli-session",
                "model": "fixture-model",
                "executable": "/bin/cat",
                "argv": [],
                "prompt_transport": "stdin",
                "authenticated": "ready",
                "enabled": True,
                "qualified": True,
                "automatic_eligible": True,
            }],
        }))

        status = invoke(binary, ["interop", "status", "--json"], manifest)
        assert status.returncode == 0, status.stderr
        status_doc = json.loads(status.stdout)
        assert status_doc["subscription_path_limit"] == 65_536
        path = status_doc["subscription_paths"][0]
        assert path["cataloged"] and path["installed"] and path["ready"]
        assert path["qualified"] and path["automatic_eligible"]

        lanes = invoke(binary, ["auth", "lanes"], manifest)
        assert lanes.returncode == 0, lanes.stderr
        lanes_doc = json.loads(lanes.stdout)
        lane = next(row for row in lanes_doc["lanes"] if row.get("adapter_id") == "fixture-cat-plan")
        assert lane["id"] == "subscription:fixture-cat-plan:team-a"
        assert lane["automatic_eligible"] is True

        prompt = "literal $(touch never) ; semicolons remain data"
        run = invoke(binary, [
            "interop", "run", "fixture-cat-plan", "--prompt", prompt, "--json",
        ], manifest)
        assert run.returncode == 1, run.stderr
        run_doc = json.loads(run.stdout)
        assert run_doc["process_outcome"] == "exit_zero"
        assert run_doc["provider_outcome"] == "unknown" and not run_doc["ok"]
        assert run_doc["outcome"] == "incomplete"
        assert run_doc["adapter"] == "fixture-cat-plan"
        assert run_doc["output"] == prompt

        bad = Path(temp) / "bad.json"
        bad.write_text(json.dumps({
            "schema": "dsco.subscription-paths.v1",
            "paths": [{
                "id": "bad", "provider": "bad", "product": "Bad",
                "model": "m", "executable": "/bin/cat",
                "argv": ["--prompt={prompt}"], "prompt_transport": "argv",
            }],
        }))
        rejected = invoke(binary, ["interop", "status", "--json"], bad)
        assert rejected.returncode == 0, rejected.stderr
        rejected_doc = json.loads(rejected.stdout)
        assert rejected_doc["subscription_paths"] == []
        assert rejected_doc["subscription_path_manifests_valid"] is False
        assert rejected_doc["manifest_errors"]

        duplicate = Path(temp) / "duplicate.json"
        base = {
            "provider": "fixture", "product": "Fixture", "model": "m",
            "executable": "/bin/cat", "argv": [], "prompt_transport": "stdin",
            "authenticated": "ready", "enabled": True,
        }
        duplicate.write_text(json.dumps({
            "schema": "dsco.subscription-paths.v1",
            "paths": [
                {**base, "id": "duplicate", "lane_id": "subscription:duplicate:a"},
                {**base, "id": "duplicate", "lane_id": "subscription:duplicate:b"},
            ],
        }))
        duplicate_status = invoke(binary, ["interop", "status", "--json"], duplicate)
        assert duplicate_status.returncode == 0, duplicate_status.stderr
        duplicate_doc = json.loads(duplicate_status.stdout)
        assert duplicate_doc["subscription_path_manifests_valid"] is False
        assert any("duplicate subscription path identity" in error
                   for error in duplicate_doc["manifest_errors"])
        duplicate_run = invoke(binary, [
            "interop", "run", "duplicate", "--prompt", "must not run",
        ], duplicate)
        assert duplicate_run.returncode == 2
        assert "registry is invalid" in duplicate_run.stderr

    print("subscription_paths_binary: ok")


if __name__ == "__main__":
    main()
