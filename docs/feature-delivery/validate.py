#!/usr/bin/env python3
"""Validate and assemble this static feature catalog. Never dispatches workers."""
import argparse
import hashlib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PARTS = ('runtime-manifest.json', 'tools-manifest.json', 'fleet-manifest.json')
FIELDS = {'id', 'title', 'slug', 'prompt', 'domain', 'primary_paths',
          'proposed_modules', 'hotspots', 'acceptance'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def within(base, relative):
    path = Path(relative)
    require(not path.is_absolute() and '..' not in path.parts, f'unsafe path: {relative}')
    target = (base / path).resolve()
    require(target.is_relative_to(base.resolve()), f'path escapes root: {relative}')
    return target


def assemble():
    rows = []
    for part in PARTS:
        data = json.loads((HERE / part).read_text())
        require(isinstance(data, list) and len(data) == 20, f'{part}: expected 20 rows')
        rows.extend(data)
    rows.sort(key=lambda row: row['id'])
    require([r['id'] for r in rows] == [f'{n:02}' for n in range(1, 61)],
            'IDs must be unique and exactly 01 through 60')
    seen_titles, seen_slugs, seen_modules = set(), set(), {}
    files = set()
    for row in rows:
        require(set(row) == FIELDS, f"{row['id']}: unexpected or missing fields")
        for key in ('id', 'title', 'slug', 'prompt', 'domain', 'acceptance'):
            require(isinstance(row[key], str) and row[key].strip(), f'{key}: empty field')
        require(re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', row['slug']), 'invalid slug')
        require(row['title'].casefold() not in seen_titles, 'duplicate title')
        require(row['slug'] not in seen_slugs, 'duplicate slug')
        seen_titles.add(row['title'].casefold())
        seen_slugs.add(row['slug'])
        expected = f"prompts/{row['id']}-{row['slug']}.md"
        require(row['prompt'] == expected, f'noncanonical prompt path: {expected}')
        path = within(HERE, row['prompt'])
        data = path.read_bytes()
        text = data.decode('utf-8')
        words = len(text.split())
        require(200 <= words <= 400, f'{path.name}: {words} words; expected 200-400')
        require(text.startswith(f"# {row['id']}"), f'{path.name}: missing ID heading')
        require('/Users/arthurcolle/Dsco/dsco-cli' in text, f'{path.name}: missing repo context')
        require('tools_execute_for_tier' in text, f'{path.name}: missing governed tool boundary')
        require('DSCO_NO_INSTALL=1' in text, f'{path.name}: missing no-install build guard')
        require('propos' in text.lower(), f'{path.name}: proposed status missing')
        for key in ('primary_paths', 'proposed_modules', 'hotspots'):
            require(isinstance(row[key], list) and (row[key] or key == 'hotspots'),
                    f'{path.name}: invalid {key}')
            require(len(row[key]) == len(set(row[key])), f'{path.name}: duplicate {key}')
            for entry in row[key]:
                target = within(ROOT, entry)
                if key != 'proposed_modules':
                    require(target.exists(), f'{path.name}: nonexistent current path {entry}')
                else:
                    require(entry not in seen_modules,
                            f'{entry}: proposed module collision with {seen_modules.get(entry)}')
                    seen_modules[entry] = row['id']
        files.add(path)
        row['word_count'] = words
        row['sha256'] = hashlib.sha256(data).hexdigest()
        row['dependencies'] = []
        row['suggested_batch'] = (int(row['id']) - 1) % 20 + 1
    require(files == {p.resolve() for p in (HERE / 'prompts').glob('*.md')},
            'unlisted or missing prompt files')
    config = json.loads((HERE / 'delivery-config.json').read_text())
    require(config['dispatch_enabled'] is False and config['coordinator_implemented'] is False,
            'this package is a design, not a dispatcher')
    require(config['worker_environment']['DSCO_NO_INSTALL'] == '1', 'worker install guard missing')
    return rows


def outputs(rows):
    counts = [r['word_count'] for r in rows]
    catalog = {
        'schema': 'dsco.feature-catalog/v1', 'status': 'proposed-features',
        'source_checkout': '/Users/arthurcolle/Dsco/dsco-cli',
        'prompt_count': len(rows), 'word_count_method': 'UTF-8 text split on whitespace; includes title',
        'min_words': min(counts), 'max_words': max(counts), 'total_words': sum(counts),
        'batch_semantics': 'Suggested selection order only; no dependency or conflict-free guarantee',
        'features': rows,
    }
    intro = """# 60 independent feature prompts and concurrent delivery design

This package proposes 60 bounded features for dsco-cli. Each linked file is a complete, standalone implementation prompt: copy the entire file into a fresh session opened in the repository or an assigned private worktree. The prompts require checking current code before implementing a missing behavior, because the checkout already contains substantial work beyond the older roadmap.

The features are not implemented by this package. The concurrent coordinator is designed, not deployed. Validation here checks the catalog, source references, hashes and word counts; it does not prove feature correctness or the future coordinator's runtime guarantees.

- [All 60 prompts in one document](PROMPTS.md)
- [Concurrent delivery system](DELIVERY_SYSTEM.md)
- [Machine-readable feature catalog](catalog.json)
- [Proposed delivery configuration](delivery-config.json)
- [Candidate contract example](candidate.example.json)
- [Prime Intellect evaluation and learning integration](PRIME_INTELLECT_LEARNING.md)

Start with one coordinator and three implementation sessions in isolated worktrees, using the existing blackboard for atomic claims and acceptance. Rotate a free session into independent review, and keep a single integration writer. Explicit no-install builds prevent feature workers from overwriting the user's executable. The design explains how to freeze the dirty baseline, coordinate shared hooks, recover crashed attempts and verify merged revisions.

For a single independent session, choose any prompt below. For coordinated delivery, the coordinator adds baseline, scope, frozen checks, resource budget and attempt identity. No numbered prompt requires another numbered prompt to be completed first.

Run the static validation from the repository root:

```sh
python3 docs/feature-delivery/validate.py
```

After intentionally editing the source prompt files or the three domain manifests, regenerate the catalog and combined document with `python3 docs/feature-delivery/validate.py --refresh`, then validate again. This command only writes generated documents in this package; it never launches agents.

"""
    intro += f"Verified catalog: **60 prompts, {min(counts)}–{max(counts)} words each, {sum(counts):,} words total.** Counts include each prompt's title.\n\n"
    intro += '| ID | Feature | Domain | Words |\n| --- | --- | --- | ---: |\n'
    for row in rows:
        intro += f"| {row['id']} | [{row['title']}]({row['prompt']}) | {row['domain']} | {row['word_count']} |\n"
    intro += '\n## Suggested three-worker batches\n\nThese mix domains to distribute work. They are selection suggestions, not dependency waves or a guarantee of nonoverlapping shared hooks. Check reservations and actual diff scope before dispatch. Priorities may change after baseline discovery.\n\n'
    intro += '| Batch | Worker A | Worker B | Worker C |\n| --- | --- | --- | --- |\n'
    for n in range(1, 21):
        chosen = [rows[n - 1], rows[n + 19], rows[n + 39]]
        intro += f'| {n:02} | ' + ' | '.join(f"[{r['id']}]({r['prompt']}) {r['title']}" for r in chosen) + ' |\n'
    bundle = '# 60 independent dsco-cli feature prompts\n\nEach numbered section below is a complete standalone prompt. Copy one section into its own implementation session. Proposed commands are specifications, not claims of existing interfaces.\n\n'
    bundle += '\n\n---\n\n'.join((HERE / row['prompt']).read_text().rstrip() for row in rows) + '\n'
    return {'catalog.json': json.dumps(catalog, indent=2, ensure_ascii=False) + '\n',
            'README.md': intro, 'PROMPTS.md': bundle}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh', action='store_true', help='Regenerate catalog, index and combined prompts')
    args = parser.parse_args()
    rows = assemble()
    for name, content in outputs(rows).items():
        path = HERE / name
        if args.refresh:
            path.write_text(content)
        else:
            require(path.exists() and path.read_text() == content, f'{name}: stale; run --refresh')
    # Relative document links are part of the delivered artifact contract.
    for name in ('README.md', 'DELIVERY_SYSTEM.md'):
        for target in re.findall(r'\]\(([^)]+)\)', (HERE / name).read_text()):
            if '://' not in target and not target.startswith('#'):
                require((HERE / target.split('#')[0]).exists(), f'{name}: broken link {target}')
    words = [r['word_count'] for r in rows]
    print(json.dumps({'passed': True, 'prompts': len(rows), 'min_words': min(words),
                      'max_words': max(words), 'total_words': sum(words),
                      'independent': all(not r['dependencies'] for r in rows),
                      'dispatch_enabled': False}))


if __name__ == '__main__':
    main()
