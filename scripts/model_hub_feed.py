#!/usr/bin/env python3
"""Incremental public Hub publication discovery, with persistent pagination.

Fetches metadata only. Each poll checks the head and advances queued backfill;
bounded pagination is reported explicitly instead of claiming an exhaustive scan.
"""
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import time
import urllib.error
import urllib.parse
import urllib.request

from model_lab_catalog import atomic_json

HEAD = 'https://huggingface.co/api/models?sort=createdAt&direction=-1&limit=1000'


def fetch(url, cache):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != 'https' or parsed.netloc != 'huggingface.co' or parsed.path != '/api/models':
        raise ValueError('untrusted Hub pagination URL')
    path = cache/(hashlib.sha256(url.encode()).hexdigest()+'.json')
    old = json.loads(path.read_text()) if path.exists() else {}
    headers = {'User-Agent': 'DSCO-model-publication-watch/1'}
    if old.get('etag'): headers['If-None-Match'] = old['etag']
    if old.get('modified'): headers['If-Modified-Since'] = old['modified']
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=12) as response:
            body = response.read(16*1024*1024+1)
            if len(body)>16*1024*1024: raise ValueError('Hub page too large')
            rows = json.loads(body)
            if not isinstance(rows, list): raise ValueError('invalid Hub page')
            link = response.headers.get('Link', '')
            match = re.search(r'<([^>]+)>;\s*rel="?next"?', link)
            data = dict(rows=rows, next=match.group(1) if match else None,
                        etag=response.headers.get('ETag'), modified=response.headers.get('Last-Modified'))
            atomic_json(path, data)
            return data
    except urllib.error.HTTPError as error:
        if error.code == 304 and old:
            return old
        raise


def created(row):
    value = row.get('createdAt')
    if not isinstance(value, str): return None
    try: return dt.datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()
    except ValueError: return None


def refresh(snapshot, state_dir, max_pages=10, now=None, get=fetch):
    now = time.time() if now is None else now
    state_dir.mkdir(parents=True, exist_ok=True)
    state_path = state_dir/'hub-cursor.json'
    state = json.loads(state_path.read_text()) if state_path.exists() else {'pending': [], 'head_at': None}
    cache = state_dir/'hub-http-cache'
    cache.mkdir(exist_ok=True)
    pending = list(state['pending'])
    rows, requests = {}, 0
    feed = {'provider': 'huggingface-hub', 'catalog_auth': 'hub_publication',
            'catalog_url': HEAD, 'observed_at': dt.datetime.fromtimestamp(now, dt.timezone.utc).isoformat(),
            'complete': False, 'native_transport_supported': False}
    try:
        head = get(HEAD, cache)
        requests += 1
        cutoff = (state.get('head_at') or now-86400) - 600
        def add(page, cutoff):
            dates = [created(r) for r in page['rows'] if isinstance(r, dict)]
            for row in page['rows']:
                if isinstance(row, dict) and isinstance(row.get('id'), str):
                    # Retain the whole page at boundary timestamps, avoiding ties/gaps.
                    rows[row['id']] = row
            return bool(page.get('next')) and not any(t is not None and t < cutoff for t in dates)
        if add(head, cutoff):
            pending.append({'url': head['next'], 'cutoff': cutoff})
        # Round-robin older cursors and new publications; keep unfinished cursors.
        while pending and requests < max_pages:
            cursor = pending.pop(0)
            try:
                page = get(cursor['url'], cache)
            except Exception:
                pending.insert(0, cursor)
                raise
            requests += 1
            if add(page, cursor['cutoff']):
                pending.append({'url': page['next'], 'cutoff': cursor['cutoff']})
        head_dates = [created(r) for r in head['rows'] if isinstance(r, dict)]
        next_head = max((t for t in head_dates if t is not None), default=state.get('head_at'))
        # HTTP cache is bounded to the head and pending pages; model history lives in SQLite.
        keep = {hashlib.sha256(u.encode()).hexdigest()+'.json' for u in [HEAD]+[c['url'] for c in pending]}
        for path in cache.glob('*.json'):
            if path.name not in keep: path.unlink()
        pending = list({(c['url'],c['cutoff']):c for c in pending}.values())
        atomic_json(state_path, {'pending': pending, 'head_at': next_head})
        feed['status'] = 'catalog_ok'
    except Exception as error:
        # Don't advance the head on a failed pass; partial observations are still useful.
        pending = list({(c['url'],c['cutoff']):c for c in pending}.values())
        atomic_json(state_path, {'pending': pending, 'head_at': state.get('head_at')})
        feed['status'] = 'catalog_ok' if rows else ('HTTP_'+str(error.code) if hasattr(error, 'code') else type(error).__name__)
        feed['partial_error'] = type(error).__name__
    feed['model_count'] = len(rows)
    feed['coverage'] = {'scope': 'incremental_publications', 'bootstrap_lookback_hours': 24,
                        'requests': requests, 'page_budget': max_pages,
                        'pending_pages': len(pending), 'caught_up': not pending and 'partial_error' not in feed}
    atomic_json(snapshot/'huggingface-hub-models.json', {'data': list(rows.values())})
    return feed
