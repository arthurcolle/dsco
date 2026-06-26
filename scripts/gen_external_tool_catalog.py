#!/usr/bin/env python3
"""Generate a catalog of external app/tool integrations from local Codex caches."""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter
from pathlib import Path
from typing import Any


def truthy(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on", "enabled"}
    return False


def as_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value] if value else []
    if isinstance(value, list):
        return [str(v) for v in value if str(v)]
    if isinstance(value, dict):
        return [str(k) for k, v in value.items() if truthy(v)]
    return [str(value)]


def first_str(obj: dict[str, Any], *keys: str) -> str:
    for key in keys:
        value = obj.get(key)
        if isinstance(value, str) and value:
            return value
    return ""


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8", errors="ignore") as fh:
        return json.load(fh)


def catalog_entries(data: Any) -> list[dict[str, Any]]:
    if isinstance(data, list):
        return [x for x in data if isinstance(x, dict)]
    if not isinstance(data, dict):
        return []
    for key in ("connectors", "apps", "items"):
        value = data.get(key)
        if isinstance(value, list):
            return [x for x in value if isinstance(x, dict)]
    return []


def normalize_catalog_entry(raw: dict[str, Any]) -> dict[str, Any]:
    meta = raw.get("appMetadata") if isinstance(raw.get("appMetadata"), dict) else {}
    labels = set(as_list(raw.get("labels")))
    labels.update(k for k in ("interactive", "consequential", "retrievable", "sync") if truthy(raw.get(k)))

    categories = as_list(raw.get("categories") or raw.get("category"))
    if not categories and isinstance(meta, dict):
        categories = as_list(meta.get("categories") or meta.get("category"))

    status = first_str(raw, "catalog_status", "catalogStatus", "status")
    review = meta.get("review") if isinstance(meta, dict) else None
    if not status and isinstance(review, dict):
        status = str(review.get("status") or "")
    if not status:
        status = "cached"

    name = first_str(raw, "display_name", "displayName", "title", "name", "id")
    connector_id = first_str(raw, "connector_id", "connectorId", "slug", "id", "app_id") or name
    entry_id = first_str(raw, "id", "app_id", "name") or connector_id

    return {
        "name": name,
        "id": entry_id,
        "connector_id": connector_id,
        "distribution": first_str(raw, "distribution_channel", "distributionChannel", "distribution", "source") or "codex_app_directory",
        "categories": sorted(categories),
        "labels": sorted(labels),
        "enabled": truthy(raw.get("isEnabled", raw.get("is_enabled", False))),
        "accessible": truthy(raw.get("isAccessible", raw.get("is_accessible", False))),
        "status": status,
        "description": str(raw.get("description") or ""),
    }


def default_catalog_paths() -> list[Path]:
    paths: list[Path] = []
    env = os.environ.get("DSCO_CODEX_APP_DIRECTORY")
    if env:
        paths.append(Path(env).expanduser())
    home = Path.home()
    paths.append(home / ".dsco" / "codex_app_directory.json")
    paths.extend(sorted((home / ".codex" / "cache" / "codex_app_directory").glob("*.json")))
    return paths


def default_plugin_dir() -> Path:
    return Path.home() / ".codex" / ".tmp" / "plugins" / "plugins"


def load_catalog(paths: list[Path]) -> tuple[Path | None, list[dict[str, Any]]]:
    for path in paths:
        if not path.exists():
            continue
        entries = [normalize_catalog_entry(x) for x in catalog_entries(read_json(path))]
        entries = [x for x in entries if x["name"] or x["id"]]
        if entries:
            return path, sorted(entries, key=lambda x: (x["name"].lower(), x["id"]))
    return None, []


def load_plugin_apps(plugin_dir: Path) -> list[dict[str, str]]:
    apps: list[dict[str, str]] = []
    if not plugin_dir.exists():
        return apps
    for path in sorted(plugin_dir.glob("*/.app.json")):
        try:
            data = read_json(path)
        except (OSError, json.JSONDecodeError):
            continue
        raw_apps = data.get("apps") if isinstance(data, dict) else None
        if not isinstance(raw_apps, dict):
            continue
        for slug, meta in raw_apps.items():
            app_id = ""
            if isinstance(meta, dict):
                app_id = str(meta.get("id") or "")
            apps.append({"slug": str(slug), "id": app_id, "path": str(path)})
    return apps


