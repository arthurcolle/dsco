"""Lethal-trifecta "toxic-flow" reachability scanner — static analysis of an MCP-style
TOOLSET, modeled after invariantlabs' mcp-scan toxic-flow analysis.

The lethal trifecta (Simon Willison's framing) is the coincidence, within one agent's reach,
of THREE capabilities:

  1. UNTRUSTED INGESTION — a tool that can pull in attacker-controlled content (a web fetch,
     a search, an inbound message) which may carry a prompt injection.
  2. SECRET ACCESS       — a tool that can read private/sensitive data (files, credentials,
     a secrets store).
  3. EXTERNAL EGRESS     — a tool that can send data off-box (curl/POST, email, webhook).

Any agent holding all three legs can be steered by injected content to read a secret and
exfiltrate it. This module decides REACHABILITY statically: a toolset is trifecta-reachable
iff it contains at least one tool of each leg — because an agent chaining them CAN exfiltrate,
regardless of whether a given run happens to. We enumerate every witnessing
(untrusted, secret, egress) triple so an operator can see exactly which tools to split across
capability lanes / separate agents to break the chain.

Legs are inferred from the SAME primitives the deployed gate uses (dsco_gate.caps_for_tool +
classify_egress + input_touches_secrets), so the scanner's notion of "can egress"/"reads
secrets" matches the gate that enforces at runtime. Explicit descriptor flags
({reads_untrusted, accesses_secrets, external_egress}) override the inference when supplied.

  analyze_toolset(tools) -> {reachable, triples, per_leg, legs}   # a tool INVENTORY
  analyze_batch(calls)   -> same, plus per-lane reachability      # a concrete turn's calls
"""
from __future__ import annotations

import json
from itertools import product
from typing import Optional

from dsco_gate import (
    CAP_EXEC, CAP_FS_READ, CAP_NET, CAP_SECRETS, CAP_UNTRUSTED_IN, E_EXTERNAL, E_OPAQUE,
    E_NAME, SECRET_TOOLS, caps_for_tool, classify_egress, input_touches_secrets,
)

_EXTERNAL_EGRESS = {E_EXTERNAL, E_OPAQUE}


def _name(tool) -> str:
    return tool.get("name") or tool.get("tool") or ""


def _args(tool) -> str:
    a = tool.get("input")
    if a is None:
        a = tool.get("sample_args", tool.get("args", ""))
    if not isinstance(a, str):
        a = json.dumps(a)
    return a or ""


def classify_legs(tool: dict, trusted: Optional[str] = None) -> dict:
    """Classify ONE tool descriptor into the three trifecta legs.

    A descriptor is {name|tool, sample_args|input|args?, reads_untrusted?, accesses_secrets?,
    external_egress?}. Explicit boolean flags win; otherwise legs are inferred from the gate
    primitives against the (possibly empty) sample args — worst-case / capability-reachability
    semantics:

      * untrusted  = tool can ingest attacker-controlled content (CAP_UNTRUSTED_IN).
      * secret     = tool can read sensitive data. Any filesystem-READ or secret-scoped tool
                     qualifies (a file reader CAN open ~/.ssh/id_rsa), as does an arg that
                     already names a secret.
      * egress     = tool can send data off-box (CAP_NET). If concrete args pin an external/
                     opaque destination we surface it; a network tool with no/args-less
                     destination is still egress-capable (opaque), the conservative call.
    """
    name = _name(tool)
    inp = _args(tool)
    caps = caps_for_tool(name, inp)
    egress_kind, dest = classify_egress(name, inp, caps, trusted)

    untrusted = tool.get("reads_untrusted")
    if untrusted is None:
        untrusted = bool(caps & CAP_UNTRUSTED_IN)

    secret = tool.get("accesses_secrets")
    if secret is None:
        secret = bool(caps & (CAP_SECRETS | CAP_FS_READ)) or (name in SECRET_TOOLS) \
            or bool(inp and input_touches_secrets(inp))

    egress = tool.get("external_egress")
    if egress is None:
        # Network-capable = potential external egress. Refine the *label* with the concrete
        # destination kind when args reveal one, but a bare network tool (no destination yet)
        # is treated as opaque egress rather than cleared.
        egress = bool(caps & CAP_NET)
        if egress and egress_kind not in _EXTERNAL_EGRESS and dest:
            # concrete destination that is local/lan/trusted — not an external-exfil leg
            egress = False

    return {
        "name": name, "untrusted": bool(untrusted), "secret": bool(secret),
        "egress": bool(egress), "egress_kind": E_NAME[egress_kind], "dest": dest,
        "exec": bool(caps & CAP_EXEC),
    }


