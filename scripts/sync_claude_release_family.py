#!/usr/bin/env python3
"""Generate or verify Claude release constants from DSCO's release contract."""

from __future__ import annotations

import argparse
import difflib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "data/claude_release_family.json"
DEFAULT_HEADER = ROOT / "include/claude_models.h"


def load_manifest(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema") != "dsco.claude_release_family.v1":
        raise ValueError(f"unsupported Claude release schema in {path}")
    if set(value.get("releases", {})) != {"fable", "opus", "sonnet", "haiku"}:
        raise ValueError("Claude release contract must define fable, opus, sonnet, and haiku")
    return value


def render_header(contract: dict) -> str:
    releases = contract["releases"]
    lines = [
        "#ifndef DSCO_CLAUDE_MODELS_H",
        "#define DSCO_CLAUDE_MODELS_H",
        "",
        "/* Generated from data/claude_release_family.json. Do not edit by hand.",
        " * Catalog discovery does not promote a release; the qualification gate does. */",
    ]
    for role, release in releases.items():
        lines.append(f'#define DSCO_CLAUDE_{role.upper()}_MODEL "{release["native_id"]}"')
    lines.append("")
    for role, release in releases.items():
        lines.append(f'#define DSCO_OR_CLAUDE_{role.upper()}_MODEL "{release["openrouter_id"]}"')
    lines.append("")
    for role, candidate in contract.get("candidates", {}).items():
        lines.append(
            f'#define DSCO_CLAUDE_{role.upper()}_CANDIDATE_MODEL "{candidate["native_id"]}"'
        )
    lines.extend(["", "#endif", ""])
    return "\n".join(lines)


def py_string(value: str) -> str:
    return json.dumps(value)


def render_python(contract: dict) -> str:
    releases = contract["releases"]
    lines = [
        '"""Generated Claude release family. Source: dsco-cli/data/claude_release_family.json."""',
        "",
        "from __future__ import annotations",
        "",
        "from dataclasses import dataclass",
        "",
        f'QUALIFIED_AT = {py_string(contract["qualified_at"])}',
        "",
        "",
        "@dataclass(frozen=True, slots=True)",
        "class ClaudeRelease:",
        "    native_id: str",
        "    openrouter_id: str",
        "    context_window: int",
        "    max_tokens: int",
        "    cache_min_tokens: int",
        "    input_per_mtok: float",
        "    output_per_mtok: float",
        "    cache_read_per_mtok: float",
        "    cache_write_per_mtok: float",
        "    default_effort: str",
        "    allowed_efforts: tuple[str, ...]",
        "",
        "    @property",
        "    def dspy_openrouter_id(self) -> str:",
        '        return f"openrouter/{self.openrouter_id}"',
        "",
    ]
    fields = (
        "native_id", "openrouter_id", "context_window", "max_tokens", "cache_min_tokens",
        "input_per_mtok", "output_per_mtok", "cache_read_per_mtok", "cache_write_per_mtok",
        "default_effort", "allowed_efforts",
    )
    for role, release in releases.items():
        lines.extend(["", f"CLAUDE_{role.upper()} = ClaudeRelease("])
        for field in fields:
            value = release[field]
            if isinstance(value, str):
                rendered = py_string(value)
            elif isinstance(value, list):
                rendered = repr(tuple(value))
            else:
                rendered = repr(value)
            lines.append(f"    {field}={rendered},")
        lines.append(")")
    lines.extend(["", "", "PROMOTED_CLAUDE_RELEASES: dict[str, ClaudeRelease] = {"])
    for role in releases:
        lines.append(f'    "{role}": CLAUDE_{role.upper()},')
    lines.extend(["}", "", "DISCOVERED_UNQUALIFIED_CLAUDE: dict[str, str] = {"])
    for candidate in contract.get("candidates", {}).values():
        lines.append(f'    {py_string(candidate["native_id"])}: {py_string(candidate["reason"])},')
    lines.extend(["}", ""])
    return "\n".join(lines)


def check(path: Path, expected: str) -> bool:
    actual = path.read_text(encoding="utf-8") if path.exists() else ""
    if actual == expected:
        print(f"ok: {path}")
        return True
    print(f"drift: {path}")
    print("".join(difflib.unified_diff(
        actual.splitlines(keepends=True), expected.splitlines(keepends=True),
        fromfile=str(path), tofile=f"{path} (generated)",
    )))
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--header-output", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--python-output", type=Path)
    parser.add_argument("--write", action="store_true", help="write generated outputs; default verifies")
    args = parser.parse_args()

    contract = load_manifest(args.manifest)
    outputs = [(args.header_output, render_header(contract))]
    if args.python_output:
        outputs.append((args.python_output, render_python(contract)))
    if args.write:
        for path, content in outputs:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            print(f"wrote: {path}")
        return 0
    return 0 if all(check(path, content) for path, content in outputs) else 1


if __name__ == "__main__":
    raise SystemExit(main())
