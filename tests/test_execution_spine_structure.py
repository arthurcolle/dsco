#!/usr/bin/env python3
"""Guard the single production leaf-dispatch choke point."""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
CALL = re.compile(r"\btools_execute_internal\s*\(")


def scrub_c(source: str) -> str:
    """Remove comments and literal bodies while preserving byte positions."""
    out = list(source)
    i = 0
    state = "code"
    quote = ""
    while i < len(source):
        if state == "code":
            if source.startswith("//", i):
                out[i] = out[i + 1] = " "
                i += 2
                state = "line"
                continue
            if source.startswith("/*", i):
                out[i] = out[i + 1] = " "
                i += 2
                state = "block"
                continue
            if source[i] in {'"', "'"}:
                quote = source[i]
                out[i] = " "
                i += 1
                state = "literal"
                continue
        elif state == "line":
            if source[i] == "\n":
                state = "code"
            else:
                out[i] = " "
        elif state == "block":
            if source.startswith("*/", i):
                out[i] = out[i + 1] = " "
                i += 2
                state = "code"
                continue
            if source[i] != "\n":
                out[i] = " "
        else:
            if source[i] == "\\" and i + 1 < len(source):
                if source[i] != "\n":
                    out[i] = " "
                if source[i + 1] != "\n":
                    out[i + 1] = " "
                i += 2
                continue
            if source[i] == quote:
                out[i] = " "
                state = "code"
            elif source[i] != "\n":
                out[i] = " "
        i += 1
    assert state in {"code", "line"}, f"unterminated C token state: {state}"
    return "".join(out)


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    tools_path = ROOT / "src" / "tools.c"
    raw = tools_path.read_text()
    clean = scrub_c(raw)

    for path in (ROOT / "src").glob("*.c"):
        if path == tools_path:
            continue
        assert not CALL.search(scrub_c(path.read_text())), (
            f"production leaf dispatcher escaped tools.c: {path}"
        )

    occurrences = list(CALL.finditer(clean))
    assert len(occurrences) == 3, (
        "tools_execute_internal must have one definition, one test-only call, "
        f"and one production call; found {len(occurrences)}"
    )
    assert "static bool tools_execute_internal" in clean

    test_call = occurrences[1].start()
    test_begin = clean.rfind("#ifdef DSCO_INTERNAL_TESTS", 0, test_call)
    test_end = clean.index("#endif", test_call)
    assert test_begin < occurrences[1].start() < test_end, (
        "the raw dispatcher adapter must remain test-only"
    )

    gated = function_body(clean, "bool tools_execute_for_tier(")
    assert len(CALL.findall(gated)) == 1, "the gate must own the only production leaf call"
    begin = gated.index("execution_kernel_begin")
    floor = gated.index("tool_capability_floor")
    admit = gated.index("execution_kernel_admit")
    start = gated.index("execution_kernel_start")
    leaf = gated.index("tools_execute_internal")
    finish = gated.index("execution_kernel_finish")
    returns = list(re.finditer(r"\breturn\b", gated))
    assert begin < floor < admit < start < leaf
    assert finish < returns[0].start() and len(returns) == 1, (
        "every gate return must close the retained execution attempt"
    )

    public = function_body(clean, "bool tools_execute(")
    assert "return tools_execute_for_tier" in public

    print("PASS: execution spine is the single production tool-leaf choke point")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