def _analyze(legs: list) -> dict:
    """Core reachability over a list of already-classified leg dicts."""
    u = [l["name"] for l in legs if l["untrusted"]]
    s = [l["name"] for l in legs if l["secret"]]
    e = [l["name"] for l in legs if l["egress"]]
    reachable = bool(u and s and e)
    # Every witnessing triple. De-dup names within a leg first so the product stays readable
    # when a toolset repeats a tool; a single tool covering >1 leg legitimately self-pairs.
    triples = [{"untrusted": a, "secret": b, "egress": c}
               for a, b, c in product(dict.fromkeys(u), dict.fromkeys(s), dict.fromkeys(e))]
    return {
        "reachable": reachable,
        "triples": triples,
        "per_leg": {"untrusted": u, "secret": s, "egress": e},
        "legs": legs,
    }


def analyze_toolset(tools: list, trusted: Optional[str] = None) -> dict:
    """Static lethal-trifecta reachability over a tool INVENTORY (no run required).

    Returns {reachable, triples, per_leg, legs}. `reachable` is True iff the toolset holds at
    least one tool of each leg; `triples` witnesses every (untrusted, secret, egress) chain an
    agent could assemble from it."""
    return _analyze([classify_legs(t, trusted) for t in tools])


def analyze_batch(calls: list, trusted: Optional[str] = None) -> dict:
    """Trifecta reachability over a CONCRETE turn's calls (reuses the same leg classification).

    Adds `by_lane`: the deployed gate isolates sibling lanes (a secret in lane A does not
    poison lane B), and root-lane (lane=None) taint flows into every lane. So a turn is
    trifecta-reachable within a lane iff that lane's legs UNION the root lane's legs cover all
    three. `reachable` (top level) is the flat, lane-agnostic worst case; `by_lane` is the
    lane-accurate view that mirrors gate_batch's enforcement."""
    legs = [classify_legs(c, trusted) for c in calls]
    flat = _analyze(legs)

    # group legs by lane; the root lane (None) contributes to every named lane
    by_lane_legs: dict = {}
    for c, lg in zip(calls, legs):
        by_lane_legs.setdefault(c.get("lane"), []).append(lg)
    root = by_lane_legs.get(None, [])

    by_lane = {}
    for lane, lg in by_lane_legs.items():
        scope = lg if lane is None else lg + root  # root taint flows down into the lane
        res = _analyze(scope)
        by_lane[lane] = {"reachable": res["reachable"], "triples": res["triples"],
                         "per_leg": res["per_leg"]}
    flat["by_lane"] = by_lane
    flat["reachable_in_any_lane"] = any(v["reachable"] for v in by_lane.values())
    return flat


