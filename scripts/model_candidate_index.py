#!/usr/bin/env python3
"""Persistent model observations and an evidence-driven evaluation queue.

Catalog membership never implies entitlement or qualification. No inference,
model downloads, or routing changes happen in this module.
"""
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
import time

from model_lab_catalog import atomic_json

ID = re.compile(r'^[\w./:+~@-]{1,200}$', re.ASCII)
FIELDS = ('created', 'createdAt', 'lastModified', 'pipeline_tag', 'context_length',
          'context_window', 'architecture', 'pricing', 'supported_parameters',
          'supported_reasoning_levels', 'default_reasoning_level', 'visibility',
          'priority', 'input_modalities', 'providers', 'sha', 'tags', 'disabled')


def connect(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path)
    db.row_factory = sqlite3.Row
    db.executescript('''
      CREATE TABLE IF NOT EXISTS feeds (
        provider TEXT PRIMARY KEY, status TEXT, scope TEXT, url TEXT,
        last_attempt REAL, last_success REAL, complete INTEGER, detail TEXT);
      CREATE TABLE IF NOT EXISTS models (
        provider TEXT, model TEXT, metadata TEXT, digest TEXT,
        first_seen REAL, last_seen REAL, changed_at REAL, listed INTEGER,
        PRIMARY KEY(provider,model));
      CREATE TABLE IF NOT EXISTS evaluations (
        provider TEXT, model TEXT, digest TEXT, task_digest TEXT,
        evaluated_at REAL, verified INTEGER, evidence TEXT,
        PRIMARY KEY(provider,model,digest,task_digest));
    ''')
    return db


def model_items(data):
    rows = data if isinstance(data, list) else data.get('data', data.get('models')) if isinstance(data,dict) else None
    if not isinstance(rows, list) or any(not isinstance(r,dict) for r in rows):
        raise ValueError('catalog models must be a list')
    return rows


def ingest(db, provider, feed, rows, now):
    old = db.execute('SELECT last_success FROM feeds WHERE provider=?', (provider,)).fetchone()
    success = feed['status'] == 'catalog_ok'
    complete = bool(feed.get('complete', True))
    db.execute('INSERT OR REPLACE INTO feeds VALUES(?,?,?,?,?,?,?,?)', (
        provider, feed['status'], feed.get('catalog_auth', 'unknown'), feed.get('catalog_url'),
        now, now if success else old['last_success'] if old else None, int(complete),
        json.dumps(feed.get('coverage', {}), sort_keys=True)))
    counts = dict(new=0, changed=0, observed=0)
    if not success:
        return counts
    if complete:
        db.execute('UPDATE models SET listed=0 WHERE provider=?', (provider,))
    for row in rows:
        if not isinstance(row, dict):
            continue
        ident = row.get('id', row.get('slug', row.get('name')))
        if not isinstance(ident, str) or not ID.fullmatch(ident):
            continue
        meta = {key: row[key] for key in FIELDS if key in row}
        encoded = json.dumps(meta, sort_keys=True, separators=(',', ':'), allow_nan=False)
        digest = hashlib.sha256(encoded.encode()).hexdigest()
        old = db.execute('SELECT digest,first_seen,changed_at FROM models WHERE provider=? AND model=?',
                         (provider, ident)).fetchone()
        changed = bool(old and old['digest'] != digest)
        counts['new'] += old is None
        counts['changed'] += changed
        counts['observed'] += 1
        db.execute('INSERT OR REPLACE INTO models VALUES(?,?,?,?,?,?,?,1)', (
            provider, ident, encoded, digest, old['first_seen'] if old else now, now,
            now if not old or changed else old['changed_at']))
    return counts


