#!/usr/bin/env python3
"""Build and score a deterministic 1,500-prompt autonomy corpus.

The sweep prefers checked-in task/eval prompts, then public-news prompts already
cached by dsco's prompt pool. Prompt text is never written to the report; the
optional --samples output is intended for local, ephemeral inspection.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LIMIT = 1500


def normalized(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip().casefold()


def prompt_hash(text: str) -> str:
    return hashlib.sha256(normalized(text).encode()).hexdigest()


def stable_unique(values: Iterable[str]) -> list[str]:
    unique: dict[str, str] = {}
    for value in values:
        if not isinstance(value, str):
            continue
        value = value.strip()
        key = normalized(value)
        if len(key) >= 8 and key not in unique:
            unique[key] = value
    return sorted(unique.values(), key=lambda value: (prompt_hash(value), normalized(value)))


def extract_c_array(source: str, name: str) -> list[str]:
    match = re.search(
        rf"static const char \*{re.escape(name)}\[\]\s*=\s*\{{(.*?)\n\}};",
        source,
        re.DOTALL,
    )
    if not match:
        raise RuntimeError(f"could not find C string array {name}")
    literals = re.findall(r'"(?:\\.|[^"\\])*"', match.group(1))
    return [ast.literal_eval(literal) for literal in literals]


def seed_prompts() -> list[str]:
    source = (ROOT / "src/prompt_pool.c").read_text()
    subjects = extract_c_array(source, "k_subjects")
    templates = extract_c_array(source, "k_templates")
    return stable_unique(template % subject for template in templates for subject in subjects)


def jsonl_rows(path: Path) -> Iterable[dict]:
    if not path.exists():
        return
    with path.open(errors="replace") as handle:
        for line in handle:
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                yield value


def field_prompts(path: str, field: str) -> list[str]:
    return stable_unique(
        row[field]
        for row in jsonl_rows(ROOT / path)
        if isinstance(row.get(field), str)
    )


def curated_repo_prompts() -> list[str]:
    specs = (
        (".workspace/training/low_level_mastery/low_level_mastery_sft.v1.jsonl", "instruction"),
        (".workspace/training/low_level_mastery/low_level_mastery_sft.v2.jsonl", "instruction"),
        (".workspace/training/low_level_mastery/low_level_mastery_preferences.v1.jsonl", "prompt"),
        (".workspace/training/low_level_mastery/low_level_mastery_preferences.v2.jsonl", "prompt"),
        ("reports/perf_push_limit/multiturn_rollout.jsonl", "task"),
        ("reports/dspy_session_distill_trainset_large.jsonl", "session_prompt"),
    )
    values: list[str] = []
    for path, field in specs:
        values.extend(field_prompts(path, field))
    return stable_unique(values)


def runtime_news_prompts(path: Path) -> list[str]:
    return stable_unique(
        row["q"]
        for row in jsonl_rows(path)
        if row.get("src") == "news" and isinstance(row.get("q"), str)
    )


PATTERNS = {
    "imperative_start": re.compile(
        r"^(?:add|analyze|argue|assess|audit|benchmark|build|choose|compare|create|"
        r"debug|deep-dive|design|draft|estimate|explain|fact-check|find|fix|"
        r"identify|implement|inspect|profile|refactor|review|rewrite|run|search|"
        r"sketch|summarize|test|threat-model|trace|validate|verify|wire|write)\b",
        re.IGNORECASE,
    ),
    "concrete_deliverable": re.compile(
        r"\b(?:add|artifact|benchmark|build|create|design|draft|implement|patch|"
        r"program|refactor|report|rewrite|runbook|script|tests?|wire|write)\b",
        re.IGNORECASE,
    ),
    "tool_or_execution": re.compile(
        r"\b(?:api|call|command|compile|execute|fetch|load|run|search|shell|tool|"
        r"write_file)\b",
        re.IGNORECASE,
    ),
    "verification_evidence": re.compile(
        r"\b(?:benchmark|compile|evidence|inspect(?:ing)? outputs?|proof|tests?|"
        r"validate|verification|verify)\b",
        re.IGNORECASE,
    ),
    "completion_terminal": re.compile(
        r"\b(?:complete|completion|done|end-to-end|finish|terminal state|until)\b",
        re.IGNORECASE,
    ),
    "continuation": re.compile(
        r"\b(?:continue|do not stop|keep going|next highest-value|next step|"
        r"without (?:asking|confirmation|waiting))\b",
        re.IGNORECASE,
    ),
    "recovery": re.compile(
        r"\b(?:debug|fallback|failure modes?|post-mortem|recover|repair|retry)\b",
        re.IGNORECASE,
    ),
    "authority_boundary": re.compile(
        r"\b(?:approval|authority|capability gate|permission|prohibited|"
        r"reversible|safe|scope)\b",
        re.IGNORECASE,
    ),
    "parallel_delegation": re.compile(
        r"\b(?:concurrent|delegate|fan-out|map-reduce|parallel|swarm|workers?)\b",
        re.IGNORECASE,
    ),
    "permission_seeking": re.compile(
        r"\b(?:can you|could you|if you(?:'d| would) like|may i|want me to|"
        r"would you)\b",
        re.IGNORECASE,
    ),
    "advisory_only": re.compile(
        r"\b(?:consider|might want|recommend|suggest|you can|you could|you may)\b",
        re.IGNORECASE,
    ),
}


def assemble(limit: int, prompt_pool_path: Path) -> tuple[list[tuple[str, str]], dict[str, int]]:
    sources = {
        "prompt_pool_seed": seed_prompts(),
        "runtime_news": runtime_news_prompts(prompt_pool_path),
        "low_level_eval": field_prompts(
            ".workspace/training/low_level_mastery/low_level_mastery_evals.v2.jsonl",
            "question",
        ),
        "curated_repo_tasks": curated_repo_prompts(),
    }
    quotas = {
        "prompt_pool_seed": 1000,
        "runtime_news": 300,
        "low_level_eval": 140,
        "curated_repo_tasks": 60,
    }
    selected: list[tuple[str, str]] = []
    seen: set[str] = set()
    consumed: dict[str, set[str]] = defaultdict(set)

    def add(source: str, prompt: str) -> None:
        key = normalized(prompt)
        if key in seen or len(selected) >= limit:
            return
        seen.add(key)
        consumed[source].add(key)
        selected.append((source, prompt))

    for source in sources:
        for prompt in sources[source][: quotas[source]]:
            add(source, prompt)

    # Keep the sweep runnable without a local news cache by filling from every
    # unused checked-in prompt in stable hash order.
    if len(selected) < limit:
        remainder = sorted(
            (
                (prompt_hash(prompt), source, prompt)
                for source, prompts in sources.items()
                for prompt in prompts
                if normalized(prompt) not in consumed[source]
            ),
            key=lambda item: (item[0], item[1]),
        )
        for _, source, prompt in remainder:
            add(source, prompt)

    if len(selected) != limit:
        available = sum(len(prompts) for prompts in sources.values())
        raise RuntimeError(f"needed {limit} unique prompts; assembled {len(selected)} from {available}")
    return selected, {source: len(prompts) for source, prompts in sources.items()}


def score(selected: list[tuple[str, str]]) -> Counter:
    overall: Counter = Counter()
    for _, prompt in selected:
        hits = {name for name, pattern in PATTERNS.items() if pattern.search(prompt)}
        overall.update(hits)
        control_hits = hits & {
            "verification_evidence",
            "completion_terminal",
            "continuation",
            "recovery",
            "authority_boundary",
        }
        if len(control_hits) >= 2:
            overall["multi_stage_control"] += 1
        if "verification_evidence" in hits and "completion_terminal" in hits:
            overall["verified_terminal_state"] += 1
    return overall


def render_report(selected: list[tuple[str, str]], available: dict[str, int], pool_path: Path) -> str:
    overall = score(selected)
    selected_counts = Counter(source for source, _ in selected)
    manifest = hashlib.sha256(
        "\n".join(sorted(f"{source}\t{normalized(prompt)}" for source, prompt in selected)).encode()
    ).hexdigest()
    lines = [
        f"# Autonomy prompt sweep ({len(selected):,} unique prompts)",
        "",
        "Deterministic corpus-level review of task wording that informs dsco's runtime autonomy contract.",
        "Prompt bodies are intentionally omitted from this artifact; use the script's `--samples` flag for local inspection.",
        "",
        "## Corpus",
        "",
        "| Source | Available unique | Selected |",
        "|---|---:|---:|",
    ]
    for source in available:
        lines.append(f"| `{source}` | {available[source]} | {selected_counts[source]} |")
    lines.extend(
        [
            f"| **Total** |  | **{len(selected)}** |",
            "",
            f"Corpus SHA-256: `{manifest}`",
            "",
            f"Runtime news cache: `{pool_path}` (only records marked `src=news`; public headline-derived prompts).",
            "",
            "## Language signals",
            "",
            "| Signal | Prompts | Share |",
            "|---|---:|---:|",
        ]
    )
    ordered = list(PATTERNS) + ["multi_stage_control", "verified_terminal_state"]
    for name in ordered:
        count = overall[name]
        lines.append(f"| `{name}` | {count} | {count / len(selected):.1%} |")
    lines.extend(
        [
            "",
            "## Prompt-engineering conclusions",
            "",
            "- Task prompts commonly specify an action or artifact, but rarely define the agent's continuation, recovery, authority, and completion loop together.",
            "- Verification language is materially more common than a verified terminal-state contract; the system prompt must join execution, evidence, and stopping criteria explicitly.",
            "- Permission-seeking language is not the main corpus problem. The failure mode is an underspecified control policy: models can still narrate, defer, or stop after a partial result.",
            "- Full and cheap modes therefore need one shared autonomy contract with an action default, bounded persistence, runtime-evidenced blockers, and exact authority limits.",
            "- The contract should prohibit speculative tool denial while preserving actual capability gates and approvals as authoritative runtime boundaries.",
            "",
            "## Reproduce",
            "",
            "```sh",
            "python3 scripts/review_autonomy_prompts.py --report reports/AUTONOMY_PROMPT_SWEEP_1500.md",
            "```",
            "",
        ]
    )
    return "\n".join(lines)


def print_samples(selected: list[tuple[str, str]], count: int) -> None:
    if count <= 0:
        return
    groups: dict[str, list[str]] = defaultdict(list)
    for source, prompt in selected:
        groups[source].append(prompt)
    per_source = max(1, count // len(groups))
    emitted = 0
    for source, prompts in groups.items():
        for prompt in prompts[:per_source]:
            if emitted >= count:
                return
            print(json.dumps({"source": source, "sha256": prompt_hash(prompt), "prompt": prompt}))
            emitted += 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--limit", type=int, default=DEFAULT_LIMIT)
    parser.add_argument("--prompt-pool", type=Path, default=Path.home() / ".dsco/prompt_pool.jsonl")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--samples", type=int, default=0)
    args = parser.parse_args()

    selected, available = assemble(args.limit, args.prompt_pool)
    report = render_report(selected, available, args.prompt_pool)
    if args.report:
        target = args.report if args.report.is_absolute() else ROOT / args.report
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(report)
    else:
        print(report)
    print_samples(selected, args.samples)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