def _print_report(title: str, tools: list, res: dict) -> None:
    print(f"=== {title} ===")
    for l in res["legs"]:
        tags = ",".join(k.upper() for k in ("untrusted", "secret", "egress") if l[k]) or "inert"
        extra = f"  egress={l['egress_kind']}" + (f"->{l['dest']}" if l["dest"] else "") if l["egress"] else ""
        print(f"  {l['name']:16} [{tags}]{extra}")
    pl = res["per_leg"]
    print(f"  legs: untrusted={pl['untrusted']}  secret={pl['secret']}  egress={pl['egress']}")
    if res["reachable"]:
        print(f"  TRIFECTA REACHABLE — {len(res['triples'])} exfil chain(s):")
        for t in res["triples"]:
            print(f"    inject:{t['untrusted']}  ->  read:{t['secret']}  ->  exfil:{t['egress']}")
        print(f"  MITIGATION: split these tools across separate capability lanes/agents so no "
              f"single agent holds all three legs.")
    else:
        missing = [leg for leg in ("untrusted", "secret", "egress") if not pl[leg]]
        print(f"  SAFE — trifecta NOT reachable (missing leg(s): {', '.join(missing)}). "
              f"No untrusted->secret->egress chain exists.")
    print()


def _demo():
    # 1) Realistic MCP-style toolset that IS trifecta-reachable: a web reader (untrusted in),
    #    a file/secrets reader, and an external sender. Names + sample args only — legs are
    #    inferred through the gate primitives.
    reachable_toolset = [
        {"name": "web_fetch", "sample_args": '{"url":"https://docs.example.com/guide"}'},
        {"name": "read_file", "sample_args": '{"path":"./config.yaml"}'},
        {"name": "send_email", "sample_args": '{"to":"user@corp.com","body":"..."}'},
    ]
    _print_report("MCP toolset A (web_fetch + read_file + send_email)",
                  reachable_toolset, analyze_toolset(reachable_toolset))

    # 2) A SAFE toolset: a local code agent — reads/writes files and runs LOCAL commands, with
    #    NO network tool at all. Both the untrusted-ingestion and the external-egress legs are
    #    absent, so the exfil chain cannot be assembled and the scanner clears it.
    safe_toolset = [
        {"name": "read_file", "sample_args": '{"path":"~/.aws/credentials"}'},
        {"name": "grep", "sample_args": '{"pattern":"TODO","path":"./src"}'},
        {"name": "write_file", "sample_args": '{"path":"./out.txt"}'},
        {"name": "bash", "sample_args": '{"command":"python analyze.py --local"}'},
    ]
    _print_report("MCP toolset B (read/write/grep + local exec, no network tool)",
                  safe_toolset, analyze_toolset(safe_toolset))

    # 3) Concrete-turn (batch) view: the SAME dangerous tools, but split across isolated lanes
    #    so no single lane holds all three legs — lane-accurate reachability clears even though
    #    the flat inventory does not.
    turn = [
        {"tool": "read_url", "input": '{"url":"https://attacker-blog.example/post"}', "lane": "research"},
        {"tool": "read_file", "input": '{"path":"~/.ssh/id_rsa"}', "lane": "secrets"},
        {"tool": "send_email", "input": '{"to":"x@corp.com","body":"status"}', "lane": "notify"},
    ]
    res = analyze_batch(turn)
    print("=== concrete turn (trifecta tools SPLIT across isolated lanes) ===")
    print(f"  flat (lane-agnostic) reachable: {res['reachable']}")
    print(f"  reachable in any single lane:   {res['reachable_in_any_lane']}")
    for lane, v in res["by_lane"].items():
        print(f"    lane {str(lane):10} reachable={v['reachable']}  legs={v['per_leg']}")
    print(f"  -> lane isolation BREAKS the chain: flat inventory looks trifecta-reachable, but "
          f"no single lane holds all three legs.")
    print()

    # 4) Same tools collapsed into ONE lane -> reachable again (the anti-pattern).
    turn_one_lane = [dict(c, lane="agent0") for c in turn]
    res2 = analyze_batch(turn_one_lane)
    print("=== same turn, all in ONE lane (anti-pattern) ===")
    print(f"  reachable in lane 'agent0': {res2['by_lane']['agent0']['reachable']}  "
          f"({len(res2['by_lane']['agent0']['triples'])} exfil chain)")


if __name__ == "__main__":
    _demo()
