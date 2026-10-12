#!/usr/bin/env python3
"""Independent executable oracle for model-authored event-ledger tests."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import random

SPEC = """Implement ledger.py with fold_events(events). Input is a list of dicts,
each with id (nonempty string), kind ('credit' or 'debit'), amount (nonnegative
integer, excluding bool), status ('ok' or 'failed'). Validate every event, even
duplicates and failures; raise ValueError for any invalid input. Successful ids
apply exactly once, in first-success order. Failed events neither change balance
nor consume an id; a later success with that id can apply. A duplicate successful
id is ignored even if its amount/kind differs (first success wins). Credit adds,
debit subtracts; negative balances are allowed. Return {'balance': integer,
'applied': [ids in application order]}. Do not mutate any input. Empty input
returns balance 0 and applied []. Python standard library only."""


def oracle(events):
    if not isinstance(events, list):
        raise ValueError('events must be a list')
    balance, applied = 0, []
    for event in events:
        if not isinstance(event, dict):
            raise ValueError('invalid event')
        ident, kind, amount, status = (event.get(k) for k in ('id', 'kind', 'amount', 'status'))
        if (not isinstance(ident, str) or not ident or kind not in ('credit', 'debit')
                or type(amount) is not int or amount < 0 or status not in ('ok', 'failed')):
            raise ValueError('invalid event')
        if status == 'ok' and ident not in applied:
            balance += amount if kind == 'credit' else -amount
            applied.append(ident)
    return {'balance': balance, 'applied': applied}


def load_cases(path):
    if path.stat().st_size > 1024 * 1024:
        raise ValueError('case file exceeds 1 MiB')
    cases = json.loads(path.read_text())
    if not isinstance(cases, list) or not 10 <= len(cases) <= 30:
        raise ValueError('expected 10..30 model-authored cases')
    names = set()
    for case in cases:
        if not isinstance(case, dict) or set(case) != {'name', 'events', 'expected'}:
            raise ValueError('case shape')
        if not isinstance(case['name'], str) or case['name'] in names:
            raise ValueError('case identity')
        names.add(case['name'])
        if case['expected'] != oracle(case['events']):
            raise ValueError('model-authored expectation contradicts oracle: ' + case['name'])
    return cases


def grade(candidate, cases):
    spec = importlib.util.spec_from_file_location('candidate_ledger', candidate)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    rng = random.Random(424242)
    samples = [case['events'] for case in cases]
    for _ in range(300):
        samples.append([{'id': rng.choice(('a', 'b', 'c', 'é', '0')),
                         'kind': rng.choice(('credit', 'debit')), 'amount': rng.randrange(100),
                         'status': rng.choice(('ok', 'failed'))}
                        for _ in range(rng.randrange(25))])
    for events in samples:
        before = copy.deepcopy(events)
        observed = module.fold_events(events)
        assert observed == oracle(before), (before, observed, oracle(before))
        assert events == before, 'input mutation'
    invalid = [None, {}, 'events', [None], [{}]]
    event = {'id': 'x', 'kind': 'credit', 'amount': 2, 'status': 'ok'}
    for field, values in {'id': ['', None, 1], 'kind': ['other', None],
                          'amount': [-1, True, 1.5, None], 'status': ['pending', None]}.items():
        for value in values:
            invalid.append([{**event, field: value}])
            invalid.append([event, {**event, field: value}])
    for events in invalid:
        try:
            module.fold_events(copy.deepcopy(events))
        except ValueError:
            pass
        else:
            raise AssertionError('invalid event accepted')
    print(json.dumps({'passed': True, 'model_cases': len(cases),
                      'generated_sequences': 300, 'invalid_inputs': len(invalid)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('cases', 'implementation'))
    parser.add_argument('--cases', type=Path, default=Path('cases.json'))
    parser.add_argument('--candidate', type=Path, default=Path('ledger.py'))
    options = parser.parse_args()
    cases = load_cases(options.cases)
    if options.mode == 'cases':
        print(json.dumps({'passed': True, 'validated_model_cases': len(cases)}))
    else:
        grade(options.candidate, cases)
