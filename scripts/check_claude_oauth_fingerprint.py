#!/usr/bin/env python3
"""Diff dsco's Claude Code OAuth wire fingerprint against upstream oh-my-pi (OMP).

DSCO borrows a real Claude Code OAuth token and then hand-builds the /v1/messages
request wire bytes (User-Agent, anthropic-beta list, X-Stainless-* headers, the
billing-header system block, etc.) to match what the real Claude Code CLI sends.
Anthropic ships new Claude Code releases on no fixed schedule and each one can
change these literals; OMP (github.com/can1357/oh-my-pi) reverse-engineers them
continuously, so it is the reference this script diffs against.

Usage:
    scripts/check_claude_oauth_fingerprint.py [--omp-repo PATH]

Exit code is 1 if any tracked constant drifted or could not be extracted from
either side (a missing/renamed symbol means the parser is stale, not that
things match), 0 if everything lines up.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

DSCO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OMP_REPO = Path.home() / "oh-my-pi"

OMP_FINGERPRINT_PATH = "packages/ai/src/providers/claude-code-fingerprint.ts"
OMP_ANTHROPIC_PROVIDER_PATH = "packages/ai/src/providers/anthropic.ts"
OMP_ANTHROPIC_OAUTH_PATH = "packages/ai/src/registry/oauth/anthropic.ts"

DSCO_PROVIDER_H = DSCO_ROOT / "include" / "provider.h"
DSCO_LLM_C = DSCO_ROOT / "src" / "llm.c"
DSCO_PROVIDER_C = DSCO_ROOT / "src" / "provider.c"


class Drift(Exception):
    pass


def git_show(repo: Path, ref: str, path: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), "show", f"{ref}:{path}"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise Drift(f"could not read {path} @ {ref} from {repo}: {result.stderr.strip()}")
    return result.stdout


def git_fetch(repo: Path) -> None:
    result = subprocess.run(
        ["git", "-C", str(repo), "fetch", "origin", "--quiet"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise Drift(f"git fetch failed in {repo}: {result.stderr.strip()}")


def extract(pattern: str, text: str, label: str, flags=0) -> str:
    m = re.search(pattern, text, flags)
    if not m:
        raise Drift(f"could not find {label} (pattern stale? OMP source shape changed)")
    return m.group(1)


def extract_ts_array_tokens(name: str, text: str) -> list[str]:
    """Pull ordered element tokens out of `const NAME = [...] as const;`.

    Elements are either quoted string literals or bare identifiers referencing
    another `const X = "...";` — both appear in OMP's beta-default arrays, e.g.
    `["claude-code-20250219", oauthAuthBeta, "interleaved-thinking-2025-05-14"]`.
    Returned tokens are the literal (quotes stripped) or the identifier name,
    in source order; resolve identifiers with resolve_ts_const_refs().
    """
    m = re.search(rf"const\s+{re.escape(name)}\s*=\s*\[(.*?)\]\s*as const", text, re.S)
    if not m:
        raise Drift(f"could not find array {name}")
    tokens = []
    for raw in m.group(1).split(","):
        item = raw.strip()
        if not item:
            continue
        q = re.fullmatch(r'"([^"]*)"', item)
        tokens.append(q.group(1) if q else item)
    return tokens


def resolve_ts_const_refs(items: list[str], consts: dict[str, str]) -> list[str]:
    """Array literals mix bare strings and identifier references to `const X = "..."`."""
    resolved = []
    for item in items:
        if item in consts:
            resolved.append(consts[item])
        elif re.fullmatch(r"[A-Za-z_]\w*", item) and item not in consts:
            raise Drift(f"identifier {item!r} referenced in array but no `const {item} = \"...\"` found")
        else:
            resolved.append(item)
    return resolved


def c_define_str(text: str, name: str, path: Path) -> str:
    """Extract a (possibly line-continued) C string #define's concatenated literal.

    Several of these are `#ifdef DSCO_USE_OBF_SECRETS` / `#else` pairs where the
    obfuscated branch is `dsco_secret("NAME")` (a quoted string that happens to
    match \\"([^"]*)\\" too). Prefer whichever #define body is NOT a dsco_secret()
    call so we compare the real literal, not the secret-table key name.
    """
    bodies = [
        m.group(1)
        for m in re.finditer(rf'#define\s+{re.escape(name)}\b((?:[^\n]*\\\n)*[^\n]*)', text)
    ]
    if not bodies:
        raise Drift(f"could not find #define {name} in {path}")
    plain = [b for b in bodies if "dsco_secret(" not in b]
    body = plain[-1] if plain else bodies[-1]
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    if not parts:
        raise Drift(f"#define {name} in {path} has no string literal (secret-obfuscated build?)")
    return "".join(parts)


def c_call_return_default(text: str, func_name: str, path: Path) -> str:
    """Best-effort: last bare `return "literal";` inside a named function body."""
    m = re.search(rf"{re.escape(func_name)}\s*\([^)]*\)\s*\{{(.*?)\n\}}", text, re.S)
    if not m:
        raise Drift(f"could not find function {func_name} in {path}")
    body = m.group(1)
    returns = re.findall(r'return\s+"([^"]*)"\s*;', body)
    if not returns:
        raise Drift(f"no bare string return in {func_name} ({path})")
    return returns[-1]


def report_row(name: str, omp_val: str, dsco_val: str, rows: list[tuple[str, str, str, bool]]) -> None:
    rows.append((name, omp_val, dsco_val, omp_val == dsco_val))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--omp-repo", type=Path, default=DEFAULT_OMP_REPO,
                        help=f"path to a local oh-my-pi clone (default: {DEFAULT_OMP_REPO})")
    parser.add_argument("--no-fetch", action="store_true",
                        help="skip `git fetch origin` on the OMP repo (use whatever is local)")
    parser.add_argument("--ref", default="origin/main",
                        help="git ref to diff against in the OMP repo (default: origin/main)")
    args = parser.parse_args()

    if not args.omp_repo.exists():
        print(f"error: OMP repo not found at {args.omp_repo}", file=sys.stderr)
        print("clone github.com/can1357/oh-my-pi there, or pass --omp-repo", file=sys.stderr)
        return 2

    errors: list[str] = []
    rows: list[tuple[str, str, str, bool]] = []

    try:
        if not args.no_fetch:
            git_fetch(args.omp_repo)

        fp_ts = git_show(args.omp_repo, args.ref, OMP_FINGERPRINT_PATH)
        anthropic_ts = git_show(args.omp_repo, args.ref, OMP_ANTHROPIC_PROVIDER_PATH)
        oauth_ts = git_show(args.omp_repo, args.ref, OMP_ANTHROPIC_OAUTH_PATH)
    except Drift as e:
        print(f"error fetching OMP reference state: {e}", file=sys.stderr)
        return 2

    provider_h = DSCO_PROVIDER_H.read_text()
    llm_c = DSCO_LLM_C.read_text()
    provider_c = DSCO_PROVIDER_C.read_text()

    def check(label: str, omp_fn, dsco_fn) -> None:
        try:
            omp_val = omp_fn()
        except Drift as e:
            errors.append(f"[OMP] {label}: {e}")
            return
        try:
            dsco_val = dsco_fn()
        except Drift as e:
            errors.append(f"[DSCO] {label}: {e}")
            return
        report_row(label, omp_val, dsco_val, rows)

    # --- version fingerprint -------------------------------------------------
    check(
        "claudeCodeVersion",
        lambda: extract(r'claudeCodeVersion\s*=\s*"([^"]+)"', fp_ts, "claudeCodeVersion"),
        lambda: c_define_str(provider_h, "CLAUDE_CODE_OAUTH_COMPAT_VERSION", DSCO_PROVIDER_H),
    )
    check(
        "claudeCodeSdkVersion",
        lambda: extract(r'claudeCodeSdkVersion\s*=\s*"([^"]+)"', fp_ts, "claudeCodeSdkVersion"),
        lambda: c_define_str(provider_h, "CLAUDE_CODE_OAUTH_SDK_VERSION", DSCO_PROVIDER_H),
    )
    check(
        "claudeCodeSystemInstruction",
        lambda: extract(r'claudeCodeSystemInstruction\s*=\s*"([^"]+)"', fp_ts, "claudeCodeSystemInstruction"),
        lambda: c_define_str(llm_c, "CLAUDE_CODE_AGENT_INSTRUCTION", DSCO_LLM_C),
    )

    # --- entrypoint / user-agent suffix --------------------------------------
    check(
        "cc_entrypoint (billing header)",
        lambda: extract(r'cc_entrypoint=([a-zA-Z0-9_-]+);', anthropic_ts, "cc_entrypoint literal"),
        lambda: c_call_return_default(llm_c, "claude_code_entrypoint", DSCO_LLM_C),
    )
    check(
        "User-Agent suffix (claudeCodeUserAgent)",
        lambda: extract(r'claudeCodeUserAgent\s*=\s*`claude-cli/\$\{claudeCodeVersion\}\s*(\([^)]*\))`', fp_ts, "claudeCodeUserAgent suffix"),
        lambda: extract(r'User-Agent:\s*claude-cli/%s\s*(\([^)]*\))', llm_c, "dsco User-Agent suffix"),
    )

    # --- refresh token wire ---------------------------------------------------
    check(
        "OAuth token URL",
        lambda: extract(r'TOKEN_URL\s*=\s*"([^"]+)"', oauth_ts, "TOKEN_URL"),
        lambda: c_define_str(provider_h, "CLAUDE_CODE_OAUTH_TOKEN_URL", DSCO_PROVIDER_H),
    )
    check(
        "OAuth client_id",
        lambda: __import__("base64").b64decode(
            extract(r'decode\("([^"]+)"\)', oauth_ts, "base64 client id")
        ).decode(),
        lambda: c_define_str(provider_c, "CLAUDE_CODE_OAUTH_CLIENT_ID", DSCO_PROVIDER_C),
    )

    # --- anthropic-beta agent fingerprint -------------------------------------
    def omp_agent_betas() -> list[str]:
        simple_consts = dict(re.findall(r'const\s+(\w+)\s*=\s*"([^"]+)";', anthropic_ts))
        agent_defaults = resolve_ts_const_refs(
            extract_ts_array_tokens("claudeCodeAgentBetaDefaults", anthropic_ts), simple_consts
        )
        effort_beta = simple_consts.get("effortBeta")
        fallback_credit_beta = simple_consts.get("fallbackCreditBeta")
        if effort_beta is None or fallback_credit_beta is None:
            raise Drift("effortBeta / fallbackCreditBeta constants not found")
        # buildClaudeCodeBetas({agentRequest: true, thinkingRequest: true}):
        # agent defaults, then effort (thinking request), then fallback-credit always.
        return agent_defaults + [effort_beta, fallback_credit_beta]

    omp_beta_list: list[str] | None = None
    try:
        omp_beta_list = omp_agent_betas()
    except Drift as e:
        errors.append(f"[OMP] anthropic-beta agent fingerprint: {e}")

    dsco_beta_str: str | None = None
    try:
        dsco_beta_str = c_define_str(llm_c, "CLAUDE_CODE_OAUTH_BETAS", DSCO_LLM_C)
    except Drift as e:
        errors.append(f"[DSCO] anthropic-beta agent fingerprint: {e}")

    if omp_beta_list is not None and dsco_beta_str is not None:
        omp_beta_str = ",".join(omp_beta_list)
        omp_set, dsco_set = set(omp_beta_list), set(dsco_beta_str.split(","))
        if omp_set == dsco_set:
            report_row("anthropic-beta (agent, set)", omp_beta_str, dsco_beta_str, rows)
        else:
            rows.append(("anthropic-beta (agent, set)", omp_beta_str, dsco_beta_str, False))
            added = omp_set - dsco_set
            removed = dsco_set - omp_set
            if added:
                errors.append(f"betas OMP has that dsco is missing: {sorted(added)}")
            if removed:
                errors.append(f"betas dsco sends that OMP dropped: {sorted(removed)}")

    # --- report ----------------------------------------------------------------
    name_w = max((len(r[0]) for r in rows), default=10)
    any_drift = False
    for name, omp_val, dsco_val, ok in rows:
        status = "OK" if ok else "DRIFT"
        any_drift = any_drift or not ok
        print(f"[{status:5}] {name:<{name_w}}  omp={omp_val!r}  dsco={dsco_val!r}")

    if errors:
        print(file=sys.stderr)
        print("extraction problems (parser may be stale, verify manually):", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)

    if any_drift or errors:
        print(file=sys.stderr)
        print("Claude OAuth fingerprint has drifted from upstream OMP. "
              "Review the DRIFT rows above and update src/llm.c / include/provider.h "
              "(and the pinned literals in tests/test.c) to match.", file=sys.stderr)
        return 1

    print(file=sys.stderr)
    print("Claude OAuth fingerprint matches the latest tracked OMP state.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
