#!/usr/bin/env python3
"""Search the RLM prompt catalog or export complete standalone prompts."""
import argparse
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--query', help='All whitespace-separated terms must match catalog metadata')
    parser.add_argument('--role', help='Role ID, exact slug, or case-insensitive name fragment')
    parser.add_argument('--feature', help='Feature ID, exact slug, or case-insensitive name fragment')
    parser.add_argument('--ids', help='Comma-separated IDs or inclusive ranges, e.g. 121,125-129')
    parser.add_argument('--limit', type=int, help='Explicitly limit the matching set')
    parser.add_argument('--list', action='store_true', help='List matching metadata (default without --output)')
    parser.add_argument('--output', type=Path, help='Export full prompts as Markdown; use - for stdout')
    parser.add_argument('--force', action='store_true', help='Replace an existing exported file')
    args = parser.parse_args()
    if args.limit is not None and args.limit <= 0:
        parser.error('--limit must be positive')
    catalog = json.loads((HERE / 'catalog.json').read_text())
    wanted = None
    if args.ids:
        wanted = set()
        for token in args.ids.split(','):
            match = re.fullmatch(r'\s*(\d+)(?:-(\d+))?\s*', token)
            if not match:
                parser.error('invalid ID range')
            lo, hi = int(match[1]), int(match[2] or match[1])
            if not 121 <= lo <= hi <= 3120:
                parser.error('IDs must be in 121-3120, with increasing range endpoints')
            wanted.update(range(lo, hi + 1))
    roles = {r['id']: r for r in catalog['roles']}
    modes = {m['id']: m for m in catalog['modes']}
    def matches(value, identity):
        value = value.casefold()
        return value in (identity['id'], identity['slug'].casefold()) or (value.isdigit() and int(value) == int(identity['id'])) or value in identity['name'].casefold()
    rows = []
    for row in catalog['prompts']:
        if wanted is not None and row['id'] not in wanted:
            continue
        if args.role and not matches(args.role, roles[row['role_id']]):
            continue
        if args.feature and not matches(args.feature, modes[row['mode_id']]):
            continue
        searchable = json.dumps(row, ensure_ascii=False).casefold()
        if args.query and not all(term.casefold() in searchable for term in args.query.split()):
            continue
        rows.append(row)
    matched = len(rows)
    if args.limit:
        rows = rows[:args.limit]
    if not rows:
        print('No matching prompts.', file=sys.stderr)
        return 1
    if args.output is not None:
        if args.list:
            parser.error('--list and --output cannot be combined')
        parts = [(HERE / row['prompt']).read_text().rstrip() for row in rows]
        body = '# Selected role RLM implementation prompts\n\n' + '\n\n---\n\n'.join(parts) + '\n'
        if str(args.output) == '-':
            print(body, end='')
        else:
            destination = args.output.resolve()
            if destination.is_relative_to(HERE.parent):
                parser.error('export outside the managed prompt package')
            try:
                with destination.open('w' if args.force else 'x') as stream:
                    stream.write(body)
            except FileExistsError:
                parser.error('output exists; choose a new path or pass --force')
            print(f'Exported {len(rows)} complete prompts to {destination}')
    else:
        for row in rows:
            print(f"{row['id']:4}  [{row['role_id']}/{row['mode_id']}]  {row['title']}  ({row['word_count']} words)")
    if matched > len(rows):
        print(f'Showing {len(rows)} of {matched} matches because --limit was supplied.', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
