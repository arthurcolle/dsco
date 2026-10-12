// org.test.mjs — autonomous organization end-to-end against the real dsco binary.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Org, Treasury, Policy } from '../src/org.js';

const BIN = process.env.DSCO_BIN;

test('treasury: credit/debit/transfer with insufficient-funds guard', () => {
  const t = new Treasury();
  t.credit('alice', 100, 'seed');
  t.transfer('alice', 'bob', 30, 'payment');
  assert.equal(t.balance('alice'), 70);
  assert.equal(t.balance('bob'), 30);
  assert.throws(() => t.transfer('bob', 'alice', 999, 'too much'), /insufficient/);
});

test('policy: budget + capability gates', () => {
  const p = new Policy({ maxCentsPerTask: 10, maxCentsTotal: 25, denyTools: ['delete_file'] });
  assert.equal(p.authorize('read_file', 5).ok, true);
  assert.equal(p.authorize('delete_file', 1).ok, false);   // denied tool
  assert.equal(p.authorize('bash', 50).ok, false);          // over per-task budget
  p.recordSpend(20);
  assert.equal(p.authorize('read_file', 10).ok, false);     // over total budget
});

test('org: hire agents, governed execution, reward, status', { skip: !BIN }, async () => {
  const org = new Org({ name: 'test-org', policy: new Policy({ toolsets: 'core', maxCentsTotal: 1000 }) });
  await org.init();
  const scout = org.hire('scout', 'reconnaissance', 100);
  const builder = org.hire('builder', 'construction', 50);

  // Governed tool call through the real binary: read a file that exists.
  const r = await scout.act('read_file', { path: 'package.json' }, 1);
  assert.equal(typeof r.ok, 'boolean');

  // Denied tool is blocked before touching the runtime.
  const denied = await builder.act('delete_file', { path: '/etc/passwd' }, 1);
  assert.equal(denied.ok, false);
  assert.match(denied.denied, /denied/);

  // Reward verified work.
  org.reward(scout, 25, 'task complete');
  assert.ok(org.treasury.balance('scout') > 0);

  const s = org.status();
  assert.deepEqual(s.agents.sort(), ['builder', 'scout']);
  assert.ok(s.actions > 0);
  await org.close();
});
