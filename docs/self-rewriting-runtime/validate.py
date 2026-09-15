#!/usr/bin/env python3
"""Build/check the 120 source-grounded prompts. This never runs candidate code."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PARTS = ('native-manifest.json', 'evidence-manifest.json', 'reasoning-manifest.json',
         'advanced-native-manifest.json', 'learning-manifest.json', 'role-training-manifest.json')
FIELDS = {'id', 'title', 'slug', 'prompt', 'domain', 'primary_paths', 'donor_sources',
          'mechanism', 'challenge', 'hotspots', 'proposed_modules'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def local_path(base, relative):
    path = Path(relative)
    require(not path.is_absolute() and '..' not in path.parts, f'unsafe relative path: {relative}')
    path = (base / path).resolve()
    require(path.is_relative_to(base.resolve()), f'path escaped root: {relative}')
    return path


def declarations(path, text):
    result = {}
    if path.suffix == '.py':
        tree = ast.parse(text)
        def walk(node, scope=''):
            for child in ast.iter_child_nodes(node):
                if isinstance(child, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                    qualified = f'{scope}.{child.name}' if scope else child.name
                    result[qualified] = child.lineno
                    result.setdefault(child.name, child.lineno)
                    walk(child, qualified)
                else:
                    if isinstance(child, (ast.Assign, ast.AnnAssign)):
                        targets = child.targets if isinstance(child, ast.Assign) else [child.target]
                        for target in targets:
                            if isinstance(target, ast.Name):
                                result.setdefault(target.id, child.lineno)
                    walk(child, scope)
        walk(tree)
    return result


def collect():
    rows, sources, parsed = [], {}, {}
    for part in PARTS:
        data = json.loads((HERE / part).read_text())
        require(isinstance(data, list) and len(data) == 20, f'{part}: expected 20 prompts')
        rows.extend(data)
    rows.sort(key=lambda x: int(x['id']))
    require([x['id'] for x in rows] == [f'{n:02}' for n in range(1, 121)], 'expected unique IDs 01-120')
    used_paths, titles, prompt_hashes = set(), set(), set()
    for row in rows:
        require(set(row) == FIELDS, f"{row['id']}: missing/extra fields {set(row) ^ FIELDS}")
        require(row['title'] not in titles, 'duplicate title')
        titles.add(row['title'])
        require(re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', row['slug']), 'invalid slug')
        require(row['prompt'] == f"prompts/{row['id']}-{row['slug']}.md", 'prompt path mismatch')
        path = local_path(HERE, row['prompt'])
        body = path.read_text()
        count = len(body.split())
        require(200 <= count <= 400, f'{path.name}: {count} words (expected 200-400)')
        require('/Users/arthurcolle/Dsco/dsco-cli' in body, f'{path.name}: no target checkout')
        require('tools_execute_for_tier' in body, f'{path.name}: missing dynamic tool boundary')
        require('DSCO_NO_INSTALL=1' in body, f'{path.name}: missing no-install build')
        if int(row['id']) > 60:
            require('RLM' in body and 'REPL' in body, f'{path.name}: missing role RLM contract')
            require(re.search(r'checkpoint|adapter', body, re.I), f'{path.name}: missing trained artifact')
            require(re.search(r'held[ -]?out|hold[ -]?out|unseen', body, re.I), f'{path.name}: missing independent evaluation')
        require(row['mechanism'].strip() and row['challenge'].strip(), f'{path.name}: no mechanism/challenge')
        row['word_count'] = count
        row['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        require(row['sha256'] not in prompt_hashes, 'identical prompts')
        prompt_hashes.add(row['sha256'])
        used_paths.add(path)
        for key in ('primary_paths', 'hotspots', 'proposed_modules'):
            require(isinstance(row[key], list), f'{path.name}: {key} must be a list')
            for entry in row[key]:
                target = local_path(ROOT, entry)
                if key != 'proposed_modules':
                    require(target.exists(), f'{path.name}: missing C source {entry}')
                    sources[str(target)] = {'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
                                            'kind': 'current-dsco-source'}
        require(row['donor_sources'], f'{path.name}: missing donor source')
        for source in row['donor_sources']:
            donor = Path(source['path'])
            require(donor.is_absolute() and donor.is_file(), f'{path.name}: missing donor {donor}')
            require(source['observation'].strip(), f'{path.name}: missing observed donor mechanism')
            raw = donor.read_bytes()
            text = raw.decode('utf-8')
            if str(donor) not in parsed:
                parsed[str(donor)] = declarations(donor, text)
            symbols = parsed[str(donor)]
            lines = {}
            for symbol in source['symbols']:
                require(symbol in symbols, f'{path.name}: undeclared donor symbol {symbol} in {donor}')
                lines[symbol] = symbols[symbol]
            require(lines, f'{path.name}: missing named donor symbol')
            entry = sources.setdefault(str(donor), {'sha256': hashlib.sha256(raw).hexdigest(),
                                                    'kind': 'donor-source', 'symbols': {}})
            entry['symbols'].update(lines)
            source['source_sha256'] = entry['sha256']
            source['symbol_lines'] = lines
        row['standalone'] = True
    actual = {p.resolve() for p in (HERE / 'prompts').glob('*.md')}
    require(used_paths == actual, 'extra or missing prompt files')
    return rows, dict(sorted(sources.items()))


def generate(rows, sources):
    words = [x['word_count'] for x in rows]
    catalog = {'schema': 'dsco.resident-evolution.prompts/v3', 'status': 'implementation-specifications',
               'target': 'resident self-rewriting native reasoning runtime',
               'prompt_count': len(rows), 'min_words': min(words), 'max_words': max(words),
               'total_words': sum(words), 'word_count_method': 'whitespace split, including title',
               'runtime_implemented_by_this_package': False, 'features': rows}
    header = """# A self-rewriting binary that reasons and improves

