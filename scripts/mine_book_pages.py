#!/usr/bin/env python3
"""Mine repo-local systems PDFs into page-level derived metadata.

The generated artifacts intentionally avoid storing raw page text. They keep
hashes, counts, terms, tags, and source pointers so the corpus is queryable
without duplicating copyrighted book content in repo artifacts.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile
from typing import Counter, Dict, Iterable, List, Tuple


STOPWORDS = {
    "about", "above", "after", "again", "against", "also", "and", "any",
    "are", "because", "been", "before", "being", "between", "both", "but",
    "can", "chapter", "code", "could", "did", "does", "each", "else",
    "example", "figure", "for", "from", "had", "has", "have", "into",
    "its", "may", "more", "most", "must", "not", "one", "only", "other",
    "our", "out", "program", "programming", "same", "see", "should",
    "some", "such", "than", "that", "the", "their", "then", "there",
    "these", "they", "this", "those", "through", "two", "use", "used",
    "using", "value", "values", "was", "were", "when", "where", "which",
    "while", "will", "with", "would", "you", "your",
}


TAG_KEYWORDS: Dict[str, Dict[str, int]] = {
    "cli_parser": {
        "argc": 3, "argv": 3, "argument": 2, "arguments": 2, "command": 1,
        "grammar": 3, "lex": 2, "option": 3, "options": 3, "parse": 3,
        "parser": 3, "prompt": 2, "scan": 2, "scanner": 3, "token": 3,
        "tokens": 3,
    },
    "memory_lifetime": {
        "address": 2, "alloc": 3, "allocate": 3, "allocator": 3, "arena": 4,
        "buffer": 2, "free": 3, "heap": 2, "lifetime": 3, "malloc": 4,
        "memory": 2, "pointer": 3, "pointers": 3, "realloc": 4, "stack": 2,
    },
    "network_io": {
        "accept": 3, "addrinfo": 4, "bind": 3, "connect": 3, "descriptor": 2,
        "listen": 3, "network": 2, "poll": 2, "select": 2, "sockaddr": 4,
        "socket": 4, "sockets": 4, "tcp": 3, "udp": 3,
    },
    "process_unix": {
        "daemon": 2, "descriptor": 2, "exec": 3, "fd": 3, "file": 1,
        "fork": 4, "pipe": 3, "process": 3, "signal": 3, "signals": 3,
        "wait": 2, "waitpid": 4,
    },
    "security_ub": {
        "bounds": 3, "cert": 2, "exploit": 3, "integer": 2, "misra": 2,
        "overflow": 4, "secure": 2, "security": 3, "taint": 3,
        "undefined": 4, "vulnerability": 4, "vulnerabilities": 4,
    },
    "error_cleanup": {
        "cleanup": 4, "diagnostic": 3, "errno": 4, "error": 3, "errors": 3,
        "fail": 2, "failure": 3, "goto": 2, "recover": 3, "recovery": 3,
        "retry": 3, "status": 1,
    },
    "performance_locality": {
        "branch": 2, "cache": 4, "compiler": 2, "cpu": 3, "instruction": 3,
        "instructions": 3, "latency": 3, "locality": 4, "optimization": 3,
        "register": 3, "throughput": 4, "vector": 3,
    },
    "linking_build": {
        "dynamic": 2, "library": 2, "link": 3, "linker": 4, "loader": 4,
        "object": 2, "relocation": 4, "shared": 2, "static": 2, "symbol": 3,
        "symbols": 3,
    },
    "data_structures": {
        "array": 2, "graph": 3, "hash": 3, "heap": 2, "list": 2, "queue": 3,
        "search": 2, "sort": 2, "stack": 2, "table": 2, "tree": 3,
    },
    "api_design": {
        "abstraction": 3, "contract": 3, "encapsulation": 3, "header": 2,
        "implementation": 2, "interface": 4, "module": 3, "opaque": 3,
        "type": 1,
    },
    "concurrency": {
        "atomic": 4, "condition": 2, "concurrent": 3, "deadlock": 4,
        "lock": 3, "mutex": 4, "race": 4, "thread": 3, "threads": 3,
    },
    "data_layout": {
        "alignment": 4, "array": 2, "byte": 2, "cache": 2, "endian": 4,
        "layout": 4, "padding": 4, "struct": 3, "structure": 2,
    },
}


SIGNAL_BY_TAG = {
    "cli_parser": "Keep command parsing deterministic; add parser matrix tests for every flag shape.",
    "memory_lifetime": "Prefer explicit ownership, bounded growth, and arena-backed per-turn scratch data.",
    "network_io": "Treat providers and workers as socket-like endpoints with framing, timeouts, and backpressure.",
    "process_unix": "Make worker spawn, signal handling, teardown, and recovery first-class contracts.",
    "security_ub": "Guard integer conversions, buffer sizes, pointer lifetimes, and untrusted tool inputs.",
    "error_cleanup": "Return structured errors with cleanup paths, retryability, and route/auth provenance.",
    "performance_locality": "Budget cold start, cache locality, registry size, and hot-loop allocations.",
    "linking_build": "Track object size, generated data size, symbol growth, and link-time failure modes.",
    "data_structures": "Choose data structures by access pattern, not convenience.",
    "api_design": "Keep module boundaries narrow, opaque, and testable.",
    "concurrency": "Use leases, heartbeats, atomic ownership, and failure-aware worker coordination.",
    "data_layout": "Design hot metadata for contiguous layout, alignment, and cache behavior.",
}


TOKEN_RE = re.compile(r"[a-z_][a-z0-9_]{2,}")
CODE_LINE_RE = re.compile(
    r"^\s*(#\s*(include|define|if|ifdef|endif)|"
    r"(static\s+)?(const\s+)?(unsigned\s+)?(int|char|void|long|size_t|struct|typedef)\b|"
    r"(if|for|while|switch)\s*\(|return\b|[a-zA-Z_][a-zA-Z0-9_]*\s*\(|[{};]\s*$)"
)


def run_cmd(argv: List[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv,
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def pdf_pages(path: Path) -> Tuple[int, str]:
    cp = run_cmd(["pdfinfo", str(path)])
    if cp.returncode != 0:
        raise RuntimeError(f"pdfinfo failed for {path}: {cp.stderr.strip()}")
    pages = 0
    title = ""
    for line in cp.stdout.splitlines():
        if line.startswith("Pages:"):
            pages = int(line.split(":", 1)[1].strip())
        elif line.startswith("Title:"):
            title = line.split(":", 1)[1].strip()
    if pages <= 0:
        raise RuntimeError(f"pdfinfo did not report pages for {path}")
    return pages, title


def pdf_text_pages(path: Path, expected_pages: int) -> List[str]:
    cp = run_cmd(["pdftotext", "-layout", "-enc", "UTF-8", str(path), "-"])
    if cp.returncode != 0:
        return [""] * expected_pages
    pages = cp.stdout.split("\f")
    if pages and pages[-1].strip() == "":
        pages = pages[:-1]
    if len(pages) < expected_pages:
        pages.extend([""] * (expected_pages - len(pages)))
    elif len(pages) > expected_pages:
        pages = pages[: expected_pages - 1] + ["\n".join(pages[expected_pages - 1 :])]
    return pages


def ocr_pdf_page(path: Path, page_no: int, dpi: int, lang: str) -> Tuple[str, str]:
    with tempfile.TemporaryDirectory(prefix="dsco-book-ocr-") as tmp:
        prefix = Path(tmp) / "page"
        render = run_cmd([
            "pdftoppm", "-f", str(page_no), "-l", str(page_no),
            "-r", str(dpi), "-gray", "-png", str(path), str(prefix),
        ])
        if render.returncode != 0:
            return "", "ocr_render_failed"
        images = sorted(Path(tmp).glob("page-*.png"))
        if not images:
            return "", "ocr_render_empty"
        ocr = run_cmd(["tesseract", str(images[0]), "stdout", "-l", lang])
        if ocr.returncode != 0:
            return "", "ocr_failed"
        return ocr.stdout, "ocr"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def normalize_space(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def tokens(text: str) -> List[str]:
    return [t for t in TOKEN_RE.findall(text.lower()) if t not in STOPWORDS]


def top_terms(text: str, limit: int) -> List[Dict[str, object]]:
    counts = collections.Counter(tokens(text))
    return [{"term": term, "count": count} for term, count in counts.most_common(limit)]


def tag_scores(term_counts: Counter[str]) -> Dict[str, int]:
    scores: Dict[str, int] = {}
    for tag, kws in TAG_KEYWORDS.items():
        score = 0
        for kw, weight in kws.items():
            score += term_counts.get(kw, 0) * weight
        if score > 0:
            scores[tag] = score
    return dict(sorted(scores.items(), key=lambda kv: (-kv[1], kv[0])))


def page_status(chars: int, min_page_chars: int) -> str:
    if chars == 0:
        return "empty"
    if chars < min_page_chars:
        return "low_text"
    return "ok"


def code_line_count(text: str) -> Tuple[int, int]:
    lines = [ln for ln in text.splitlines() if ln.strip()]
    code = sum(1 for ln in lines if CODE_LINE_RE.search(ln))
    return len(lines), code


def book_sort_key(path: Path) -> Tuple[int, str]:
    m = re.match(r"^(\d+)_", path.name)
    return (int(m.group(1)) if m else 9999, path.name)


def json_dump(obj: object) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"))


def write_jsonl(path: Path, rows: Iterable[dict]) -> None:
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, sort_keys=True, ensure_ascii=False) + "\n")


def init_db(path: Path) -> sqlite3.Connection:
    if path.exists():
        path.unlink()
    con = sqlite3.connect(path)
    con.executescript(
        """
        pragma journal_mode=wal;
        create table book (
            book_id integer primary key,
            file text not null,
            title text,
            pages integer not null,
            extracted_pages integer not null,
            chars integer not null,
            words integer not null,
            status text not null,
            pdf_sha256 text not null
        );
        create table page (
            page_id text primary key,
            book_id integer not null,
            file text not null,
            page integer not null,
            status text not null,
            needs_ocr integer not null,
            extraction_method text not null,
            chars integer not null,
            words integer not null,
            lines integer not null,
            code_lines integer not null,
            code_score real not null,
            text_sha256 text not null,
            top_terms_json text not null,
            tags_json text not null,
            dsco_signals_json text not null
        );
        create table page_term (
            page_id text not null,
            term text not null,
            count integer not null,
            primary key (page_id, term)
        );
        create table page_tag (
            page_id text not null,
            tag text not null,
            score integer not null,
            primary key (page_id, tag)
        );
        create index idx_page_book on page(book_id, page);
        create index idx_page_status on page(status);
        create index idx_page_tag_tag on page_tag(tag, score desc);
        create index idx_page_term_term on page_term(term, count desc);
        """
    )
    return con


def mine(args: argparse.Namespace) -> int:
    books_dir = Path(args.books_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    pdfs = sorted(books_dir.glob("*.pdf"), key=book_sort_key)
    if not pdfs:
        print(f"no PDFs found under {books_dir}", file=sys.stderr)
        return 2

    db_path = out_dir / "page_mining.sqlite"
    con = init_db(db_path)
    page_rows: List[dict] = []
    book_rows: List[dict] = []
    action_rows: List[dict] = []
    tag_pages: Dict[str, List[dict]] = collections.defaultdict(list)

    for book_id, pdf in enumerate(pdfs, start=1):
        pages_count, title = pdf_pages(pdf)
        text_pages = pdf_text_pages(pdf, pages_count)
        pdf_hash = sha256_file(pdf)
        book_chars = sum(len(normalize_space(p)) for p in text_pages)
        book_words = sum(len(tokens(p)) for p in text_pages)
        extracted_pages = sum(1 for p in text_pages if normalize_space(p))
        book_status = "ok"
        if book_chars == 0:
            book_status = "empty"
        elif book_chars < args.min_book_chars:
            book_status = "low_text"
        initial_book_status = book_status

        book_row = {
            "book_id": book_id,
            "file": str(pdf),
            "title": title,
            "pages": pages_count,
            "extracted_pages": extracted_pages,
            "chars": book_chars,
            "words": book_words,
            "status": book_status,
            "pdf_sha256": pdf_hash,
        }
        book_rows.append(book_row)
        con.execute(
            "insert into book values (?,?,?,?,?,?,?,?,?)",
            (
                book_id, str(pdf), title, pages_count, extracted_pages, book_chars,
                book_words, book_status, pdf_hash,
            ),
        )

        final_book_chars = 0
        final_book_words = 0
        final_extracted_pages = 0
        for page_no, text in enumerate(text_pages, start=1):
            compact = normalize_space(text)
            chars = len(compact)
            raw_status = page_status(chars, args.min_page_chars)
            extraction_method = "pdftotext"
            if args.ocr_missing and initial_book_status in {"empty", "low_text"} and raw_status != "ok":
                ocr_text, ocr_method = ocr_pdf_page(pdf, page_no, args.ocr_dpi, args.tesseract_lang)
                ocr_chars = len(normalize_space(ocr_text))
                if ocr_chars > chars:
                    text = ocr_text
                    extraction_method = ocr_method
                    compact = normalize_space(text)
                    chars = len(compact)
                else:
                    extraction_method = ocr_method if ocr_method != "ocr" else "ocr_attempted"

            term_counter: Counter[str] = collections.Counter(tokens(text))
            words = sum(term_counter.values())
            lines, code_lines = code_line_count(text)
            status = page_status(chars, args.min_page_chars)
            if extraction_method == "ocr":
                status = "ocr_ok" if status == "ok" else f"ocr_{status}"
            needs_ocr = 1 if (
                initial_book_status in {"empty", "low_text"} and
                raw_status != "ok" and
                not args.ocr_missing
            ) else 0
            terms = [{"term": term, "count": count} for term, count in term_counter.most_common(args.max_terms)]
            scores = tag_scores(term_counter)
            signals = [SIGNAL_BY_TAG[tag] for tag in scores if tag in SIGNAL_BY_TAG]
            page_id = f"{book_id:02d}:{page_no:04d}"
            if chars > 0:
                final_extracted_pages += 1
            final_book_chars += chars
            final_book_words += words
            page_row = {
                "page_id": page_id,
                "book_id": book_id,
                "book_file": str(pdf),
                "book_name": pdf.name,
                "page": page_no,
                "status": status,
                "needs_ocr": bool(needs_ocr),
                "extraction_method": extraction_method,
                "chars": chars,
                "words": words,
                "lines": lines,
                "code_lines": code_lines,
                "code_score": round((code_lines / lines) if lines else 0.0, 4),
                "text_sha256": hashlib.sha256(text.encode("utf-8", "replace")).hexdigest(),
                "top_terms": terms,
                "tags": scores,
                "dsco_signals": signals,
            }
            page_rows.append(page_row)

            con.execute(
                "insert into page values (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    page_id, book_id, str(pdf), page_no, status, needs_ocr, extraction_method, chars,
                    words, lines, code_lines, page_row["code_score"], page_row["text_sha256"],
                    json_dump(terms), json_dump(scores), json_dump(signals),
                ),
            )
            con.executemany(
                "insert into page_term values (?,?,?)",
                [(page_id, item["term"], item["count"]) for item in terms],
            )
            con.executemany(
                "insert into page_tag values (?,?,?)",
                [(page_id, tag, score) for tag, score in scores.items()],
            )
            for tag, score in scores.items():
                tag_pages[tag].append(
                    {
                        "tag": tag,
                        "score": score,
                        "page_id": page_id,
                        "book": pdf.name,
                        "page": page_no,
                        "top_terms": [item["term"] for item in terms[:6]],
                    }
                )
            if signals:
                action_rows.append(
                    {
                        "page_id": page_id,
                        "source": f"{pdf.name}:{page_no}",
                        "extraction_method": extraction_method,
                        "tags": scores,
                        "top_terms": [item["term"] for item in terms[:10]],
                        "dsco_signals": signals[:6],
                    }
                )

        final_book_status = "ok"
        if final_book_chars == 0:
            final_book_status = "empty"
        elif final_book_chars < args.min_book_chars:
            final_book_status = "low_text"
        elif initial_book_status in {"empty", "low_text"} and args.ocr_missing:
            final_book_status = "ocr_ok"
        book_row.update({
            "extracted_pages": final_extracted_pages,
            "chars": final_book_chars,
            "words": final_book_words,
            "status": final_book_status,
        })
        con.execute(
            "update book set extracted_pages=?, chars=?, words=?, status=? where book_id=?",
            (final_extracted_pages, final_book_chars, final_book_words, final_book_status, book_id),
        )

    con.commit()
    con.close()

    tag_index_rows = []
    for tag, rows in sorted(tag_pages.items()):
        rows.sort(key=lambda r: (-int(r["score"]), str(r["book"]), int(r["page"])))
        tag_index_rows.append(
            {
                "tag": tag,
                "page_count": len(rows),
                "top_pages": rows[: args.tag_top_pages],
            }
        )

    write_jsonl(out_dir / "page_features.jsonl", page_rows)
    write_jsonl(out_dir / "book_rollup.jsonl", book_rows)
    write_jsonl(out_dir / "tag_index.jsonl", tag_index_rows)
    write_jsonl(out_dir / "dsco_action_candidates.jsonl", action_rows)
    write_report(out_dir / "page_mining_report.md", book_rows, page_rows, tag_index_rows, action_rows, args)
    print(json.dumps(summary(book_rows, page_rows, tag_index_rows, action_rows, db_path), indent=2, sort_keys=True))
    return 0


def summary(book_rows: List[dict], page_rows: List[dict], tag_rows: List[dict],
            action_rows: List[dict], db_path: Path) -> dict:
    status_counts = collections.Counter(str(r["status"]) for r in page_rows)
    method_counts = collections.Counter(str(r.get("extraction_method", "unknown")) for r in page_rows)
    return {
        "books": len(book_rows),
        "pages": len(page_rows),
        "pages_with_text": sum(1 for r in page_rows if int(r["chars"]) > 0),
        "needs_ocr_pages": sum(1 for r in page_rows if r["needs_ocr"]),
        "page_status": dict(sorted(status_counts.items())),
        "extraction_methods": dict(sorted(method_counts.items())),
        "tag_count": len(tag_rows),
        "action_candidate_pages": len(action_rows),
        "db": str(db_path),
    }


def write_report(path: Path, book_rows: List[dict], page_rows: List[dict],
                 tag_rows: List[dict], action_rows: List[dict],
                 args: argparse.Namespace) -> None:
    status_counts = collections.Counter(str(r["status"]) for r in page_rows)
    method_counts = collections.Counter(str(r.get("extraction_method", "unknown")) for r in page_rows)
    total_chars = sum(int(r["chars"]) for r in page_rows)
    total_words = sum(int(r["words"]) for r in page_rows)
    generated = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    with path.open("w", encoding="utf-8") as f:
        f.write("# Page Mining Report\n\n")
        f.write(f"Generated: {generated}\n\n")
        f.write("This report is derived metadata only. It does not store raw page text.\n\n")
        f.write("## Summary\n\n")
        f.write(f"- Books: {len(book_rows)}\n")
        f.write(f"- Pages mined: {len(page_rows)}\n")
        f.write(f"- Pages with extracted text: {sum(1 for r in page_rows if int(r['chars']) > 0)}\n")
        f.write(f"- Pages marked needs_ocr: {sum(1 for r in page_rows if r['needs_ocr'])}\n")
        f.write(f"- Derived chars counted: {total_chars:,}\n")
        f.write(f"- Derived terms counted: {total_words:,}\n")
        f.write(f"- Page status counts: {dict(sorted(status_counts.items()))}\n")
        f.write(f"- Extraction method counts: {dict(sorted(method_counts.items()))}\n")
        f.write(f"- SQLite DB: `{Path(args.out_dir) / 'page_mining.sqlite'}`\n")
        f.write(f"- Page JSONL: `{Path(args.out_dir) / 'page_features.jsonl'}`\n")
        f.write(f"- Tag index JSONL: `{Path(args.out_dir) / 'tag_index.jsonl'}`\n\n")

        f.write("## Book Rollup\n\n")
        f.write("| # | Book | Pages | Extracted pages | Chars | Words | Status |\n")
        f.write("|---:|---|---:|---:|---:|---:|---|\n")
        for row in book_rows:
            f.write(
                f"| {row['book_id']} | `{Path(str(row['file'])).name}` | {row['pages']} | "
                f"{row['extracted_pages']} | {row['chars']:,} | {row['words']:,} | {row['status']} |\n"
            )

        f.write("\n## Tag Rollup\n\n")
        f.write("| Tag | Pages | Top source pointers |\n")
        f.write("|---|---:|---|\n")
        for row in tag_rows:
            pointers = ", ".join(
                f"`{p['book']}:{p['page']}`" for p in row["top_pages"][:5]
            )
            f.write(f"| `{row['tag']}` | {row['page_count']} | {pointers} |\n")

        f.write("\n## DSCO Action Themes\n\n")
        themes = collections.Counter()
        for row in action_rows:
            for signal in row["dsco_signals"]:
                themes[signal] += 1
        for signal, count in themes.most_common():
            f.write(f"- {signal} ({count} page pointers)\n")

        f.write("\n## Extraction Gaps\n\n")
        gaps = [r for r in book_rows if r["status"] != "ok"]
        if not gaps:
            f.write("- No book-level extraction gaps detected.\n")
        else:
            for row in gaps:
                f.write(
                    f"- `{Path(str(row['file'])).name}`: status={row['status']}, "
                    f"pages={row['pages']}, extracted_pages={row['extracted_pages']}, "
                    f"chars={row['chars']:,}. Needs OCR or a better text layer.\n"
                )

        f.write("\n## Query Examples\n\n")
        f.write("```sh\n")
        f.write("sqlite3 .workspace/book_mining/page_mining.sqlite \"select page_id,file,page,tags_json from page where tags_json like '%network_io%' limit 10;\"\n")
        f.write("sqlite3 .workspace/book_mining/page_mining.sqlite \"select term,sum(count) from page_term group by term order by sum(count) desc limit 25;\"\n")
        f.write("sqlite3 .workspace/book_mining/page_mining.sqlite \"select tag,count(*) from page_tag group by tag order by count(*) desc;\"\n")
        f.write("```\n")


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--books-dir", default=".workspace/books")
    parser.add_argument("--out-dir", default=".workspace/book_mining")
    parser.add_argument("--min-page-chars", type=int, default=80)
    parser.add_argument("--min-book-chars", type=int, default=10000)
    parser.add_argument("--max-terms", type=int, default=16)
    parser.add_argument("--tag-top-pages", type=int, default=25)
    parser.add_argument("--ocr-missing", action="store_true",
                        help="OCR pages from book-level low_text/empty PDFs using pdftoppm+tesseract")
    parser.add_argument("--ocr-dpi", type=int, default=180)
    parser.add_argument("--tesseract-lang", default="eng")
    return parser.parse_args(argv)


if __name__ == "__main__":
    raise SystemExit(mine(parse_args(sys.argv[1:])))
