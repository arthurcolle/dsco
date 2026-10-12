#!/usr/bin/env python3
"""Prepare model evaluations from fresh discovery, then record verified outcomes.

The template supplies tasks, allowed providers, and budgets. Its model names are
replaced by the discovery queue. A frozen selection preserves reproducibility.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

import harness_matrix as hm
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
import model_candidate_index as index


def task_identity(manifest, source):
    inputs = {'tasks': manifest['tasks'], 'files': {}, 'fixtures': {},
              'budgets': manifest['defaults'], 'execution_policies': []}
    policies = set()
    for adapter in manifest['adapters']:
        policy = {k:v for k,v in adapter.items() if k not in ('id','model','effort')}
        binary = hm.resolve_binary(adapter['binary'], source.parent)
        policy['binary_sha256'] = hm.sha256_file(binary)
        policies.add(json.dumps(policy,sort_keys=True))
    inputs['execution_policies'] = sorted(policies)
    for task in manifest['tasks']:
        if task.get('fixture'):
            p = (source.parent/task['fixture']).resolve()
            inputs['fixtures'][str(p)] = hm.workspace_snapshot(p)['sha256']
        for grader in task.get('graders', []):
            for arg in grader.get('argv', []):
                p = Path(arg)
                if p.is_absolute() and p.is_file(): inputs['files'][str(p)] = hm.sha256_file(p)
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


def select(db, providers, task_digest, limit, now=None, max_age=900):
    rows = index.candidates(db, now=now, max_age=max_age, task_digest=task_digest)
    return index.order_candidates([r for r in rows if r['provider'] in providers and r['needs_evaluation']])[:limit]


def prepare(args):
    source = args.template.resolve()
    manifest = hm.load_manifest(source)
    templates = {}
    for adapter in manifest['adapters']:
        if adapter['kind'] != 'dsco_native': continue
        provider = adapter['provider']
        if provider in templates:
            identity = lambda a: {k:v for k,v in a.items() if k not in ('id','model','effort')}
            if identity(templates[provider]) != identity(adapter):
                raise hm.ManifestError('ambiguous provider policies; use one account/budget policy per provider template')
        templates[provider] = adapter
    providers = set(args.provider or templates)
    if not providers or providers-set(templates):
        raise hm.ManifestError('each selected provider needs an explicit budgeted native template adapter')
    # Catalog freshness belongs to each source, never merely the report timestamp.
    db = index.connect(args.catalog/'candidates.sqlite')
    now = time.time()
    feeds = {r['provider']:dict(r) for r in db.execute('SELECT * FROM feeds')}
    needs_refresh = any(p not in feeds or feeds[p]['status']!='catalog_ok' or
        feeds[p]['last_success'] is None or not 0 <= now-feeds[p]['last_success'] <= args.max_age_seconds
        for p in providers)
    db.close()
    if needs_refresh:
        subprocess.run([sys.executable, str(ROOT/'scripts/model_lab_watch.py'),
                        '--output', str(args.catalog)], check=True, timeout=240)
    db = index.connect(args.catalog/'candidates.sqlite')
    digest = task_identity(manifest, source)
    selected = select(db, providers, digest, args.limit, max_age=args.max_age_seconds)
    db.close()
    if not selected:
        raise hm.ManifestError('no fresh unevaluated candidates for these providers/tasks; inspect feed health and qualifications')
    adapters = []
    for row in selected:
        adapter = copy.deepcopy(templates[row['provider']])
        adapter['model'] = row['model']
        adapter['id'] = 'candidate-'+hashlib.sha256((row['provider']+'\0'+row['model']).encode()).hexdigest()[:16]
        efforts = row['metadata'].get('supported_reasoning_levels', [])
        efforts = [e.get('effort') if isinstance(e, dict) else e for e in efforts]
        adapter['effort'] = next((e for e in ('low','minimal','none','medium','high','xhigh','max') if e in efforts), 'auto')
        adapters.append(adapter)
        row['adapter_id'] = adapter['id']
    # Turn relative fixture and binary references into stable paths before moving the manifest.
    for adapter in adapters:
        adapter['binary'] = str(hm.resolve_binary(adapter['binary'], source.parent))
    for task in manifest['tasks']:
        original_allowed = set(task.get('adapters', []))
        if original_allowed:
            allowed_providers = {a['provider'] for a in manifest['adapters'] if a['id'] in original_allowed}
            task['adapters'] = [a['id'] for a in adapters if a['provider'] in allowed_providers]
            if not task['adapters']:
                raise hm.ManifestError('selection leaves a restricted task with no allowed adapter')
        if task.get('fixture'): task['fixture'] = str((source.parent/task['fixture']).resolve())
    manifest['adapters'] = adapters
    manifest['name'] = 'discovery-'+hashlib.sha256(json.dumps(selected, sort_keys=True).encode()).hexdigest()[:16]
    if args.output.exists() or args.output.with_suffix('.selection.json').exists():
        raise hm.ManifestError('selection output already exists; use a new name for a new frozen evaluation')
    hm.atomic_json(args.output, manifest)
    hm.load_manifest(args.output)  # Enforce the same execution contract as the runner.
    sidecar = {'schema':'dsco.discovered_evaluation.v1', 'selected_at_unix':time.time(),
        'manifest_sha256':hm.sha256_file(args.output), 'task_digest':digest, 'task_identity_version':2,
        'catalog':str(args.catalog.resolve()), 'candidates':selected,
        'selection_policy':'fresh, unevaluated-or-expired, changed-first, account-priority, release-date'}
    hm.atomic_json(args.output.with_suffix('.selection.json'), sidecar)
    print(json.dumps({'manifest':str(args.output), 'candidates':[
        {'provider':r['provider'],'model':r['model'],'qualification':r['qualification']} for r in selected]}, indent=2))


def record(args):
    selection = json.loads(args.manifest.with_suffix('.selection.json').read_text())
    if selection['manifest_sha256'] != hm.sha256_file(args.manifest):
        raise hm.ManifestError('manifest changed since selection')
    frozen = args.run/'manifest.json'
    if not frozen.exists() or hm.sha256_file(frozen) != hm.sha256_file(args.manifest):
        # endurance_campaign writes normalized JSON instead of copying raw bytes.
        if not frozen.exists() or json.loads(frozen.read_text()) != json.loads(args.manifest.read_text()):
            raise hm.ManifestError('run does not match the selected manifest')
    state = [json.loads(p.read_text()) for p in (args.run/'state').glob('*.json')]
    if not state or any(r['status']=='started' for r in state):
        raise hm.ManifestError('evaluation is incomplete')
    manifest = hm.load_manifest(args.manifest)
    if selection['task_digest'] != task_identity(manifest,args.manifest):
        raise hm.ManifestError('task, executable, budget, or grader changed since selection')
    db = index.connect(Path(selection['catalog'])/'candidates.sqlite')
    records = []
    try:
        with db:
            for candidate in selection['candidates']:
                rows = [r for r in state if r['adapter_id']==candidate['adapter_id']]
                expected = {t['id'] for t in manifest['tasks'] if not t.get('adapters') or candidate['adapter_id'] in t['adapters']}
                if {r['task_id'] for r in rows} != expected:
                    raise hm.ManifestError('candidate lacks results for every assigned task')
                matched = lambda r: bool(r.get('native_receipts')) and all(
                    receipt.get('provider')==candidate['provider'] and receipt.get('actual_model')==candidate['model']
                    for receipt in r['native_receipts'])
                passed = all(r.get('verified') is True and matched(r) for r in rows)
                evidence = {'run':str(args.run.resolve()), 'attempts':[r['attempt_id'] for r in rows],
                            'selection_sha256':hm.sha256_file(args.manifest.with_suffix('.selection.json'))}
                receipts = [receipt for r in rows for receipt in r.get('native_receipts',[])]
                costs = [r['budget_accounted_usd'] for r in receipts
                         if type(r.get('budget_accounted_usd')) in (int,float)]
                evidence.update(artifact_verified=all(bool(r.get('graders')) and all(g.get('passed') is True for g in r['graders']) for r in rows),
                    reference_cost_usd=sum(costs) if costs and len(costs)==len(receipts) else None,
                    worker_seconds=sum(r.get('process',{}).get('wall_seconds',0) for r in rows),
                    provider_responses=len(receipts),
                    subscription_responses=sum(r.get('subscription_included') is True for r in receipts))
                observed = max((r.get('finished_at_unix',r.get('started_at_unix',0)) for r in rows), default=0)
                db.execute('INSERT OR REPLACE INTO evaluations VALUES(?,?,?,?,?,?,?)',
                    (candidate['provider'],candidate['model'],candidate['digest'],selection['task_digest'],
                     observed,int(passed),json.dumps(evidence)))
                records.append({'provider':candidate['provider'],'model':candidate['model'],'verified':passed})
    finally:
        db.close()
    print(json.dumps({'recorded':records},indent=2))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='action',required=True)
    p=sub.add_parser('prepare');p.add_argument('template',type=Path)
    p.add_argument('--catalog',type=Path,default=ROOT/'reports/model-labs/watch')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--limit',type=int,default=4)
    p.add_argument('--provider',action='append');p.add_argument('--max-age-seconds',type=int,default=900)
    p=sub.add_parser('record');p.add_argument('manifest',type=Path);p.add_argument('--run',type=Path,required=True)
    args=parser.parse_args()
    if args.action=='prepare' and (not 1 <= args.limit <= 100 or not 60 <= args.max_age_seconds <= 86400):
        parser.error('limit must be 1..100 and max-age-seconds 60..86400')
    try:
        (prepare if args.action=='prepare' else record)(args)
        return 0
    except (hm.ManifestError,OSError,ValueError,subprocess.SubprocessError) as error:
        print(json.dumps({'error':type(error).__name__,'detail':str(error)}),file=sys.stderr)
        return 2


if __name__=='__main__': raise SystemExit(main())
