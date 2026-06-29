#!/usr/bin/env python3
"""
Read-only inventory/validator for DSCO marker-addressed data slots.

Initial scope: include/config.h::MODEL_REGISTRY[].

This is deliberately boring: it does not patch source, rewrite code, or mutate
runtime state. It extracts registry rows, validates shape/domain invariants, and
emits a JSON inventory suitable for future marker-addressed data-slot tooling.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


@dataclass
class ModelSlot:
    id: str
    path: str
    line: int
    alias: str | None
    model_id: str | None
    context_window: int
    max_output: int
    input_price: float
    output_price: float
    cache_read_price: float
    cache_write_price: float
    supports_thinking: int
    raw: str
    sentinel: bool = False


@dataclass
class ValidationResult:
    ok: bool
    errors: list[str]
    warnings: list[str]
    count: int
    sentinel_count: int


def strip_comments(src: str) -> str:
    """Remove C comments while preserving string literals and line count."""
    out: list[str] = []
    i = 0
    n = len(src)
    in_str = False
    in_chr = False
    esc = False
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        if in_str:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            i += 1
            continue

        if in_chr:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == "'":
                in_chr = False
            i += 1
            continue

        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "'":
            in_chr = True
            out.append(c)
            i += 1
            continue

        if c == "/" and nxt == "/":
            # Preserve newline so line numbers remain meaningful.
            i += 2
            while i < n and src[i] != "\n":
                i += 1
            if i < n:
                out.append("\n")
                i += 1
            continue

        if c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            i += 2 if i + 1 < n else 0
            continue

        out.append(c)
        i += 1

    return "".join(out)


def find_registry_body(src: str) -> tuple[str, int]:
    m = re.search(r"static\s+const\s+model_info_t\s+MODEL_REGISTRY\s*\[\s*\]\s*=\s*\{", src)
    if not m:
        raise ValueError("MODEL_REGISTRY[] declaration not found")

    open_idx = src.find("{", m.end() - 1)
    if open_idx < 0:
        raise ValueError("MODEL_REGISTRY opening brace not found")

    depth = 0
    in_str = False
    in_chr = False
    esc = False
    for i in range(open_idx, len(src)):
        c = src[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if in_chr:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == "'":
                in_chr = False
            continue
        if c == '"':
            in_str = True
            continue
        if c == "'":
            in_chr = True
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                body = src[open_idx + 1 : i]
                start_line = src.count("\n", 0, open_idx) + 1
                return body, start_line

    raise ValueError("MODEL_REGISTRY closing brace not found")


def iter_top_level_rows(body: str, body_start_line: int) -> Iterable[tuple[str, int]]:
    depth = 0
    start: int | None = None
    row_line = body_start_line
    in_str = False
    in_chr = False
    esc = False

    for i, c in enumerate(body):
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if in_chr:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == "'":
                in_chr = False
            continue
        if c == '"':
            in_str = True
            continue
        if c == "'":
            in_chr = True
            continue
        if c == "{":
            if depth == 0:
                start = i
                row_line = body_start_line + body.count("\n", 0, i)
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start is not None:
                yield body[start : i + 1], row_line
                start = None


def split_csv_fields(row: str) -> list[str]:
    inner = row.strip()
    if not (inner.startswith("{") and inner.endswith("}")):
        raise ValueError(f"row is not a brace initializer: {row[:80]!r}")
    inner = inner[1:-1]

    fields: list[str] = []
    cur: list[str] = []
    depth = 0
    in_str = False
    in_chr = False
    esc = False
    for c in inner:
        if in_str:
            cur.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            continue
        if in_chr:
            cur.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == "'":
                in_chr = False
            continue
        if c == '"':
            in_str = True
            cur.append(c)
            continue
        if c == "'":
            in_chr = True
            cur.append(c)
            continue
        if c in "([{":
            depth += 1
            cur.append(c)
            continue
        if c in ")]}":
            depth -= 1
            cur.append(c)
            continue
        if c == "," and depth == 0:
            fields.append("".join(cur).strip())
            cur = []
            continue
        cur.append(c)
    if cur or inner.strip():
        fields.append("".join(cur).strip())
    return fields


def parse_c_string(field: str) -> str | None:
    field = field.strip()
    if field == "NULL":
        return None
    if len(field) >= 2 and field[0] == '"' and field[-1] == '"':
        # Good enough for registry strings; decode common C escapes via unicode_escape.
        return bytes(field[1:-1], "utf-8").decode("unicode_escape")
    raise ValueError(f"expected string literal or NULL, got {field!r}")


def parse_int(field: str) -> int:
    return int(field.strip(), 0)


def parse_float(field: str) -> float:
    return float(field.strip())


def parse_model_slot(row: str, line: int, path: Path) -> ModelSlot:
    fields = split_csv_fields(row)
    if len(fields) != 9:
        raise ValueError(f"line {line}: expected 9 model_info_t fields, got {len(fields)}: {row[:120]!r}")

    alias = parse_c_string(fields[0])
    model_id = parse_c_string(fields[1])
    context_window = parse_int(fields[2])
    max_output = parse_int(fields[3])
    input_price = parse_float(fields[4])
    output_price = parse_float(fields[5])
    cache_read_price = parse_float(fields[6])
    cache_write_price = parse_float(fields[7])
    supports_thinking = parse_int(fields[8])
    sentinel = alias is None and model_id is None
    slot_id = "model.registry.__sentinel__" if sentinel else f"model.registry.{alias}"

    return ModelSlot(
        id=slot_id,
        path=str(path),
        line=line,
        alias=alias,
        model_id=model_id,
        context_window=context_window,
        max_output=max_output,
        input_price=input_price,
        output_price=output_price,
        cache_read_price=cache_read_price,
        cache_write_price=cache_write_price,
        supports_thinking=supports_thinking,
        raw=" ".join(row.split()),
        sentinel=sentinel,
    )


def validate(slots: list[ModelSlot]) -> ValidationResult:
    errors: list[str] = []
    warnings: list[str] = []

    if not slots:
        errors.append("MODEL_REGISTRY has no rows")
        return ValidationResult(False, errors, warnings, 0, 0)

    sentinel_rows = [s for s in slots if s.sentinel]
    if len(sentinel_rows) != 1:
        errors.append(f"expected exactly one sentinel row, found {len(sentinel_rows)}")
    elif not slots[-1].sentinel:
        errors.append(f"sentinel row must be last, found at line {sentinel_rows[0].line}")

    aliases: dict[str, ModelSlot] = {}
    exact_duplicates: set[str] = set()
    for s in slots:
        if s.sentinel:
            nonzero = [
                s.context_window,
                s.max_output,
                s.input_price,
                s.output_price,
                s.cache_read_price,
                s.cache_write_price,
                s.supports_thinking,
            ]
            if any(v != 0 for v in nonzero):
                errors.append(f"line {s.line}: sentinel row must contain zero numeric fields")
            continue

        if not s.alias or not s.model_id:
            errors.append(f"line {s.line}: non-sentinel row requires alias and model_id")
        if s.alias in aliases:
            exact_duplicates.add(s.alias or "<null>")
            prev = aliases[s.alias or ""]
            errors.append(f"line {s.line}: duplicate alias {s.alias!r}; previous at line {prev.line}")
        elif s.alias:
            aliases[s.alias] = s

        if s.context_window <= 0:
            errors.append(f"line {s.line}: context_window must be positive for {s.alias}")
        if s.max_output <= 0:
            errors.append(f"line {s.line}: max_output must be positive for {s.alias}")
        for name in ("input_price", "output_price", "cache_read_price", "cache_write_price"):
            if getattr(s, name) < 0:
                errors.append(f"line {s.line}: {name} must be nonnegative for {s.alias}")
        if s.supports_thinking not in (0, 1):
            errors.append(f"line {s.line}: supports_thinking must be 0 or 1 for {s.alias}")

    # Same model_id may intentionally have multiple aliases; warn if the pricing/window differs.
    by_model: dict[str, list[ModelSlot]] = {}
    for s in slots:
        if s.sentinel or not s.model_id:
            continue
        by_model.setdefault(s.model_id, []).append(s)
    for model_id, rows in sorted(by_model.items()):
        if len(rows) <= 1:
            continue
        signatures = {
            (r.context_window, r.max_output, r.input_price, r.output_price,
             r.cache_read_price, r.cache_write_price, r.supports_thinking)
            for r in rows
        }
        if len(signatures) > 1:
            aliases_str = ", ".join(r.alias or "<null>" for r in rows)
            warnings.append(f"model_id {model_id!r} has aliases with differing metadata: {aliases_str}")

    return ValidationResult(
        ok=not errors,
        errors=errors,
        warnings=warnings,
        count=len([s for s in slots if not s.sentinel]),
        sentinel_count=len(sentinel_rows),
    )


def load_slots(path: Path) -> list[ModelSlot]:
    raw = path.read_text()
    src = strip_comments(raw)
    body, start_line = find_registry_body(src)
    return [parse_model_slot(row, line, path) for row, line in iter_top_level_rows(body, start_line)]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Inventory/validate DSCO marker-addressed data slots")
    ap.add_argument("--path", default="include/config.h", help="path to config.h")
    ap.add_argument("--json", action="store_true", help="emit full JSON inventory")
    ap.add_argument("--quiet", action="store_true", help="only print errors")
    args = ap.parse_args(argv)

    path = Path(args.path)
    try:
        slots = load_slots(path)
        result = validate(slots)
    except Exception as exc:  # noqa: BLE001 - CLI boundary
        print(f"slot_inventory: ERROR: {exc}", file=sys.stderr)
        return 2

    payload = {
        "registry": "MODEL_REGISTRY",
        "path": str(path),
        "validation": asdict(result),
        "slots": [asdict(s) for s in slots],
    }

    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    elif not args.quiet:
        status = "OK" if result.ok else "ERROR"
        print(f"MODEL_REGISTRY {status}: {result.count} model slots, {result.sentinel_count} sentinel")
        for w in result.warnings:
            print(f"warning: {w}")
        for e in result.errors:
            print(f"error: {e}")

    return 0 if result.ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
