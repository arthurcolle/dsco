#!/usr/bin/env python3
"""Inventory local provenance risks without transmitting or quoting source material.

Restricted terms are never stored in this repository in clear text:
  * scripts/ip_audit_restricted.sha256 (override: DSCO_IP_AUDIT_HASHES) holds a
    `salt <hex>` line and `<rule> <sha256>` lines, sha256("<salt>:<normalized term>");
    generate lines with `--hash-terms RULE < terms.txt`.
  * an optional private regex file (DSCO_IP_AUDIT_PATTERNS, default
    ~/.dsco/ip_audit_patterns.txt), one `[rule:] regex` per line.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 16 * 1024 * 1024
# Public rules. Restricted lineage/private-source terms are not listed here:
# they come from salted SHA-256 hashes committed in HASHES_FILE (matched against
# normalized word n-grams) and from an optional private local patterns file.
RULES = {
    "restricted_lineage": r"(?!x)x",
    "private_source": r"IMG_2[123]\d\d\.(?:md|txt)|lingo/corpus-index\.json",
    "derivation_claim": r"(?:copied|adapted|borrowed|derived) from|inspired by|inheritance is direct",
    "ownership_claim": r"(?:design|interpretation|implementation|intellectual property) (?:is|are) ours|patent.pending|proprietary",
    "business_claim": r"production[ -](?:ready|grade)|audit[ -]grade|pilot[ -]ready|state.of.the.art|(?:billion|70B)[ -]?(?:node|scale)",
    "third_party_notice": r"copyright|SPDX-License-Identifier|see LICENSE|GNU General Public License",
    "unverified_origin_assurance": r"not a copy of any proprietary ontology|rather than copied proprietary schema|(?:interpretation|design|implementation) (?:is|are) ours",
}
RESTRICTED_RULES = ("restricted_lineage", "private_source")
HASH_RULES = RESTRICTED_RULES + ("path_only",)
HASHES_FILE = Path(os.environ.get("DSCO_IP_AUDIT_HASHES") or Path(__file__).resolve().with_name("ip_audit_restricted.sha256"))
PRIVATE_PATTERNS_FILE = Path(os.environ.get("DSCO_IP_AUDIT_PATTERNS") or Path.home() / ".dsco" / "ip_audit_patterns.txt")
MAX_NGRAM = 4
TOKEN = re.compile(r"[a-z0-9]+")


def normalize_term(term: str) -> str:
    """Lowercase alphanumeric words joined by single spaces ("Foo_Bar-db" -> "foo bar db")."""
    return " ".join(TOKEN.findall(term.lower()))


def term_hash(salt: str, term: str) -> str:
    return hashlib.sha256(f"{salt}:{normalize_term(term)}".encode()).hexdigest()


def load_hashes(path: Path) -> tuple[str, dict]:
    """Parse `salt <hex>` plus `<rule> <sha256>` lines. Missing file -> no hashed terms."""
    salt, hashes = "", {}
    try:
        lines = path.read_text().splitlines()
    except OSError:
        return salt, hashes
    for raw in lines:
        parts = raw.split("#", 1)[0].split()
        if len(parts) != 2:
            continue
        if parts[0] == "salt":
            salt = parts[1]
        elif parts[0] in HASH_RULES and re.fullmatch(r"[0-9a-f]{64}", parts[1]):
            hashes[parts[1]] = parts[0]
    return salt, (hashes if salt else {})


def load_private_patterns(path: Path) -> dict:
    """Optional local file: `[rule:] regex` per line, rule defaults to private_source."""
    extra = collections.defaultdict(list)
    try:
        lines = path.read_text().splitlines()
    except OSError:
        return extra
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        rule, sep, pattern = line.partition(":")
        if sep and rule.strip() in RESTRICTED_RULES:
            line = pattern.strip()
        else:
            rule = "private_source"
        try:
            re.compile(line)
        except re.error:
            print(f"ip_audit: ignoring invalid private pattern on a line of {path}", file=sys.stderr)
            continue
        extra[rule.strip()].append(line)
    return extra


SALT, HASHED_TERMS = load_hashes(HASHES_FILE)
PRIVATE_PATTERNS = load_private_patterns(PRIVATE_PATTERNS_FILE)
for _rule, _patterns in PRIVATE_PATTERNS.items():
    RULES[_rule] = "|".join([RULES[_rule], *_patterns])
PATTERNS = {name: re.compile(pattern, re.I) for name, pattern in RULES.items()}
COMBINED = re.compile("|".join(f"(?P<{name}>{pattern})" for name, pattern in RULES.items()), re.I)
KEYWORDS = ("img_2", "corpus-index", "copied", "adapted", "borrowed", "derived", "inspired", "inheritance", "ours", "patent", "proprietary", "production", "audit", "pilot", "state", "billion", "70b", "copyright", "license")
_NGRAM_CACHE: dict = {}


def hashed_hits(text: str) -> list:
    """(offset, rule) for every word n-gram whose salted hash is restricted."""
    if not HASHED_TERMS:
        return []
    tokens = [(m.start(), m.end(), m.group()) for m in TOKEN.finditer(text.lower())]
    hits = []
    for index, (start, _end, word) in enumerate(tokens):
        gram = word
        for width in range(1, MAX_NGRAM + 1):
            if width > 1:
                if index + width > len(tokens):
                    break
                previous_end = tokens[index + width - 2][1]
                next_start, _, next_word = tokens[index + width - 1]
                if next_start - previous_end > 2:
                    break
                gram = f"{gram} {next_word}"
            rule = _NGRAM_CACHE.get(gram, 0)
            if rule == 0:
                rule = HASHED_TERMS.get(hashlib.sha256(f"{SALT}:{gram}".encode()).hexdigest())
                _NGRAM_CACHE[gram] = rule
            if rule:
                hits.append((start, rule))
    return hits


def restricted_name(path: str, *, include_path_only: bool = False) -> bool:
    if PATTERNS["restricted_lineage"].search(path) or PATTERNS["private_source"].search(path):
        return True
    return any(rule in RESTRICTED_RULES or include_path_only for _, rule in hashed_hits(path))

DEPENDENCY_PARTS = {".venv", "venv", "site-packages", "node_modules", ".mypy_cache", ".cache", "__pycache__"}
PRIVATE_NAMES = re.compile(r"(?:^\.env(?:\.|$)|secrets|credentials|\.pem$|\.key$|^embedded_key\.gen\.h$|^cstring_key\.gen\.h$)", re.I)
PRODUCT_ROOTS = {"docs", "demos", "prompts", "examples", "lingo", "src", "include", "npm", "web", "design", "research", "reports", "assets", "proposals", "doctrine", "config", "schemas", "data"}


def git(root: Path, *arguments: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *arguments])


def git_paths(root: Path, *arguments: str) -> set[str]:
    return {value.decode("utf-8", "surrogateescape") for value in git(root, "ls-files", "-z", *arguments).split(b"\0") if value}


def findings(data: bytes) -> dict:
    if b"\0" in data[:8192]:
        return {}
    text = data.decode("utf-8", "replace")
    lowered = text.lower()
    hits = collections.defaultdict(list)
    for offset, rule in hashed_hits(text):
        if rule in RESTRICTED_RULES and len(hits[rule]) < 40:
            hits[rule].append(offset)
    if PRIVATE_PATTERNS or any(keyword in lowered for keyword in KEYWORDS):
        for match in COMBINED.finditer(text):
            if len(hits[match.lastgroup]) < 40:
                hits[match.lastgroup].append(match.start())
    hits = {name: offsets for name, offsets in hits.items() if offsets}
    if not hits:
        return {}
    locations = collections.defaultdict(set)
    previous = 0
    line = 1
    for offset, name in sorted((offset, name) for name, offsets in hits.items() for offset in offsets):
        line += text.count("\n", previous, offset)
        previous = offset
        locations[name].add(line)
    return {name: sorted(lines) for name, lines in locations.items()}


def archive_findings(data: bytes) -> dict:
    hits = {}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        members = archive.infolist()
        if sum(member.file_size for member in members) > 64 * 1024 * 1024 or len(members) > 10000:
            return {"coverage": "archive_size_limit"}
        for member in members:
            if not member.is_dir() and member.file_size <= LIMIT:
                found = findings(archive.read(member))
                if found:
                    hits[member.filename] = found
    return hits


def category(path: str) -> str:
    parts = Path(path).parts
    if any(part in DEPENDENCY_PARTS for part in parts):
        return "installed_dependency_or_cache"
    if parts[0] in {"vendor", "gsl"}:
        return "third_party_or_compatibility_origin_unverified"
    if parts[0] in {".workspace", ".claude", "memory", "research_notes", "email-mcp", ".swarm", "logs"}:
        return "private_local_or_generated_material"
    if parts[0] in {"src", "include", "lingo", "tests", "scripts", "crates", "web", "npm"}:
        return "local_implementation_ownership_unverified"
    if parts[0] in {"docs", "demos", "prompts", "proposals", "design", "research"} or path == "README.md":
        return "documentation_or_proposal_claims_unverified"
    if parts[0] in {"build", "reports", "generated", "assets", "data", "artifacts"}:
        return "data_or_artifact_provenance_unverified"
    return "other_origin_unverified"


def audit(root: Path, output: Path) -> None:
    tracked = git_paths(root)
    visible = tracked | git_paths(root, "--others", "--exclude-standard")
    output.mkdir(parents=True, exist_ok=True)
    counts = collections.Counter()
    rules = collections.Counter()
    inventory = output / "inventory.jsonl"
    with inventory.open("w") as handle:
        for directory, children, names in os.walk(root, followlinks=False):
            children[:] = sorted(name for name in children if name != ".git")
            for name in sorted(names):
                path = Path(directory) / name
                if output == path or output in path.parents:
                    continue
                relative = path.relative_to(root).as_posix()
                row = {"path": relative, "tracked": relative in tracked, "git_visible": relative in visible, "category": category(relative)}
                counts["files"] += 1
                counts["tracked_present"] += row["tracked"]
                try:
                    row["bytes"] = path.lstat().st_size
                    counts["bytes"] += row["bytes"]
                    if path.is_symlink():
                        row["coverage"] = "symlink_not_followed"
                    elif any(part in DEPENDENCY_PARTS for part in Path(relative).parts):
                        row["coverage"] = "dependency_or_cache_inventory_only"
                    elif PRIVATE_NAMES.search(name):
                        row["coverage"] = "sensitive_name_inventory_only"
                    elif row["bytes"] > LIMIT:
                        row["coverage"] = "size_limit_inventory_only"
                    else:
                        data = path.read_bytes()
                        row["sha256"] = hashlib.sha256(data).hexdigest()
                        if data[:4] == b"PK\x03\x04":
                            row["archive_findings"] = archive_findings(data)
                            row["coverage"] = "bounded_archive_scan"
                        elif b"\0" in data[:8192]:
                            row["coverage"] = "binary_inventory_only"
                        else:
                            row["findings"] = findings(data)
                            rules.update(row["findings"].keys())
                            row["coverage"] = "text_pattern_scan"
                except (OSError, zipfile.BadZipFile, RuntimeError) as error:
                    row["coverage"] = "read_error"
                    row["error_type"] = type(error).__name__
                counts[row["coverage"]] += 1
                handle.write(json.dumps(row, ensure_ascii=True, sort_keys=True) + "\n")
    summary = {"root": str(root), "head": git(root, "rev-parse", "HEAD").decode().strip(), "counts": dict(counts), "tracked_index_entries": len(tracked), "missing_tracked_files": sorted(path for path in tracked if not (root / path).exists()), "rule_file_counts": dict(rules), "scope": "Whole-tree metadata; bounded local pattern scan, not semantic certification or proof of ownership."}
    (output / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    (output / "status-before.txt").write_bytes(git(root, "status", "--porcelain=v1", "-z"))
    print(json.dumps(summary, indent=2))


def history(root: Path, output: Path) -> None:
    objects = git(root, "rev-list", "--objects", "--all").splitlines()
    process = subprocess.Popen(["git", "-C", str(root), "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    risky = []
    scanned = 0
    for value in objects:
        pieces = value.split(b" ", 1)
        if len(pieces) != 2:
            continue
        object_id, path = pieces
        process.stdin.write(object_id + b"\n")
        process.stdin.flush()
        header = process.stdout.readline().split()
        if len(header) != 3:
            raise RuntimeError("Unexpected git cat-file response")
        length = int(header[2])
        data = process.stdout.read(length)
        if len(data) != length or process.stdout.read(1) != b"\n":
            raise RuntimeError("Incomplete git object")
        if header[1] != b"blob":
            continue
        scanned += 1
        matches = findings(data)
        archive_hits = archive_findings(data) if data[:4] == b"PK\x03\x04" and length <= LIMIT else {}
        restricted = {name: lines for name, lines in matches.items() if name in {"restricted_lineage", "private_source"}}
        restricted_members = {name: found for name, found in archive_hits.items() if isinstance(found, dict) and ("restricted_lineage" in found or "private_source" in found)}
        path_text = path.decode("utf-8", "surrogateescape")
        if restricted or restricted_members or "corpus-index" in path_text.lower() or restricted_name(path_text, include_path_only=True):
            risky.append({"path": path.decode("utf-8", "surrogateescape"), "object": object_id.decode(), "bytes": length, "findings": restricted, "archive_members": restricted_members})
    process.stdin.close()
    if process.wait() != 0:
        raise RuntimeError("git cat-file failed")
    result = {"blob_versions_scanned": scanned, "risky_versions": risky, "paths": sorted({row["path"] for row in risky})}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"blob_versions_scanned": scanned, "risky_versions": len(risky), "paths": result["paths"]}, indent=2))


def check(root: Path) -> int:
    failed = []
    paths = git_paths(root) | git_paths(root, "--others", "--exclude-standard")
    for relative in sorted(paths):
        path = root / relative
        if not path.is_file() or path.is_symlink() or path.stat().st_size > LIMIT:
            continue
        if Path(relative).parts[0] not in PRODUCT_ROOTS and len(Path(relative).parts) != 1:
            continue
        if restricted_name(relative):
            failed.append(f"{relative}: restricted file name")
        data = path.read_bytes()
        candidates = findings(data)
        for name in ("restricted_lineage", "private_source", "unverified_origin_assurance"):
            for line in candidates.get(name, []):
                failed.append(f"{relative}:{line}: {name}")
        if data[:4] == b"PK\x03\x04":
            try:
                members = archive_findings(data)
                if "coverage" in members:
                    failed.append(f"{relative}: archive exceeds inspection limit")
                for member, matched in members.items():
                    if isinstance(matched, dict):
                        for name in ("restricted_lineage", "private_source", "unverified_origin_assurance"):
                            if name in matched:
                                failed.append(f"{relative}:{member}: {name}")
            except (zipfile.BadZipFile, RuntimeError, OSError) as error:
                failed.append(f"{relative}: unreadable archive: {type(error).__name__}")
    for item in failed:
        print(item, file=sys.stderr)
    print(f"IP documentation check: {len(failed)} restricted references; {len(paths)} repository paths considered")
    return int(bool(failed))


def hash_terms(rule: str) -> int:
    """Read restricted terms from stdin and print committable hash lines (never the terms)."""
    salt = SALT
    if not salt:
        print("ip_audit: no salt in hashes file; add a `salt <hex>` line first", file=sys.stderr)
        return 2
    for raw in sys.stdin:
        term = normalize_term(raw)
        if term:
            if len(term.split()) > MAX_NGRAM:
                print(f"ip_audit: skipping a term longer than {MAX_NGRAM} words", file=sys.stderr)
                continue
            print(f"{rule} {term_hash(salt, term)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--hash-terms", choices=HASH_RULES, metavar="RULE",
                        help="read terms on stdin, print salted hash lines for HASHES_FILE")
    parser.add_argument("--audit", type=Path)
    parser.add_argument("--history", type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    if arguments.hash_terms:
        return hash_terms(arguments.hash_terms)
    root = arguments.root.resolve()
    if arguments.audit:
        audit(root, arguments.audit.resolve())
    if arguments.history:
        history(root, arguments.history.resolve())
    return check(root) if arguments.check else 0


if __name__ == "__main__":
    raise SystemExit(main())
