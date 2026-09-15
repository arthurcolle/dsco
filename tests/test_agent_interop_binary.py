#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import subprocess
import tempfile


def run(binary, *args, env=None, check=True, timeout=20):
    proc = subprocess.run(
        [binary, *args], text=True, capture_output=True, env=env, timeout=timeout
    )
    if check and proc.returncode != 0:
        raise AssertionError(
            f"command failed ({proc.returncode}): {args}\nstdout={proc.stdout}\nstderr={proc.stderr}"
        )
    return proc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    ns = parser.parse_args()
    binary = str(pathlib.Path(ns.binary).resolve())

    manifest = json.loads(run(binary, "interop", "manifest").stdout)
    assert manifest["schema"] == "dsco.agent.interop/v1"
    assert {row["protocol"] for row in manifest["inbound"]} >= {"mcp", "acp", "headless"}
    assert manifest["authority"]["shell_interpolation"] is False

    status = json.loads(run(binary, "interop", "status", "--json").stdout)
    assert {row["id"] for row in status["adapters"]} == {
        "codex", "claude-code", "opencode", "omp", "hermes"
    }
    assert status["generic"]["prompt_transports"] == ["argv", "stdin"]
    leading = json.loads(run(binary, "--profile", "lite", "interop", "status", "--json").stdout)
    assert leading["schema"] == "dsco.agent.interop/v1"
    assert run(binary, "interop", "status", "--bogus", check=False).returncode == 2

    mcp = json.loads(run(binary, "interop", "mcp-config", "json", "--tier", "standard").stdout)
    assert mcp["mcpServers"]["dsco"]["args"] == ["mcp", "serve", "--tier", "standard"]
    toml = run(binary, "interop", "mcp-config", "toml").stdout
    assert "[mcp_servers.dsco]" in toml and '"mcp", "serve"' in toml

    with tempfile.TemporaryDirectory(prefix="dsco-interop-") as td:
        root = pathlib.Path(td)
        fake_body = (
            "#!/usr/bin/python3\n"
            "import json, os, sys\n"
            "print(json.dumps({'argv': sys.argv[1:], 'cwd': os.getcwd()}))\n"
        )
        for executable in ("codex", "claude", "opencode", "omp", "hermes"):
            fake = root / executable
            fake.write_text(fake_body)
            fake.chmod(0o755)
        env = os.environ.copy()
        env["PATH"] = f"{root}:/usr/bin:/bin"

        fake_status = json.loads(run(binary, "interop", "status", "--json", env=env).stdout)
        assert all(row["available"] for row in fake_status["adapters"])
        assert all(pathlib.Path(row["path"]).parent == root for row in fake_status["adapters"])

        canary = root / "must-not-exist"
        prompt = f"literal $(touch {canary}); still one argument"
        expected = {
            "codex": [
                "exec", "--json", "--approve-for-me", "--ephemeral",
                "--skip-git-repo-check", "-C", td,
                "-m", "test/model", prompt,
            ],
            "claude-code": [
                "-p", "--output-format", "stream-json", "--permission-mode",
                "dontAsk", "--no-session-persistence", "--verbose",
                "--model", "test/model", prompt,
            ],
            "opencode": [
                "run", "--format", "json", "--dir", td,
                "-m", "test/model", prompt,
            ],
            "omp": [
                "-p", "--mode", "json", "--no-session", "--approval-mode",
                "write", "--cwd", td, "--model", "test/model", prompt,
            ],
            "hermes": ["chat", "-m", "test/model", "-q", prompt],
        }
        for adapter, wanted in expected.items():
            wrapped = run(
                binary, "interop", "run", adapter, "--prompt", prompt,
                "--model", "test/model", "--cwd", td, "--json", env=env
            )
            receipt = json.loads(wrapped.stdout)
            assert receipt["adapter"] == adapter and receipt["ok"] is True
            child = json.loads(receipt["output"])
            assert child["cwd"] == str(root.resolve())
            assert child["argv"] == wanted, (adapter, child["argv"])
        assert not canary.exists(), "prompt was interpreted by a shell"

        templated = run(
            binary, "interop", "run", "generic", "--prompt", prompt,
            "--model", "model-x", "--cwd", td, "--json", "--",
            "/usr/bin/python3", "-c",
            "import json,sys; print(json.dumps(sys.argv[1:]))",
            "{prompt}", "{model}", "{cwd}",
        )
        templated_receipt = json.loads(templated.stdout)
        assert json.loads(templated_receipt["output"]) == [prompt, "model-x", td]

        for adapter, executable in (("opencode", "opencode"), ("omp", "omp")):
            acp = json.loads(run(binary, "interop", "acp-command", adapter, env=env).stdout)
            assert acp == [str(root / executable), "acp"]

        no_acp = run(binary, "interop", "acp-command", "codex", env=env, check=False)
        assert no_acp.returncode == 2 and not no_acp.stdout

    generic_prompt = "generic stdin preserves spaces, unicode λ, newlines\nand $() literally"
    generic = run(
        binary, "interop", "run", "generic", "--prompt", generic_prompt,
        "--stdin", "--json", "--", "/bin/cat"
    )
    generic_receipt = json.loads(generic.stdout)
    assert generic_receipt["ok"] is True
    assert generic_receipt["output"] == generic_prompt

    binary_output = run(
        binary, "interop", "run", "generic", "--prompt", "ignored",
        "--json", "--", "/usr/bin/python3", "-c",
        "import os; os.write(1, b'a\\x00\\xff')", "{model}",
    )
    binary_receipt = json.loads(binary_output.stdout)
    assert binary_receipt["output"] == "a\x00\ufffd"
    assert binary_receipt["output_base64"] == "YQD/"

    truncated = run(
        binary, "interop", "run", "generic", "--prompt", "ignored",
        "--max-output", "64", "--json", "--", "/usr/bin/python3", "-c",
        "import os; os.write(1, b'x' * 4096)",
    )
    truncated_receipt = json.loads(truncated.stdout)
    assert truncated_receipt["ok"] is True
    assert truncated_receipt["truncated"] is True
    assert truncated_receipt["output"] == "x" * 64

    failed = run(
        binary, "interop", "run", "generic", "--prompt", "ignored", "--json",
        "--", "/usr/bin/python3", "-c", "raise SystemExit(7)", check=False,
    )
    failed_receipt = json.loads(failed.stdout)
    assert failed.returncode == 7
    assert failed_receipt["ok"] is False and failed_receipt["exit_code"] == 7

    timed = run(
        binary, "interop", "run", "generic", "--prompt", "ignored",
        "--timeout-ms", "50", "--json", "--", "/usr/bin/python3", "-c",
        "import time; time.sleep(5)", check=False,
    )
    timed_receipt = json.loads(timed.stdout)
    assert timed.returncode != 0 and timed_receipt["timed_out"] is True

    missing = run(
        binary, "interop", "run", "generic", "--prompt", "x", "--",
        "/definitely/missing-agent", check=False,
    )
    assert missing.returncode == 127

    too_many = [
        binary, "interop", "run", "generic", "--prompt", "x", "--", "/bin/true",
        *[f"arg-{i}" for i in range(70)],
    ]
    assert subprocess.run(too_many, capture_output=True, timeout=20).returncode != 0

    invalid_tier = run(
        binary, "interop", "mcp-config", "toml", "--tier", 'trusted"\nbad=true',
        check=False,
    )
    assert invalid_tier.returncode == 2 and not invalid_tier.stdout

    for option, value in (("--timeout-ms", "1oops"), ("--max-output", "-1")):
        invalid_bound = run(
            binary, "interop", "run", "generic", "--prompt", "x",
            option, value, "--", "/bin/cat", check=False,
        )
        assert invalid_bound.returncode == 2 and not invalid_bound.stdout

    print(
        "agent interop binary: all named argv contracts, generic stdin/argv, "
        "failure/timeout/truncation, binary output, and MCP/ACP commands passed"
    )


if __name__ == "__main__":
    main()