def md_escape(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ").strip()


def build_markdown(source: Path | None, entries: list[dict[str, Any]], plugins: list[dict[str, str]]) -> str:
    label_counts = Counter(label for e in entries for label in e["labels"])
    category_counts = Counter(cat for e in entries for cat in e["categories"])
    enabled = sum(1 for e in entries if e["enabled"])
    accessible = sum(1 for e in entries if e["accessible"])

    lines = [
        "# External Tool and Integration Catalog",
        "",
        "Generated by `scripts/gen_external_tool_catalog.py`. Do not hand-edit generated counts; rerun `make docs` after refreshing the Codex app directory cache.",
        "",
        "This catalog is a map of cached external app/integration entries, not a guarantee that every entry is currently installed, authenticated, or live-callable. Use `discover_tools` for live built-in/MCP tools and `discover_integrations` for runtime catalog status.",
        "",
        "## Summary",
        "",
        f"- Source: `{source}`" if source else "- Source: unavailable on this machine",
        f"- Cached app-directory entries: {len(entries)}",
        f"- Enabled catalog entries: {enabled}",
        f"- Accessible catalog entries: {accessible}",
        f"- Curated plugin app manifests: {len(plugins)}",
        "",
        "## Label Counts",
        "",
        "| Label | Entries |",
        "|---|---:|",
    ]
    if label_counts:
        for label, count in sorted(label_counts.items(), key=lambda item: (-item[1], item[0])):
            lines.append(f"| `{md_escape(label)}` | {count} |")
    else:
        lines.append("| none | 0 |")

    lines.extend(["", "## Top Categories", "", "| Category | Entries |", "|---|---:|"])
    if category_counts:
        for category, count in sorted(category_counts.items(), key=lambda item: (-item[1], item[0]))[:50]:
            lines.append(f"| `{md_escape(category)}` | {count} |")
    else:
        lines.append("| none | 0 |")

    lines.extend(
        [
            "",
            "## Cached App Directory",
            "",
            "| Name | Connector ID | Categories | Labels | Enabled | Accessible | Status |",
            "|---|---|---|---:|---:|---:|---|",
        ]
    )
    for e in entries:
        lines.append(
            "| "
            f"{md_escape(e['name'])} | "
            f"`{md_escape(e['connector_id'])}` | "
            f"{md_escape(', '.join(e['categories']))} | "
            f"{md_escape(', '.join(e['labels']))} | "
            f"{'yes' if e['enabled'] else ''} | "
            f"{'yes' if e['accessible'] else ''} | "
            f"{md_escape(e['status'])} |"
        )

    lines.extend(
        [
            "",
            "## Curated Plugin App Manifests",
            "",
            "| Slug | App ID |",
            "|---|---|",
        ]
    )
    if plugins:
        for app in plugins:
            lines.append(f"| `{md_escape(app['slug'])}` | `{md_escape(app['id'])}` |")
    else:
        lines.append("| none |  |")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repo root")
    parser.add_argument("--out", default="docs/EXTERNAL_TOOL_CATALOG.md", help="Markdown output")
    parser.add_argument("--catalog", action="append", default=[], help="catalog JSON path; can be passed more than once")
    parser.add_argument("--plugin-dir", default=str(default_plugin_dir()), help="curated plugin app manifest directory")
    parser.add_argument("--check", action="store_true", help="verify generated output without rewriting")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    out_path = root / args.out
    paths = [Path(p).expanduser() for p in args.catalog] or default_catalog_paths()
    source, entries = load_catalog(paths)
    plugins = load_plugin_apps(Path(args.plugin_dir).expanduser())

    if args.check and source is None and not plugins and out_path.exists():
        print("external tool catalog check skipped: no local Codex app cache found", file=sys.stderr)
        return 0

    generated = build_markdown(source, entries, plugins)
    if args.check:
        current = out_path.read_text(errors="ignore") if out_path.exists() else ""
        if current != generated:
            print(
                "docs drift: external tool catalog is out of date. Run "
                "python3 scripts/gen_external_tool_catalog.py --root .",
                file=sys.stderr,
            )
            return 1
        return 0

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(generated)
    print(f"wrote {out_path.relative_to(root)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
