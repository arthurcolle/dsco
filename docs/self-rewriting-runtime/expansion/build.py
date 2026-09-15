#!/usr/bin/env python3
"""Render and validate 3,000 role-specific RLM implementation prompts; never train models."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import zipfile

HERE = Path(__file__).resolve().parent
BASE = HERE.parent
REPO = BASE.parent.parent
PROFILE_PARTS = ('roles-native.json', 'roles-evidence.json', 'roles-operations.json')
CONTENT = ('brief', 'context', 'program', 'data', 'oracle', 'failure', 'holdout', 'artifact')
PROFILE_FIELDS = set(CONTENT) | {'id', 'name', 'slug', 'primary_paths', 'donor_sources'}
MODE_FIELDS = {'id', 'slug', 'name', 'mechanism', 'acceptance'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    return json.loads(path.read_text())


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'))


def slug_ok(value):
    return bool(re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', value))


def load_inputs():
    spec = importlib.util.spec_from_file_location('foundation_validator', BASE / 'validate.py')
    foundation = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(foundation)
    roles = [row for name in PROFILE_PARTS for row in read_json(HERE / name)]
    roles.sort(key=lambda r: int(r['id']))
    modes = read_json(HERE / 'modes.json')
    require([r['id'] for r in roles] == [f'{n:03}' for n in range(1, 101)], 'expected roles 001-100')
    require([m['id'] for m in modes] == [f'{n:02}' for n in range(1, 31)], 'expected modes 01-30')
    require(len({r['name'] for r in roles}) == 100, 'duplicate role names')
    require(len({r['slug'] for r in roles}) == 100, 'duplicate role slugs')
    require(len({m['slug'] for m in modes}) == 30, 'duplicate mode slugs')
    require(len({r['failure'] for r in roles}) == 100, 'duplicate domain counterexamples')
    require(len({r['oracle'] for r in roles}) == 100, 'duplicate domain oracles')
    sources, ast_cache = {}, {}
    for role in roles:
        require(set(role) == PROFILE_FIELDS, f"role {role['id']}: schema mismatch {set(role) ^ PROFILE_FIELDS}")
        require(slug_ok(role['slug']), 'invalid role slug')
        for key in CONTENT:
            require(isinstance(role[key], str) and role[key].strip(), f"{role['id']}: empty {key}")
        words = sum(len(role[key].split()) for key in CONTENT)
        require(95 <= words <= 135, f"role {role['id']}: profile length {words}, expected 95-135")
        for relative in role['primary_paths']:
            target = foundation.local_path(REPO, relative)
            require(target.is_file(), f'missing C anchor {relative}')
            sources[str(target)] = {'sha256': digest(target.read_bytes()), 'kind': 'dsco-source'}
        require(len(role['primary_paths']) >= 2, 'need two concrete integration anchors')
        require(role['donor_sources'], 'missing donor source')
        for donor in role['donor_sources']:
            require(set(donor) == {'path', 'symbols', 'observation'}, 'donor schema mismatch')
            path = Path(donor['path'])
            require(path.is_absolute() and path.is_file(), f'missing donor {path}')
            require(donor['observation'].strip(), 'missing observed donor behavior')
            if str(path) not in ast_cache:
                raw = path.read_bytes()
                ast_cache[str(path)] = foundation.declarations(path, raw.decode('utf-8'))
                sources[str(path)] = {'sha256': digest(raw), 'kind': 'donor-source', 'symbols': {}}
            for symbol in donor['symbols']:
                require(symbol in ast_cache[str(path)], f'unknown donor symbol {symbol} in {path}')
                sources[str(path)]['symbols'][symbol] = ast_cache[str(path)][symbol]
            require(donor['symbols'], 'missing named donor symbol')
    for mode in modes:
        require(set(mode) == MODE_FIELDS and slug_ok(mode['slug']), 'mode schema/slug invalid')
        require(55 <= len((mode['mechanism'] + ' ' + mode['acceptance']).split()) <= 90, 'mode content length outside contract')
    foundation_hashes = read_json(HERE / 'foundation-hashes.json')
    require(len(foundation_hashes) == 120, 'foundation snapshot incomplete')
    require(set(foundation_hashes) == {p.name for p in (BASE / 'prompts').glob('*.md')}, 'foundation file inventory changed')
    for name, expected in foundation_hashes.items():
        require(digest((BASE / 'prompts' / name).read_bytes()) == expected, f'foundation prompt changed: {name}')
    return roles, modes, dict(sorted(sources.items()))


def sentence(value):
    return value.rstrip().rstrip('.') + '.'


def render(role, mode, number):
    donor = role['donor_sources'][0]
    title = f"{role['name']}: {mode['name']}"
    body = f"# {number} — {title}\n\n"
    body += f"Implement the {mode['name'].lower()} feature for the {role['name']} RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. {sentence(role['brief'])}\n\n"
    body += f"{sentence(role['context'])} The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. {sentence(role['program'])} Role output: {sentence(role['artifact'])} Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.\n\n"
    body += mode['mechanism'] + '\n\n'
    body += f"{sentence(role['data'])} {sentence(role['oracle'])} {sentence(role['holdout'])} Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.\n\n"
    body += f"Domain challenge: {sentence(role['failure'])} {mode['acceptance']}\n\n"
    body += 'Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.\n\n'
    anchors = ', '.join(f'`{p}`' for p in role['primary_paths'])
    symbols = ', '.join(f'`{s}`' for s in donor['symbols'])
    body += f"Inspect {anchors} and `{donor['path']}` ({symbols}). Donor baseline: {sentence(donor['observation'])} Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.\n"
    return title, body


def build_artifacts(roles, modes, sources):
    files, rows, text_hashes, signatures = {}, [], set(), set()
    books, all_bodies = {}, []
    for role in roles:
        book = f"# {role['name']} — 30 implementation prompts\n\nEach section is independently usable. These are proposed features and training tasks, not completed runs.\n\n"
        bodies = []
        for mode in modes:
            number = 121 + (int(role['id']) - 1) * 30 + int(mode['id']) - 1
            title, body = render(role, mode, number)
            words = len(body.split())
            require(200 <= words <= 400, f'prompt {number}: {words} words, expected 200-400')
            # Strip the title before duplicate checking so unique numbering cannot hide duplicate bodies.
            body_hash = digest(re.sub(r'\s+', ' ', body.split('\n', 1)[1]).strip().encode())
            require(body_hash not in text_hashes, f'duplicate prompt body {number}')
            text_hashes.add(body_hash)
            signature = digest(compact([role['context'], role['program'], role['oracle'], role['failure'], mode['mechanism'], mode['acceptance']]).encode())
            require(signature not in signatures, f'duplicate feature contract {number}')
            signatures.add(signature)
            relative = f"prompts/{number:04}-{role['slug']}-{mode['slug']}.md"
            files[relative] = body
            row = {'id': number, 'role_id': role['id'], 'role': role['name'], 'mode_id': mode['id'],
                   'feature': mode['name'], 'title': title, 'prompt': relative,
                   'word_count': words, 'sha256': digest(body.encode()), 'contract_sha256': signature,
                   'primary_paths': role['primary_paths'], 'donor_sources': role['donor_sources'],
                   'oracle': role['oracle'], 'domain_challenge': role['failure'], 'feature_challenge': mode['acceptance'],
                   'coordination_key': f"role:{role['slug']}", 'standalone': True, 'training_executed': False}
            rows.append(row)
            bodies.append(body.rstrip())
            all_bodies.append(body.rstrip())
        books[role['id']] = f"books/{role['id']}-{role['slug']}.md"
        files[books[role['id']]] = book + '\n\n---\n\n'.join(bodies) + '\n'
    counts = [r['word_count'] for r in rows]
    report = {'schema': 'dsco.rlm-expansion.qa/v1', 'passed': True, 'prompt_count': len(rows),
              'ids': [121, 3120], 'role_count': 100, 'feature_mode_count': 30,
              'word_count_method': 'whitespace split including title',
              'min_words': min(counts), 'max_words': max(counts), 'total_words': sum(counts),
              'unique_bodies_excluding_titles': len(text_hashes), 'unique_structured_feature_contracts': len(signatures),
              'verified_source_files': len(sources), 'foundation_prompts_unchanged': 120,
              'semantic_uniqueness_claim': 'Distinct role-feature contracts; related prompts intentionally share RLM, training and authority requirements.',
              'authoring_method': 'Source-grounded role profiles combined with thirty explicitly authored feature mechanisms; deterministic rendering.',
              'training_runs': 0, 'runtime_changes': 0}
    catalog = {'schema': 'dsco.rlm-expansion.catalog/v1', 'status': 'implementation-specifications',
               'prompt_count': 3000, 'first_id': 121, 'last_id': 3120, 'roles': roles, 'modes': modes, 'prompts': rows}
    files['catalog.json'] = json.dumps(catalog, ensure_ascii=False, indent=2) + '\n'
    files['prompts.jsonl'] = ''.join(compact({'id': r['id'], 'role': r['role'], 'feature': r['feature'], 'prompt': files[r['prompt']]}) + '\n' for r in rows)
    files['sources.json'] = json.dumps({'sources': sources}, indent=2) + '\n'
    files['qa.json'] = json.dumps(report, indent=2) + '\n'
    files['PROMPTS_0121_3120.md'] = '# 3,000 additional prompts: each role trains an RLM\n\nPrompts 121–3120. Each section is a complete implementation request, with training and evaluation acceptance criteria. No training was run to create this document.\n\n' + '\n\n---\n\n'.join(all_bodies) + '\n'
    readme = '# 3,000 more prompts: roles that train their own RLMs\n\n'
    readme += f"Prompts **121–3120** extend the original 120 to **3,120 total**. Each new prompt is **{min(counts)}–{max(counts)} words**; the expansion contains **{sum(counts):,} words**.\n\n"
    readme += 'The catalog contains **100 specialist roles × 30 concrete feature mechanisms**. It is an explicitly structured expansion: related prompts share their role contract or training mechanism. Each combination specifies a domain, executable behavior, actual root training, independent scoring, a domain counterexample and a feature-specific acceptance test. These are implementation specifications, not 3,000 already-built features or trained models.\n\n'
    readme += '- [All 3,000 prompts](PROMPTS_0121_3120.md)\n- [Feature mechanisms](FEATURES.md)\n- [Parallel delivery](DELIVERY.md)\n- [Machine-readable catalog](catalog.json) and [full prompt JSONL](prompts.jsonl)\n- [Validation results](qa.json) and [verified source references](sources.json)\n- [Role RLM training architecture](../ROLE_RLM_TRAINING.md)\n\n'
    readme += '## Find and export prompts\n\n```sh\npython3 docs/self-rewriting-runtime/expansion/select.py --query "recovery" --limit 12\npython3 docs/self-rewriting-runtime/expansion/select.py --role 001 --list\npython3 docs/self-rewriting-runtime/expansion/select.py --ids 121,122,150 --output /tmp/selected-rlm-prompts.md\npython3 docs/self-rewriting-runtime/expansion/build.py --check\n```\n\n'
    readme += '## Role readers\n\n| Role | Prompt range | Independent task contract |\n| --- | --- | --- |\n'
    for role in roles:
        first = 121 + (int(role['id']) - 1) * 30
        readme += f"| [{role['id']} · {role['name']}]({books[role['id']]}) | {first}–{first+29} | {role['brief'].replace('|', '/')} |\n"
    readme += '\n## What validation establishes\n\nThe builder checks numbering, counts, lengths, unique bodies without titles, unique structured role-feature contracts, source files and Python symbols, links, byte-for-byte generated artifacts, and preservation of the original 120 prompts. Those structural checks do not establish semantic novelty, training efficacy or runtime correctness. Domain profiles and feature mechanisms were reviewed separately; selected complete prompts were sampled for coherence.\n\nRun `build.py --write` only after intentional profile or mechanism edits. `--check --zip` packages the expansion with the foundation prompts, supporting documents and validator in `docs/self-rewriting-runtime.zip`. Checking C and Python source references requires the original checkouts; those source trees are not bundled. No command in this package launches training.\n'
    files['README.md'] = readme
    features = '# Thirty feature mechanisms for each specialist RLM\n\nEach numbered mechanism adds a specific implementation and training capability to a role. Its independent task oracle and domain challenge come from the role profile.\n\n'
    for mode in modes:
        features += f"## {mode['id']} — {mode['name']}\n\n{mode['mechanism']}\n\nAcceptance: {mode['acceptance']}\n\n"
    files['FEATURES.md'] = features
    return files, report


def validate_links(files):
    # Individual prompts contain code identifiers and source paths; only actual Markdown links are resolved.
    for relative, content in files.items():
        if not relative.endswith('.md'):
            continue
        for link in re.findall(r'\]\(([^)]+)\)', content):
            if '://' in link or link.startswith('#'):
                continue
            target = (HERE / relative).parent / link.split('#')[0]
            require(target.exists() or str(target.relative_to(HERE)) in files if target.is_relative_to(HERE) else target.exists(), f'broken link {relative}: {link}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--write', action='store_true')
    mode.add_argument('--check', action='store_true')
    parser.add_argument('--zip', action='store_true')
    args = parser.parse_args()
    roles, modes, sources = load_inputs()
    files, report = build_artifacts(roles, modes, sources)
    for relative, content in files.items():
        path = HERE / relative
        if args.write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        else:
            require(path.is_file() and path.read_text() == content, f'artifact drift: {relative}; inspect before --write')
    actual = {str(p.relative_to(HERE)) for p in (HERE / 'prompts').glob('*.md')}
    require(actual == {r for r in files if r.startswith('prompts/')}, 'unexpected or missing generated prompt files')
    validate_links(files)
    if args.zip:
        archive = BASE.parent / 'self-rewriting-runtime.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for path in sorted(BASE.rglob('*')):
                if path.is_file() and path.suffix in ('.md', '.json', '.jsonl', '.py') and '__pycache__' not in path.parts:
                    z.write(path, 'self-rewriting-runtime/' + str(path.relative_to(BASE)))
        with zipfile.ZipFile(archive) as z:
            require(z.testzip() is None, 'archive integrity failure')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