120 implementation prompts grounded in the supplied DSPy, cognitive-agent and metaprogramming source. Prompts 01–60 establish the resident self-rewriting runtime. The 60 new prompts, 61–120, extend it into a system where each operational role trains its own Recursive Language Model (RLM), with executable trajectories, independent evaluation, and versioned model weights or adapters.

The complete collection now contains **3,120 prompts**: these 120 foundations plus [3,000 specialist role-feature prompts, 121–3120](expansion/README.md). The expansion has its own searchable catalog, role readers and validator.

Each prompt is independently usable: copy its entire file into a session opened in `/Users/arthurcolle/Dsco/dsco-cli` or an assigned private worktree. It contains its own source references, mechanism and falsifying acceptance case. For coordinated work, bind the shared interfaces described in the delivery design. These are specifications; this package does not implement or activate the native runtime.

- [The 60 new role-RLM prompts, 61–120](PROMPTS_061_120.md)
- [3,000 additional prompts, 121–3120: specialist roles × feature mechanisms](expansion/README.md)
- [All 120 prompts in one document](PROMPTS.md)
- [How each role trains an RLM](ROLE_RLM_TRAINING.md)
- [Native runtime architecture and first proof](ARCHITECTURE.md)
- [Multi-agent delivery and integration](DELIVERY.md)
- [What the supplied source actually implements](SOURCE_AUDIT.md)
- [Machine-readable catalog](catalog.json) and [source hashes/symbols](sources.json)

The first milestone is an inference-proposed correction to a native procedure, executed from new machine-code bytes with resident heap/session continuity, no `exec`, independent acceptance and a working rollback path. Changing prompts or preserving the PID alone does not prove that milestone.

