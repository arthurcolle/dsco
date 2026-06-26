#!/usr/bin/env python3
"""Index C preprocessor constants and environment variables.

Outputs a machine-readable JSON index plus a human-readable Markdown report.
This is intentionally conservative: it records every uppercase #define it can parse,
then attaches env-var mappings when the relationship is explicit or high-confidence.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from collections import defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable, Optional

RE_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)(.*)$")
RE_ENV_CALL = re.compile(
    r"\b(getenv|secure_getenv|setenv|unsetenv|provider_getenv_nonempty|sealed_store_get|sealed_store_peek|sealed_getenv|require_key|oai_env|dsco_env_bool|dsco_env_int|dsco_env_long|dsco_env_size|dsco_env_double|dsco_env_str)\s*\(\s*\"([A-Z_][A-Z0-9_]*)\""
)
RE_UPPER_STRING = re.compile(r'"([A-Z][A-Z0-9_]*(?:_[A-Z0-9]+)+)"')
RE_ENVISH = re.compile(
    r"^(DSCO_|ANTHROPIC_|OPENAI_|OPENROUTER_|OPEN_ROUTER_|CLAUDE_|CHATGPT_|GOOGLE_|GEMINI_|GROQ_|DEEPSEEK_|XAI_|X_AI_|GROK_|MISTRAL_|TOGETHER_|PERPLEXITY_|CEREBRAS_|COHERE_|MOONSHOT|KIMI_|KALSHI_|POLY|JINA_|TAVILY_|BRAVE_|FRED_|SERPAPI_|TWILIO_|ELEVENLABS_|PINECONE_|STRIPE_|SUPABASE_|MAPBOX_|TOOLS_|TOOL_MANAGEMENT_|GRAPHSUB_)|.*(_API_KEY|_TOKEN|_AUTH_TOKEN|_PASSPHRASE|_SECRET|_BASE_URL|_API_BASE|_URL|_HOST|_PORT|_PATH|_TIMEOUT|_RETRIES|_DEBUG|_DISABLE|_ENABLE|_BUDGET|_MODEL|_KEY)$"
)
RE_DYNAMIC_SUFFIX = re.compile(r'"(_API_KEY|_ACCESS_TOKEN|_AUTH_TOKEN|_TOKEN|_API_BASE|_BASE_URL)"')
RE_ENV_NAME = re.compile(r"\b[A-Z][A-Z0-9_]*(?:_[A-Z0-9]+)+\b")

SENSITIVE_TOKENS = ("KEY", "TOKEN", "SECRET", "PASSPHRASE", "PASSWORD", "PRIVATE")
STOP_TOKENS = {"DSCO", "ENV", "VAR", "VARS", "DEFAULT", "CONFIG", "VALUE", "VALUES"}

@dataclass
class Constant:
    name: str
    file: str
    line: int
    value: str
    function_like: bool
    category: str
    comment: str
    proposed_env: str | None
    env_status: str
    mapped_env: list[dict]

@dataclass
class EnvVar:
    name: str
    file: str
    line: int
    source: str
    operation: str
    sensitive: bool
    mapped_constants: list[dict]


def repo_files(root: Path) -> Iterable[Path]:
    candidates = [root / "include", root / "src"]
    for base in candidates:
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if p.suffix in {".c", ".h"} and p.is_file():
                yield p
    dsc = root / "dsc.c"
    if dsc.exists():
        yield dsc


def rel(root: Path, p: Path) -> str:
    return str(p.relative_to(root))


def read_lines(p: Path) -> list[str]:
    try:
        return p.read_text(errors="ignore").splitlines()
    except UnicodeDecodeError:
        return []


def clean_define_tail(tail: str) -> tuple[bool, str]:
    function_like = tail.startswith("(")
    if function_like:
        depth = 0
        end = 0
        for i, ch in enumerate(tail):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        value = tail[end:].strip()
    else:
        value = tail.strip()
    return function_like, value


def gather_multiline_value(lines: list[str], idx: int, first_value: str) -> str:
    parts = [first_value.rstrip("\\").rstrip()]
    j = idx
    while j < len(lines) and lines[j].rstrip().endswith("\\"):
        j += 1
        if j >= len(lines):
            break
        parts.append(lines[j].rstrip("\\").rstrip())
    return " ".join(p for p in parts if p).strip()


def nearby_comment(lines: list[str], idx: int) -> str:
    """Return nearby prose comments only, not the raw #define line.

    Inline comments are retained, but the macro name/value before the comment is
    stripped so DSCO_* constants do not get misclassified as env vars merely
    because the define line contains their name.
    """
    chunks: list[str] = []
    for j in range(max(0, idx - 4), idx):
        s = lines[j].strip()
        if s.startswith("#"):
            continue
        if s.startswith("/*") or s.startswith("*") or s.startswith("//") or s.endswith("*/"):
            chunks.append(s)
    s = lines[idx]
    if "//" in s:
        chunks.append(s[s.index("//"):].strip())
    elif "/*" in s:
        chunks.append(s[s.index("/*"):].strip())
    for j in range(idx + 1, min(len(lines), idx + 3)):
        s = lines[j].strip()
        if s.startswith("#"):
            continue
        if s.startswith("/*") or s.startswith("*") or s.startswith("//") or s.endswith("*/"):
            chunks.append(s)
        elif s:
            break
    return " ".join(chunks)


def proposed_env_for_constant(name: str, category: str) -> str | None:
    if category in {"include_guard", "macro_function", "prompt_or_blob", "capability_mask", "bit_flag_or_magic"}:
        return None
    if name in {"BUILD_DATE", "GIT_HASH"}:
        return None
    if name.startswith("DSCO_"):
        return name
    return f"DSCO_{name}"


def classify_constant(name: str, value: str, function_like: bool, file: str, line: int) -> str:
    if function_like:
        return "macro_function"
    if (name.endswith("_H") or name.endswith("_H_")) and not value and line <= 20:
        return "include_guard"
    if name.startswith("CAP_") or name.endswith("_CAP") or "CAPABILITY" in name:
        return "capability_mask"
    if "PROMPT" in name or len(value) > 512:
        return "prompt_or_blob"
    if any(x in name for x in ("URL", "URI", "DIR", "PATH", "FILE", "MODEL", "VERSION", "LABEL")) or value.startswith('"'):
        return "string_default"
    if "<<" in value or re.search(r"\b0x[0-9A-Fa-f]+", value):
        return "bit_flag_or_magic"
    if re.fullmatch(r"[-+]?\d+(?:\.\d+)?[uUlLfF]*", value.strip()):
        return "numeric_literal"
    if re.search(r"\d", value) and any(op in value for op in ("*", "+", "-", "/", "(", ")")):
        return "numeric_expression"
    return "macro_constant"


def extract_constants(root: Path, files: list[Path]) -> list[Constant]:
    out: list[Constant] = []
    for p in files:
        lines = read_lines(p)
        r = rel(root, p)
        for i, line in enumerate(lines):
            m = RE_DEFINE.match(line)
            if not m:
                continue
            name, tail = m.group(1), m.group(2)
            function_like, value0 = clean_define_tail(tail)
            value = gather_multiline_value(lines, i, value0)
            comment = nearby_comment(lines, i)
            category = classify_constant(name, value, function_like, r, i + 1)
            out.append(Constant(
                name=name,
                file=r,
                line=i + 1,
                value=value,
                function_like=function_like,
                category=category,
                comment=comment,
                proposed_env=proposed_env_for_constant(name, category),
                env_status="unknown",
                mapped_env=[],
            ))
    return out


def inside_env_vars_block(lines: list[str], start: int) -> int:
    # Returns end index if this line starts a .env_vars initializer, else -1.
    if ".env_vars" not in lines[start]:
        return -1
    depth = lines[start].count("{") - lines[start].count("}")
    j = start
    while j + 1 < len(lines) and depth > 0:
        j += 1
        depth += lines[j].count("{") - lines[j].count("}")
    return j


def extract_env_vars(root: Path, files: list[Path]) -> list[EnvVar]:
    envs: list[EnvVar] = []
    seen_occ = set()

    def add(name: str, file: str, line: int, source: str, op: str):
        if not name or not RE_ENVISH.match(name):
            return
        key = (name, file, line, source, op)
        if key in seen_occ:
            return
        seen_occ.add(key)
        envs.append(EnvVar(
            name=name,
            file=file,
            line=line,
            source=source,
            operation=op,
            sensitive=any(tok in name for tok in SENSITIVE_TOKENS),
            mapped_constants=[],
        ))

    for p in files:
        lines = read_lines(p)
        r = rel(root, p)
        i = 0
        while i < len(lines):
            line = lines[i]
            for m in RE_ENV_CALL.finditer(line):
                add(m.group(2), r, i + 1, "env_call", m.group(1))

            end = inside_env_vars_block(lines, i)
            if end >= i:
                blob = "\n".join(lines[i:end + 1])
                for s in RE_UPPER_STRING.findall(blob):
                    add(s, r, i + 1, "provider_profile_env_vars", "profile")
                i = end + 1
                continue

            # Other env-ish string literals in provider endpoint tables, aliases, diagnostics, etc.
            if any(marker in line for marker in ("API_KEY", "TOKEN", "PASSPHRASE", "DSCO_", "BASE_URL", "AUTH_TOKEN", "PRIVATE_KEY", "_HOST", "_PORT", "_PATH")):
                for s in RE_UPPER_STRING.findall(line):
                    add(s, r, i + 1, "envish_string_literal", "string")

            i += 1

        # Dynamic provider env-name suffixes: records as patterns, line-specific.
        for i, line in enumerate(lines):
            if "provider_build_env_name" in line or "suffixes" in line:
                for suf in RE_DYNAMIC_SUFFIX.findall(line):
                    add(f"<PROVIDER>{suf}", r, i + 1, "dynamic_env_pattern", "dynamic")

    return envs


def tokens(s: str) -> list[str]:
    return [t for t in re.split(r"_+", s.upper()) if t and t not in STOP_TOKENS]


def similarity(const_name: str, env_name: str) -> float:
    c = tokens(const_name)
    e = [t for t in tokens(env_name.replace("<PROVIDER>", "PROVIDER")) if t != "PROVIDER"]
    if not c or not e:
        return 0.0
    cs, es = set(c), set(e)
    # Favor constants whose meaningful tokens are covered by the env var.
    coverage = len(cs & es) / len(cs)
    jaccard = len(cs & es) / len(cs | es)
    return max(coverage, jaccard)


def envs_in_text(text: str, env_names: set[str]) -> list[str]:
    found = []
    for cand in RE_ENV_NAME.findall(text or ""):
        if cand in env_names or cand.startswith("DSCO_"):
            found.append(cand)
    return sorted(set(found))


def add_mapping(c: Constant, env: EnvVar, confidence: str, reason: str, score: float = 1.0):
    m = {"env": env.name, "confidence": confidence, "reason": reason, "score": round(score, 3), "file": env.file, "line": env.line}
    if all(x["env"] != env.name or x["reason"] != reason for x in c.mapped_env):
        c.mapped_env.append(m)
    inv = {"constant": c.name, "confidence": confidence, "reason": reason, "score": round(score, 3), "file": c.file, "line": c.line}
    if all(x["constant"] != c.name or x["reason"] != reason for x in env.mapped_constants):
        env.mapped_constants.append(inv)


def map_constants_to_env(constants: list[Constant], envs: list[EnvVar]):
    by_name: dict[str, list[EnvVar]] = defaultdict(list)
    for e in envs:
        by_name[e.name].append(e)
    env_names = set(by_name)

    # Explicit comments: only phrases that document an override/config surface.
    # Avoid treating examples like "e.g. ANTHROPIC_API_KEY" as constant mappings.
    for c in constants:
        cl = (c.comment or "").lower()
        if not any(phrase in cl for phrase in ("override", "overridable", "env var", "environment variable", "config:")):
            continue
        for name in envs_in_text(c.comment, env_names):
            if c.proposed_env and name != c.proposed_env and similarity(c.name, name) < 0.67:
                continue
            for e in by_name.get(name, [EnvVar(name, c.file, c.line, "comment_only", "documented", False, [])]):
                add_mapping(c, e, "explicit", "env var named in override/config comment", 1.0)

    # Direct name transforms. Only map to environment variables that are actually
    # read/written or known provider profile fields; ignore env-ish string literals
    # because a constant named DSCO_FOO often appears as a C symbol, not an env var.
    direct_sources = {"env_call", "provider_profile_env_vars", "dynamic_env_pattern"}
    for c in constants:
        candidates = [c.name, f"DSCO_{c.name}"]
        # Common numeric limit spelling: MAX_FOO -> DSCO_MAX_FOO, MIN_FOO -> DSCO_MIN_FOO already covered.
        for name in candidates:
            for e in by_name.get(name, []):
                if e.source in direct_sources:
                    add_mapping(c, e, "direct", "env name equals constant or DSCO_<constant>", 1.0)
        for e in envs:
            if e.source in direct_sources and e.name.endswith("_" + c.name):
                add_mapping(c, e, "direct", "env name suffix equals constant", 0.95)

    # Same-file proximity + token overlap. This catches SESSION_MAX_USD -> DSCO_TRADING_SESSION_MAX.
    by_file_envs: dict[str, list[EnvVar]] = defaultdict(list)
    for e in envs:
        by_file_envs[e.file].append(e)
    for c in constants:
        if c.category in {"include_guard", "macro_function", "prompt_or_blob"}:
            continue
        for e in by_file_envs.get(c.file, []):
            if e.source not in {"env_call", "provider_profile_env_vars", "dynamic_env_pattern"}:
                continue
            dist = abs(c.line - e.line)
            if dist > 140:
                continue
            score = similarity(c.name, e.name)
            if score >= 0.67:
                conf = "high" if dist <= 80 else "medium"
                add_mapping(c, e, conf, f"same-file proximity ({dist} lines) + token overlap", score)

    # Annotate whether each constant already has a runtime env override.
    for c in constants:
        if c.mapped_env:
            c.env_status = "mapped"
        elif c.proposed_env:
            c.env_status = "proposed"
        else:
            c.env_status = "none"

    # Sort mappings by confidence/score.
    rank = {"explicit": 0, "direct": 1, "high": 2, "medium": 3, "low": 4}
    for c in constants:
        c.mapped_env.sort(key=lambda m: (rank.get(m["confidence"], 9), -m["score"], m["env"]))
    for e in envs:
        e.mapped_constants.sort(key=lambda m: (rank.get(m["confidence"], 9), -m["score"], m["constant"]))


def summarize(constants: list[Constant], envs: list[EnvVar]) -> dict:
    by_category = defaultdict(int)
    by_file_constants = defaultdict(int)
    by_file_envs = defaultdict(int)
    for c in constants:
        by_category[c.category] += 1
        by_file_constants[c.file] += 1
    for e in envs:
        by_file_envs[e.file] += 1
    unique_envs = sorted({e.name for e in envs})
    mapped_constants = [c for c in constants if c.mapped_env]
    proposed_constants = [c for c in constants if c.env_status == "proposed"]
    mapped_env_names = sorted({m["env"] for c in mapped_constants for m in c.mapped_env})
    return {
        "constant_count": len(constants),
        "mapped_constant_count": len(mapped_constants),
        "proposed_env_constant_count": len(proposed_constants),
        "unmapped_constant_count": len(constants) - len(mapped_constants),
        "env_occurrence_count": len(envs),
        "unique_env_var_count": len(unique_envs),
        "mapped_unique_env_var_count": len(mapped_env_names),
        "unmapped_unique_env_var_count": len(set(unique_envs) - set(mapped_env_names)),
        "constant_categories": dict(sorted(by_category.items())),
        "top_constant_files": dict(sorted(by_file_constants.items(), key=lambda kv: kv[1], reverse=True)[:25]),
        "top_env_files": dict(sorted(by_file_envs.items(), key=lambda kv: kv[1], reverse=True)[:25]),
    }


def write_json(path: Path, constants: list[Constant], envs: list[EnvVar], summary: dict):
    payload = {
        "schema": "dsco.constants_env_index.v1",
        "summary": summary,
        "constants": [asdict(c) for c in constants],
        "env_vars": [asdict(e) for e in envs],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def md_escape(s: str) -> str:
    return (s or "").replace("|", "\\|").replace("\n", " ")


def write_markdown(path: Path, constants: list[Constant], envs: list[EnvVar], summary: dict):
    unique_envs = {}
    for e in envs:
        unique_envs.setdefault(e.name, e)
    mapped_constants = [c for c in constants if c.mapped_env]
    unmapped_runtime = [c for c in constants if not c.mapped_env and c.category not in {"include_guard", "macro_function", "prompt_or_blob", "capability_mask", "bit_flag_or_magic"}]
    mapped_env_names = {m["env"] for c in mapped_constants for m in c.mapped_env}
    unmapped_envs = [e for name, e in sorted(unique_envs.items()) if name not in mapped_env_names and not name.startswith("<PROVIDER>")]

    lines: list[str] = []
    lines.append("# Constants ↔ Environment Variable Index")
    lines.append("")
    lines.append("Generated by `scripts/index_constants_env.py`. Do not hand-edit generated counts; rerun the script after changing C constants or env vars.")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append("| Metric | Count |")
    lines.append("|---|---:|")
    for key in ["constant_count", "mapped_constant_count", "proposed_env_constant_count", "unmapped_constant_count", "env_occurrence_count", "unique_env_var_count", "mapped_unique_env_var_count", "unmapped_unique_env_var_count"]:
        lines.append(f"| `{key}` | {summary[key]} |")
    lines.append("")

    lines.append("## Constant categories")
    lines.append("")
    lines.append("| Category | Count |")
    lines.append("|---|---:|")
    for k, v in summary["constant_categories"].items():
        lines.append(f"| `{k}` | {v} |")
    lines.append("")

    lines.append("## Mapped constants")
    lines.append("")
    lines.append("| Constant | Env var(s) | Confidence | Definition | Default/value |")
    lines.append("|---|---|---|---|---|")
    for c in sorted(mapped_constants, key=lambda x: (x.file, x.line, x.name)):
        env_list = ", ".join(f"`{m['env']}`" for m in c.mapped_env[:6])
        conf = ", ".join(f"{m['confidence']}:{m['score']}" for m in c.mapped_env[:3])
        value = c.value[:120] + ("…" if len(c.value) > 120 else "")
        lines.append(f"| `{c.name}` | {env_list} | {conf} | `{c.file}:{c.line}` | `{md_escape(value)}` |")
    lines.append("")

    lines.append("## Env vars without mapped constants")
    lines.append("")
    lines.append("These are usually credentials, provider profile variables, terminal variables, or pure runtime toggles with no named `#define` default.")
    lines.append("")
    lines.append("| Env var | Sensitive | First occurrence | Source |")
    lines.append("|---|---:|---|---|")
    for e in unmapped_envs:
        lines.append(f"| `{e.name}` | {str(e.sensitive).lower()} | `{e.file}:{e.line}` | `{e.source}/{e.operation}` |")
    lines.append("")

    lines.append("## Runtime constants without env override")
    lines.append("")
    lines.append("Excludes include guards, function-like macros, large prompt/blob macros, capability masks, and bit flags. `Proposed env` is the canonical env-var name to use if we decide the constant should become runtime-configurable.")
    lines.append("")
    lines.append("| Constant | Proposed env | Category | Definition | Value |")
    lines.append("|---|---|---|---|---|")
    for c in sorted(unmapped_runtime, key=lambda x: (x.file, x.line, x.name)):
        value = c.value[:120] + ("…" if len(c.value) > 120 else "")
        proposed = f"`{c.proposed_env}`" if c.proposed_env else "—"
        lines.append(f"| `{c.name}` | {proposed} | `{c.category}` | `{c.file}:{c.line}` | `{md_escape(value)}` |")
    lines.append("")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".", help="repo root")
    ap.add_argument("--json", default="data/constants_env_index.json", help="JSON output")
    ap.add_argument("--md", default="docs/CONSTANTS_ENV_INDEX.md", help="Markdown output")
    ap.add_argument("--check", action="store_true", help="verify generated outputs are current without rewriting them")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    files = sorted(set(repo_files(root)))
    constants = extract_constants(root, files)
    envs = extract_env_vars(root, files)
    map_constants_to_env(constants, envs)
    summary = summarize(constants, envs)

    json_path = root / args.json
    md_path = root / args.md
    if args.check:
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            tmp_json = Path(td) / "constants_env_index.json"
            tmp_md = Path(td) / "CONSTANTS_ENV_INDEX.md"
            write_json(tmp_json, constants, envs, summary)
            write_markdown(tmp_md, constants, envs, summary)
            drift = []
            if not json_path.exists() or json_path.read_text(errors="ignore") != tmp_json.read_text(errors="ignore"):
                drift.append(str(json_path.relative_to(root)))
            if not md_path.exists() or md_path.read_text(errors="ignore") != tmp_md.read_text(errors="ignore"):
                drift.append(str(md_path.relative_to(root)))
            if drift:
                print(
                    "docs drift: constants/env index is out of date. Run "
                    "python3 scripts/index_constants_env.py --root .",
                    file=os.sys.stderr,
                )
                for p in drift:
                    print(f"  - {p}", file=os.sys.stderr)
                return 1
        return 0

    write_json(json_path, constants, envs, summary)
    write_markdown(md_path, constants, envs, summary)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