def candidates(db, now=None, max_age=3600, task_digest=None, evaluation_ttl=86400):
    now = time.time() if now is None else now
    output = []
    for row in db.execute('SELECT m.*,f.status,f.scope,f.url,f.last_success FROM models m JOIN feeds f USING(provider)'):
        item = dict(row)
        meta = json.loads(item.pop('metadata'))
        item['metadata'] = meta
        item['fresh'] = (item['status'] == 'catalog_ok' and item['listed'] == 1
                         and 0 <= now - item['last_seen'] <= max_age)
        params = meta.get('supported_parameters')
        item['tools_advertised'] = ('tools' in params if isinstance(params, list) else
                                    True if item['scope'] == 'account_catalog' else None)
        architecture = meta.get('architecture')
        modes = architecture.get('output_modalities') if isinstance(architecture, dict) else None
        text_model = not isinstance(modes, list) or 'text' in modes
        item['evaluation_candidate'] = (item['fresh'] and item['scope'] != 'hub_publication'
            and meta.get('visibility') != 'hide' and meta.get('disabled') is not True
            and item['tools_advertised'] is not False and text_model)
        item['qualification'] = 'unverified'
        item['last_evaluated_at'] = None
        if task_digest:
            evaluation = db.execute('SELECT * FROM evaluations WHERE provider=? AND model=? AND digest=? AND task_digest=?',
                (item['provider'], item['model'], item['digest'], task_digest)).fetchone()
            if evaluation:
                item['last_evaluated_at'] = evaluation['evaluated_at']
                if 0 <= now-evaluation['evaluated_at'] <= evaluation_ttl:
                    item['qualification'] = 'passed' if evaluation['verified'] else 'failed'
                else:
                    item['qualification'] = 'expired'
        item['needs_evaluation'] = item['evaluation_candidate'] and item['qualification'] in ('unverified', 'expired')
        output.append(item)
    return output


def order_candidates(rows):
    """Unmeasured/changed models first, then account priority and release date.

    This is an evaluation admission order, not a model-quality ranking.
    """
    def numeric(value, fallback):
        try:
            result = float(value)
            return result if math.isfinite(result) else fallback
        except (ValueError, TypeError):
            return fallback
    def key(row):
        meta = row['metadata']
        created = meta.get('created', meta.get('createdAt', ''))
        if isinstance(created, str):
            try: created = dt.datetime.fromisoformat(created.replace('Z', '+00:00')).timestamp()
            except ValueError: created = 0
        return (not row['needs_evaluation'], row['last_evaluated_at'] is not None,
                -row['changed_at'], numeric(meta.get('priority'), 1e9),
                -numeric(created, 0), row['provider'], row['model'])
    return sorted(rows, key=key)


def update(snapshot, output, now=None):
    now = time.time() if now is None else now
    db = connect(output / 'candidates.sqlite')
    changes = {}
    try:
        with db:
            for feed in json.loads((snapshot/'inventory.json').read_text()):
                provider = feed['provider']
                rows = []
                if feed['status'] == 'catalog_ok':
                    path = snapshot/(provider+'-models.json')
                    if not path.is_file():
                        feed = dict(feed, status='missing_catalog_file')
                    else:
                        rows = model_items(json.loads(path.read_text()))
                observed = now
                if isinstance(feed.get('observed_at'), str):
                    try: observed = dt.datetime.fromisoformat(feed['observed_at'].replace('Z', '+00:00')).timestamp()
                    except ValueError: pass
                changes[provider] = ingest(db, provider, feed, rows, observed)
        rows = candidates(db, now)
        queue = order_candidates([r for r in rows if r['evaluation_candidate']])
        summary = {'schema': 'dsco.model_candidates.v1', 'observed_at_unix': now,
            'indexed_provider_model_pairs': len(rows),
            'fresh_pairs': sum(r['fresh'] for r in rows),
            'evaluation_candidates': len(queue),
            'hub_publications': sum(r['scope']=='hub_publication' for r in rows),
            'changes': changes, 'feeds': [dict(r) for r in db.execute('SELECT * FROM feeds ORDER BY provider')]}
        atomic_json(output/'candidates.summary.json', summary)
        atomic_json(output/'evaluation-queue.json', {'schema': 'dsco.model_evaluation_queue.v1',
            'observed_at_unix': now, 'inference_requests': 0, 'candidates': queue})
        return summary
    finally:
        db.close()