"""
    header += f'Validated: **{len(rows)} prompts, {min(words)}–{max(words)} words each; {sum(words):,} words total.**\n\n'
    header += '## The six parts\n\n'
    header += '| IDs | Purpose |\n| --- | --- |\n| 01–20 | Executable self-model, code generation, ABI, state migration, live activation and evolution |\n| 21–40 | Evidence, retrieval, memory, experiments and cognitive result flow |\n| 41–60 | Reasoning procedures, causal tests, calibration and learned improvement strategies |\n\n'
    header += '| New IDs | Purpose |\n| --- | --- |\n| 61–80 | Trained native-runtime specialist RLMs |\n| 81–100 | Trained learning and evidence specialist RLMs |\n| 101–120 | Per-role RLM environments, datasets, training, serving and joint admission |\n\n'
    header += '## Prompt index\n\n| ID | Feature | Words | Falsifying challenge |\n| --- | --- | ---: | --- |\n'
    for row in rows:
        challenge = row['challenge'].replace('|', '\\|').replace('\n', ' ')
        header += f"| {row['id']} | [{row['title']}]({row['prompt']}) | {row['word_count']} | {challenge} |\n"
    header += '\n## Recheck the package\n\n```sh\npython3 docs/self-rewriting-runtime/validate.py\n```\n\nUse `--refresh` only after intentionally changing prompt/source references; it rebuilds the catalog, combined document and source hashes. `--zip` packages the validated documents. Neither option starts models or executes candidate code. Source hashes detect later drift; structural validation does not prove cognitive or native-code correctness.\n'
    bundle = '# 120 prompts for a resident self-rewriting reasoning runtime\n\nEach numbered section is one complete standalone implementation prompt.\n\n'
    bundle += '\n\n---\n\n'.join((HERE / row['prompt']).read_text().rstrip() for row in rows) + '\n'
    additions = '# 60 new prompts: each role trains an RLM\n\nPrompts 61–120. Copy one complete numbered prompt into an independent implementation session. These are specifications, not completed training runs.\n\n'
    additions += '\n\n---\n\n'.join((HERE / row['prompt']).read_text().rstrip() for row in rows if int(row['id']) > 60) + '\n'
    return {'README.md': header, 'PROMPTS.md': bundle, 'PROMPTS_061_120.md': additions,
            'catalog.json': json.dumps(catalog, indent=2, ensure_ascii=False) + '\n',
            'sources.json': json.dumps({'schema': 'dsco.source-audit/v1', 'sources': sources}, indent=2) + '\n'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh', action='store_true')
    parser.add_argument('--zip', action='store_true')
    args = parser.parse_args()
    rows, sources = collect()
    for name, body in generate(rows, sources).items():
        path = HERE / name
        if args.refresh:
            path.write_text(body)
        else:
            require(path.exists() and path.read_text() == body, f'{name}: stale; inspect drift before --refresh')
    for doc in HERE.rglob('*.md'):
        for link in re.findall(r'\]\(([^)]+)\)', doc.read_text()):
            if '://' not in link and not link.startswith('#'):
                require((doc.parent / link.split('#')[0]).exists(), f'{doc.name}: broken link {link}')
    split_path = HERE / 'evaluation' / 'family-splits.json'
    splits = json.loads(split_path.read_text())
    require(splits['status'] == 'ok' and not splits['errors'], 'invalid family partition blueprint')
    families = [json.loads(line) for line in (HERE / 'evaluation' / 'task-families.jsonl').read_text().splitlines() if line.strip()]
    require({f['item_id'] for f in families} == {f['item_id'] for f in splits['items']}, 'family partition inventory mismatch')
    require(len(splits['items']) == len({f['item_id'] for f in splits['items']}), 'duplicate evaluation families')
    for family in splits['items']:
        bucket = int.from_bytes(hashlib.sha256(f"{splits['seed']}\0{family['item_id']}".encode()).digest()[:8], 'big') / 2**64
        expected = 'final_holdout' if bucket < splits['fractions']['holdout'] else 'calibration' if bucket < sum(splits['fractions'].values()) else 'development'
        require(family['split'] == expected, f"split changed for {family['item_id']}")
    for role in {f['domain'] for f in splits['items']}:
        require({f['split'] for f in splits['items'] if f['domain'] == role} == {'development', 'calibration', 'final_holdout'}, f'{role}: missing partition')
    if args.zip:
        archive = HERE.with_suffix('.zip')
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for path in sorted(HERE.rglob('*')):
                if path.is_file() and path.suffix in ('.md', '.py', '.json', '.jsonl') and '__pycache__' not in path.parts:
                    z.write(path, path.relative_to(HERE.parent))
        with zipfile.ZipFile(archive) as z:
            require(z.testzip() is None, 'archive corruption')
    print(json.dumps({'passed': True, 'prompts': len(rows),
                      'min_words': min(r['word_count'] for r in rows),
                      'max_words': max(r['word_count'] for r in rows),
                      'total_words': sum(r['word_count'] for r in rows),
                      'verified_source_files': len(sources), 'runtime_executed': False}))


if __name__ == '__main__':
    main()
