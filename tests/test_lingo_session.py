#!/usr/bin/env python3
"""Native workbench, MCP session, PTY and real read-only workspace conformance.

No provider or network calls. All writes stay in a private temporary directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
local l=require("lingo")
local t=l.types
local w=l.world{id="workbench-conformance"}
w:define("Review",{
  order=l.stored(t.string,"largest"),quantity=l.stored(t.integer,3),
  enabled=l.stored(t.boolean,true),
  decision=l.derived(t.json,function(self)
    assert(self.quantity>=0,"quantity must be nonnegative")
    if self.quantity==99 then
      l.call("write_file",{path=args.effect_marker,content="forbidden"})
    end
    local name=self.order=="largest" and "alpha" or "beta"
    local units=self.order=="largest" and 6 or 2
    return {selected=name,total=self.enabled and units*self.quantity or 0}
  end),
  danger=l.derived(t.string,function()
    l.call("write_file",{path=args.effect_marker,content="forbidden"})
    return "unsafe"
  end),
  loop=l.derived(t.integer,function() local n=0;while true do n=n+1 end;return n end),
  memory=l.derived(t.string,function() return string.rep("x",64*1024*1024) end),
  scaled=l.derived(t.integer,function(self,context,parameters)
    return self.quantity*parameters.factor
  end,{params={factor=t.integer}}),
},{version="1"})
return l.view{title="Conformance review",world=w,
  target={object="review",field="decision"},
  controls={
    {object="review",field="order",choices={"largest","smallest"}},
    {object="review",field="quantity"},
    {object="review",field="enabled"},
  },
  initialize=function()
    local receipt=l.call("read_file",{path=args.observation,limit=5})
    assert(receipt.ok,receipt.result)
    w:new("Review","review",{})
  end,
}
'''


class Wire:
    """Line transport with deadlines and guaranteed cleanup of owned processes."""
    def __init__(self, command, cwd, env, timeout=30):
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.log,
                                        start_new_session=True)
        self.buffer = b""
        self.timeout = timeout

    def line(self):
        deadline = time.monotonic() + self.timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "timed out waiting for native JSON response"
            readable, _, _ = select.select([self.process.stdout], [], [], remaining)
            assert readable, "native JSON response deadline expired"
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                self.log.seek(0)
                raise AssertionError(("unexpected native EOF", self.buffer,
                                      self.log.read().decode("utf-8", "replace")[-4000:]))
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def send(self, value, raw=False):
        encoded = value if raw else json.dumps(value, allow_nan=False, separators=(",", ":"))
        self.process.stdin.write(encoded.encode("utf-8") + b"\n")
        self.process.stdin.flush()
        return self.line()

    def stop(self):
        if self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
                self.process.wait(timeout=3)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                if self.process.poll() is None:
                    os.killpg(self.process.pid, signal.SIGKILL)
                    self.process.wait(timeout=3)
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default=str(ROOT / "dsco"))
    parser.add_argument("--report", type=Path)
    parser.add_argument("--skip-workspace", action="store_true",
                        help="explicitly skip the five real workspace Git observations")
    options = parser.parse_args()
    binary = str(Path(options.binary).resolve(strict=True))
    binary_hash = hashlib.sha256(Path(binary).read_bytes()).hexdigest()
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "DSCO_ENV_FILE": "/dev/null",
           "DSCO_PRICING_OFFLINE": "1", "DSCO_SECURE_STORE_NO_PROMPT": "1",
           "DSCO_GOV_MODEL": "standard", "DSCO_NO_AUTO_SUPERVISE": "1",
           "DSCO_ALLOW_NET": "0", "DSCO_TOOLMGMT": "0", "DSCO_NO_COLOR": "1"}
    checks, evidence = [], {}

    def checked(name):
        checks.append(name)
        print("PASS", name, flush=True)

    def success(reply):
        assert "error" not in reply, reply
        return reply["view"]

    def rejected(reply, substring=None):
        error = reply.get("error")
        assert isinstance(error, str) or (isinstance(error, dict) and isinstance(error.get("message"), str)), reply
        if substring:
            assert substring in json.dumps(reply), reply

    with tempfile.TemporaryDirectory(prefix="dsco-lingo-session-") as temporary:
        folder = Path(temporary)
        program = folder / "review.lingo"
        program.write_text(FIXTURE, encoding="utf-8")
        observation = folder / "observation.txt"
        observation.write_text("Synthetic conformance observation\n")
        effect_marker = folder / "forbidden-effect.txt"
        arguments = {"observation": str(observation), "effect_marker": str(effect_marker)}
        source_hash = hashlib.sha256(program.read_bytes()).hexdigest()

        def session(path=program, args=arguments, restore=None, overrides=None, timeout=30):
            command = [binary, "lingo", "open", str(path)]
            if args is not None:
                command.append(json.dumps(args))
            if restore:
                command.extend(["--restore", str(restore)])
            command.append("--json")
            return Wire(command, folder, env | (overrides or {}), timeout)

        def close(wire, expected=0):
            assert success(wire.send({"action": "close"}))["closed"] is True
            assert wire.process.wait(timeout=10) == expected

        saved = folder / "desk.session.json"
        with session() as wire:
            initial = wire.line()
            view = success(initial)
            assert view["format"] == "lingo.view.result/1" and view["title"] == "Conformance review"
            assert view["value"] == {"selected": "alpha", "total": 18} and view["context"] == "base"
            assert initial["tool_calls"] == initial["command_tool_calls"] == 1
            assert initial["source_sha256"] == source_hash and initial["session_id"]
            checked("open retains one native VM after one governed observation read")
            changed = wire.send({"action": "set", "control": 1, "value": "smallest"})
            view = success(changed)
            assert view["value"] == {"selected": "beta", "total": 6}
            assert view["context"] == "scenario" and view["baseline"] == initial["view"]["value"]
            assert changed["session_id"] == initial["session_id"] and changed["command_tool_calls"] == 0
            assert view["target"] == initial["view"]["target"]
            assert success(wire.send({"action": "set", "control": 2, "value": 5}))["value"]["total"] == 10
            checked("controls evaluate a scenario against an unchanged base and logical address")
            assert success(wire.send({"action": "set", "control": 3, "value": False}))["value"]["total"] == 0
            assert success(wire.send({"action": "set", "control": 3, "value": True}))["value"]["total"] == 10
            checked("boolean false remains a typed control value")
            for control, value in [(2, "5"), (2, 1.5), (1, "sideways"), (2, -1),
                                   (0, 1), (True, 1), ("2", 1), (2, None)]:
                rejected(wire.send({"action": "set", "control": control, "value": value}))
                after = success(wire.send({"action": "read"}))
                assert after["value"] == {"selected": "beta", "total": 10}
            checked("bad types, indices, choices and failed calculations roll back candidate edits")
            rejected(wire.send({"action": "set", "control": 2, "value": 99}))
            rejected(wire.send({"action": "select", "object": "review", "field": "danger"}))
            after = wire.send({"action": "read"})
            assert success(after)["value"] == {"selected": "beta", "total": 10}
            assert after["tool_calls"] == 1 and not effect_marker.exists()
            checked("derived/scenario host calls cannot dispatch effects or alter the retained target")
            explanation = success(wire.send({"action": "why"}))
            assert explanation["object"] == "review" and explanation["dependencies"]
            controls = success(wire.send({"action": "controls"}))
            assert controls[1]["base"] == 3 and controls[1]["current"] == 5 and controls[1]["overridden"]
            description = success(wire.send({"action": "inspect"}))
            assert description["class"] == "Review" and description["id"] == "review"
            assert {field["name"] for field in description["fields"]} >= {"order", "quantity", "decision"}
            checked("why, controls and inspect expose dependency, base/current and typed object information")
            selected = success(wire.send({"action": "select", "object": "review", "field": "quantity"}))
            assert selected["value"] == 5 and selected["baseline"] == 3
            saved_reply = wire.send({"action": "save", "path": str(saved)})
            assert success(saved_reply)["saved"] and saved_reply["command_tool_calls"] == 1
            assert saved_reply["tool_calls"] == 2
            packet = json.loads(saved.read_text())
            assert packet["format"] == "lingo.session/1" and packet["source_sha256"] == source_hash
            assert packet["args"] == arguments and packet["view"]["format"] == "lingo.view/1"
            base = next(row for row in packet["view"]["world"]["objects"] if row["id"] == "review")
            assert base["values"]["quantity"] == 3 and base["values"]["order"] == "largest"
            assert {row["index"]: row["value"] for row in packet["view"]["overrides"]}[2] == 5
            checked("explicit save uses one gated write and stores base objects separately from overrides")
            close(wire, expected=1)

        observation.unlink()
        with session(args=None, restore=saved) as wire:
            restored = wire.line()
            view = success(restored)
            assert view["value"] == 5 and view["target"] == selected["target"]
            assert view["context"] == "scenario" and view["baseline"] == 3
            assert restored["tool_calls"] == restored["command_tool_calls"] == 0
            assert restored["session_id"] != initial["session_id"]
            checked("a new process restores source-bound target and scenario without recollecting missing input")
            reset = success(wire.send({"action": "reset"}))
            assert reset["value"] == 3 and reset["context"] == "base"
            assert not any(control["overridden"] for control in reset["controls"])
            checked("reset clears scenario overrides while preserving the selected target")
            close(wire)
            evidence["deterministic"] = {"initial": initial, "restored": restored,
                                         "saved_packet_sha256": hashlib.sha256(saved.read_bytes()).hexdigest()}

        changed_source = folder / "changed.lingo"
        changed_source.write_text(FIXTURE + "\n-- changed source\n")
        with session(path=changed_source, args=None, restore=saved) as wire:
            rejected(wire.line(), "source identity mismatch")
            assert wire.process.wait(timeout=10) != 0
        with session(args={"different": True}, restore=saved) as wire:
            rejected(wire.line(), "saved program arguments")
            assert wire.process.wait(timeout=10) != 0
        checked("restore rejects different source bytes and different arguments before initialization")

        for alteration in ["view_implementation", "world_runtime", "definition", "stored_type", "control"]:
            altered = json.loads(saved.read_text())
            if alteration == "view_implementation":
                altered["view_sha256"] = "0" * 64
            elif alteration == "world_runtime":
                altered["view"]["world"]["runtime"]["runtime_sha256"] = "0" * 64
            elif alteration == "definition":
                altered["view"]["world"]["classes"]["Review"]["version"] = "changed"
            elif alteration == "stored_type":
                altered["view"]["world"]["objects"][0]["values"]["quantity"] = "3"
            else:
                altered["view"]["controls"][0]["choices"] = ["different"]
            altered_path = folder / (alteration + ".session.json")
            altered_path.write_text(json.dumps(altered))
            with session(args=None, restore=altered_path) as wire:
                rejected(wire.line())
                assert wire.process.wait(timeout=10) != 0
        checked("restore validates view/runtime/definition identities, stored types and declared controls")

        with session(args=None, restore=saved) as wire:
            success(wire.line())
            scaled = success(wire.send({"action": "select", "object": "review", "field": "scaled", "args": {"factor": 4}}))
            assert scaled["value"] == 20 and scaled["baseline"] == 12
            assert scaled["target"]["args"] == {"factor": 4}
            rejected(wire.send({"action": "select", "object": "review", "field": "scaled", "args": {"factor": "4"}}))
            assert success(wire.send({"action": "read"}))["target"] == scaled["target"]
            close(wire, expected=1)
        checked("parameterized selection binds typed arguments into the address and rejects invalid replacements")

        with session(args=None, restore=saved, overrides={"DSCO_ALLOW_RUN": "0"}) as wire:
            rejected(wire.line(), "DSCO_ALLOW_RUN")
            assert wire.process.wait(timeout=10) != 0
        checked("the session entry point obeys the native execution capability opt-out")

        denied_path = folder / "denied.session.json"
        with session(args=None, restore=saved, overrides={"DSCO_ALLOW_WRITE": "0"}) as wire:
            success(wire.line())
            rejected(wire.send({"action": "save", "path": str(denied_path)}), "DSCO_ALLOW_WRITE")
            after = wire.send({"action": "read"})
            assert success(after)["value"] == 5 and after["tool_calls"] == 1
            assert not denied_path.exists()
            close(wire, expected=1)
        checked("write opt-out denies save, counts the attempted call and preserves the usable desk")

        observation.write_text("Synthetic conformance observation\n")
        top_source = folder / "top-effect.lingo"
        top_source.write_text('local x=require("lingo");local r=x.call("write_file",'
                              '{path=args.effect_marker,content="initial"});assert(r.ok,r.result)\n' + FIXTURE)
        top_saved = folder / "top.session.json"
        with session(path=top_source) as wire:
            assert wire.line()["tool_calls"] == 2 and effect_marker.read_text() == "initial"
            success(wire.send({"action": "save", "path": str(top_saved)}))
            close(wire)
        effect_marker.unlink()
        with session(path=top_source, args=None, restore=top_saved) as wire:
            rejected(wire.line())
            assert wire.process.wait(timeout=10) != 0 and not effect_marker.exists()
        checked("restore forbids top-level host calls even before the view constructor")

        with session(args=None, restore=saved) as wire:
            success(wire.line())
            for raw in ['{', '[]', '{"action":"wat"}', '{"action":"read","value":1}',
                        '{"action":"read","session_id":"injected"}',
                        '{"action":"read","action":"close"}']:
                rejected(wire.send(raw, raw=True))
                assert success(wire.send({"action": "read"}))["value"] == 5
            close(wire, expected=1)
        checked("malformed JSON, duplicate fields and routing injection fail without losing the session")

        for field in ["loop", "memory"]:
            with session(args=None, restore=saved) as wire:
                success(wire.line())
                rejected(wire.send({"action": "select", "object": "review", "field": field}), "budget")
                rejected(wire.send({"action": "read"}), "close and reopen")
                close(wire, expected=1)
            checked(field + " exhaustion makes the VM unavailable but still allows close")

        with Wire([binary, "mcp", "serve", "--toolsets", "all", "--tier", "trusted"], folder, env) as wire:
            identifier = 0

            def rpc(method, params):
                nonlocal identifier
                identifier += 1
                response = wire.send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
                while response.get("id") != identifier:
                    response = wire.line()
                assert "error" not in response, response
                return response["result"]

            rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                               "clientInfo": {"name": "lingo-session-conformance", "version": "1"}})

            def tool(arguments, ok=True):
                result = rpc("tools/call", {"name": "lingo_session", "arguments": arguments})
                assert bool(result.get("isError")) != ok, result
                return json.loads(result["content"][0]["text"])

            opened = tool({"action": "open", "path": str(program), "restore_path": str(saved)})
            identifier_value = opened["session_id"]
            result = tool({"action": "set", "session_id": identifier_value, "control": 2, "value": 7})
            assert result["view"]["value"] == 7 and result["tool_calls"] == 0
            tool({"action": "set", "session_id": identifier_value, "control": 2, "value": "7"}, ok=False)
            assert tool({"action": "read", "session_id": identifier_value})["view"]["value"] == 7
            tool({"action": "close", "session_id": identifier_value})
            tool({"action": "read", "session_id": identifier_value}, ok=False)
            checked("MCP exposes the same typed session, recovery and process-local handle lifecycle")
            handles = [tool({"action": "open", "path": str(program), "restore_path": str(saved)})["session_id"]
                       for _ in range(4)]
            assert len(set(handles)) == 4
            rejected(tool({"action": "open", "path": str(program), "restore_path": str(saved)}, ok=False), "four")
            for handle in handles:
                assert tool({"action": "read", "session_id": handle})["view"]["value"] == 5
            tool({"action": "close", "session_id": handles.pop()})
            handles.append(tool({"action": "open", "path": str(program), "restore_path": str(saved)})["session_id"])
            for handle in handles:
                tool({"action": "close", "session_id": handle})
            checked("four independent MCP sessions are bounded and a closed slot can be reused")

        # The terminal is a real PTY; no terminal application or graphical UI is controlled.
        master, slave = pty.openpty()
        terminal = subprocess.Popen([binary, "lingo", "open", str(program), json.dumps(arguments)],
                                    cwd=folder, env=env, stdin=slave, stdout=slave, stderr=slave,
                                    start_new_session=True)
        os.close(slave)
        transcript = bytearray()

        def until_prompt():
            segment = bytearray()
            deadline = time.monotonic() + 30
            while b"lingo> " not in segment:
                remaining = deadline - time.monotonic()
                assert remaining > 0, segment.decode("utf-8", "replace")
                readable, _, _ = select.select([master], [], [], remaining)
                assert readable, "PTY prompt deadline expired"
                segment.extend(os.read(master, 65536))
            transcript.extend(segment)
            return segment.decode("utf-8", "replace")

        try:
            start = until_prompt()
            assert "Lingo workbench" in start and 'Title: "Conformance review"' in start
            assert "Value:" in start and "Controls:" in start and "alpha" in start
            os.write(master, b'set 1 "smallest"\n')
            assert "beta" in until_prompt()
            os.write(master, b"quit\n")
            assert terminal.wait(timeout=10) == 0
            evidence["pty"] = transcript.decode("utf-8", "replace")
            checked("actual PTY displays the desk, accepts a typed control and closes cleanly")
        finally:
            if terminal.poll() is None:
                os.killpg(terminal.pid, signal.SIGKILL)
                terminal.wait(timeout=3)
            os.close(master)

        if not options.skip_workspace:
            workspace_args = {"root": str(ROOT.parent),
                              "collector": str(ROOT / "scripts/verify_lingo_workspace_review.py")}
            workspace_saved = folder / "workspace.session.json"
            with session(path=ROOT / "examples/lingo/workspace-desk.lingo", args=workspace_args, timeout=150) as wire:
                opened = wire.line()
                assert success(opened)["title"] == "Workspace review desk"
                assert opened["tool_calls"] == 1
                assert success(wire.send({"action": "set", "control": 1, "value": "smallest"}))["value"]["order"] == "smallest"
                assert success(wire.send({"action": "set", "control": 2, "value": "sdk"}))["value"]["focus"] == "sdk"
                expired = success(wire.send({"action": "set", "control": 3, "value": 1000000000}))
                assert expired["value"]["status"] == "needs_evidence" and expired["value"]["selected"] is None
                assert expired["value"]["needs"] == ["sdk"]
                assert success(wire.send({"action": "reset"}))["context"] == "base"
                success(wire.send({"action": "save", "path": str(workspace_saved)}))
                close(wire)
            workspace_packet = json.loads(workspace_saved.read_text())
            capture = next(row for row in workspace_packet["view"]["world"]["objects"] if row["id"] == "capture")["values"]["metadata"]
            assert capture["atomic_snapshot"] is False
            rows = capture["repositories"]
            assert {row["name"] for row in rows} == {"cli", "sdk", "chimera", "autobot", "graphsub"}
            assert all(row["status"] == "ok" for row in rows), rows
            as_of = capture["completed_at"]
            eligible = [row for row in rows if (row["source_changes"] or row["conflicts"])
                        and 0 <= as_of - row["observed_at"] <= 300]
            eligible.sort(key=lambda row: (row["conflicts"] == 0, -row["source_changes"], row["name"]))
            assert [row["name"] for row in opened["view"]["value"]["queue"]] == [row["name"] for row in eligible]
            with session(path=ROOT / "examples/lingo/workspace-desk.lingo", args=None, restore=workspace_saved) as wire:
                reopened = wire.line()
                assert success(reopened)["value"] == opened["view"]["value"]
                assert reopened["view"]["target"] == opened["view"]["target"] and reopened["tool_calls"] == 0
                close(wire)
            evidence["workspace"] = {"initial": opened, "reopened": reopened, "capture": capture}
            checked("real five-component Git capture drives the desk and matches an independent ranking oracle")
            checked("workspace controls and a separate restored process reuse observations with zero further collection")

    assert hashlib.sha256(Path(binary).read_bytes()).hexdigest() == binary_hash, "binary changed during verification; rerun against a stable build"
    report = {"status": "PASS", "binary": binary,
              "binary_sha256": binary_hash,
              "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "example_sha256": hashlib.sha256((ROOT / "examples/lingo/workspace-desk.lingo").read_bytes()).hexdigest(),
              "passed": len(checks), "checks": checks, "evidence": evidence,
              "scope": "Native CLI, persistent MCP and actual PTY; private files; no provider/network calls",
              "workspace_skipped": options.skip_workspace}
    if options.report:
        options.report.parent.mkdir(parents=True, exist_ok=True)
        options.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "evidence"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
