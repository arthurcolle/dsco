#!/usr/bin/env python3
"""Offline, real-binary JSON tools proof. No model calls or desktop surfaces."""
import argparse, json, os, pathlib, random, tempfile
from test_swarm_interop_mcp import Mcp


def run(binary):
    passed = 0
    with tempfile.TemporaryDirectory(prefix="dsco-json-tools-") as temp:
        env = os.environ.copy()
        env.update(DSCO_NO_DOTENV="1", DSCO_GOV_MODEL="none", DSCO_TRUST_TIER="trusted",
                   DSCO_APPROVAL_MODE="never", DSCO_ALLOW_RUN="0", DSCO_ALLOW_NET="0",
                   DSCO_ALLOW_WRITE="0", DSCO_ALLOW_CONTROL="0", DSCO_ALLOW_SECRETS="0")
        m = Mcp(binary, temp, env)
        try:
            names = {t["name"] for t in m.call("tools/list", {}).get("result", {}).get("tools", [])}
            assert {"json_pointer", "json_diff"} <= names, names
            def check(name, args, expected=None, error=False):
                nonlocal passed
                failed, body = m.tool(name, **args)
                assert failed == error, (name, args, failed, body)
                if expected is not None: assert body == expected, (body, expected)
                passed += 1
                return body
            doc = json.dumps({"a/b": {"~key": [None, "雪🚀"]}, "": 7})
            check("json_pointer", {"json": doc, "pointer": "/a~1b/~0key/1"}, {"found": True, "value": "雪🚀"})
            check("json_pointer", {"json": doc, "pointer": "/a~1b/~0key/0"}, {"found": True, "value": None})
            check("json_pointer", {"json": doc, "pointer": "/missing"}, {"found": False})
            check("json_pointer", {"json": doc, "pointer": "/"}, {"found": True, "value": 7})
            for raw in ["null", "false", "42", '"hello"', "[1,2]"]:
                check("json_pointer", {"json": raw, "pointer": ""}, {"found": True, "value": json.loads(raw)})
            for pointer in ["abc", "/~", "/~2", "/x\x00"]:
                check("json_pointer", {"json": doc, "pointer": pointer}, error=True)
            for raw in ['{"a":1,"a":2}', '{bad}', "NaN", "[1] garbage", "[" * 66 + "0" + "]" * 66]:
                check("json_pointer", {"json": raw, "pointer": ""}, error=True)
            check("json_pointer", {"json": "[1,2]", "pointer": "/01"}, {"found": False})
            check("json_pointer", {"json": "[1,2]", "pointer": "/-"}, {"found": False})
            check("json_diff", {"left": '{"a":1,"b":[null,true]}', "right": '{"b":[null,true],"a":1}'},
                  {"equal": True, "differences": [], "count": 0, "truncated": False})
            result = check("json_diff", {"left": '{"a/b":1,"gone":0,"x":[1]}', "right": '{"a/b":2,"x":[1,null],"new":3}'})
            assert {tuple(r.values()) for r in result["differences"]} == {
                ("/a~1b", "changed"), ("/gone", "removed"), ("/x/1", "added"), ("/new", "added")}, result
            check("json_diff", {"left": "1", "right": "null"},
                  {"equal": False, "differences": [{"path": "", "kind": "changed"}], "count": 1, "truncated": False})
            check("json_diff", {"left": "[1,2]", "right": "[3,4]", "max_differences": 1},
                  {"equal": False, "differences": [{"path": "/0", "kind": "changed"}], "count": 1, "truncated": True})
            for limit in [0, 129, -1, 1.5, True, "invalid"]:
                check("json_diff", {"left": "1", "right": "2", "max_differences": limit}, error=True)
            # The shared harness normalizes numeric strings to schema integers.
            check("json_diff", {"left": "1", "right": "2", "max_differences": "1"},
                  {"equal": False, "differences": [{"path": "", "kind": "changed"}], "count": 1, "truncated": False})
            check("json_diff", {"left": "[1,2]", "right": "[3,2]", "max_differences": 1},
                  {"equal": False, "differences": [{"path": "/0", "kind": "changed"}], "count": 1, "truncated": False})
            check("json_diff", {"left": "true", "right": "1"},
                  {"equal": False, "differences": [{"path": "", "kind": "changed"}], "count": 1, "truncated": False})
            check("json_diff", {"left": '{"a\\u0000b":1}', "right": '{"a\\u0000b":2}'},
                  {"equal": False, "differences": [{"path": "/a\x00b", "kind": "changed"}], "count": 1, "truncated": False})
            check("json_pointer", {"json": "[" * 64 + "0" + "]" * 64, "pointer": "/0" * 64},
                  {"found": True, "value": 0})
            check("json_pointer", {"json": "[" * 65 + "0" + "]" * 65, "pointer": ""}, error=True)
            large = "[" + ",".join("0" for _ in range(19999)) + "]"
            check("json_diff", {"left": large, "right": large},
                  {"equal": True, "differences": [], "count": 0, "truncated": False})
            check("json_pointer", {"json": "[" + ",".join("0" for _ in range(20000)) + "]", "pointer": ""}, error=True)
            obj = {str(i): i for i in range(1024)}
            check("json_pointer", {"json": json.dumps(obj), "pointer": "/1023"}, {"found": True, "value": 1023})
            obj["1024"] = 1024
            check("json_pointer", {"json": json.dumps(obj), "pointer": ""}, error=True)
            check("json_pointer", {"json": json.dumps("x" * (1024 * 1024)), "pointer": ""}, error=True)
            check("json_pointer", {"json": "null", "pointer": 1}, error=True)
            check("json_diff", {"left": "bad", "right": "1"}, error=True)
            check("json_diff", {"left": '{"a":1,"a":2}', "right": '{}'}, error=True)
            # Compare against independent Python equality, using bounded random integer/string trees.
            rng = random.Random(6901)
            def value(depth=0):
                if depth >= 3 or rng.randrange(3) == 0: return rng.choice([None, False, True, rng.randrange(50), "雪", "x/y~z"])
                if rng.randrange(2): return [value(depth+1) for _ in range(rng.randrange(4))]
                return {str(i): value(depth+1) for i in range(rng.randrange(4))}
            def equal(a, b):
                if type(a) is not type(b): return False
                if isinstance(a, dict): return a.keys() == b.keys() and all(equal(a[k], b[k]) for k in a)
                if isinstance(a, list): return len(a) == len(b) and all(equal(x, y) for x, y in zip(a, b))
                return a == b
            for _ in range(80):
                a = value(); b = a if rng.randrange(2) else value()
                body = check("json_diff", {"left": json.dumps(a), "right": json.dumps(b)})
                assert body["equal"] == equal(a, b), (a, b, body)
            # Pure tools work with mutation/egress disabled; the gate still denies shell.
            denied, body = m.tool("bash", command="printf should-not-run")
            assert denied and ("denied" in str(body).lower() or "capability" in str(body).lower()), body
            passed += 1
        finally: m.close()
    return {"binary": binary, "checks_passed": passed, "result": "PASS", "inference_calls": 0}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("--binary", default="./dsco"); args = parser.parse_args()
    print(json.dumps(run(str(pathlib.Path(args.binary).resolve())), indent=2))
