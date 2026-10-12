// organism.test.mjs — the coherence layer: many organs, one body.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Organism } from '../src/organism.js';
import { Org, Policy } from '../src/org.js';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const MANIFEST = JSON.parse(fs.readFileSync(
  path.resolve(HERE, '../../../../fleet-mesh/organism.json'), 'utf8'));

test('organism loads manifest and exposes organs', () => {
  const o = new Organism(MANIFEST);
  assert.equal(o.name, 'Distributed Systems');
  assert.equal(o.organ('arena'), 'https://arena.distributed.systems');
  assert.equal(o.organ('shop'), 'https://shop.distributed.systems');
  assert.ok(o.status().organs.commons);
});

test('organism: unified treasury circulates value across organs', () => {
  const o = new Organism(MANIFEST);
  // An arena agent earns credits for work.
  o.credit('arena:scout', 50, 'verified work');
  assert.equal(o.balanceOf('arena:scout'), 50);
  // The agent spends in the shop — value circulates from arena to shop seller.
  o.circulate('arena:scout', 'shop:seller-1', 30, 'artifact purchase');
  assert.equal(o.balanceOf('arena:scout'), 20);
  assert.equal(o.balanceOf('shop:seller-1'), 30);
  // One body: the total is conserved across organs.
  assert.equal(o.treasury.total(), 50);
});

test('organism: identity is unified across organs', () => {
  const o = new Organism(MANIFEST);
  assert.equal(o.identify('scout'), 'Distributed Systems:scout');
});

test('organism: heartbeat steps every sub-org', { skip: !process.env.DSCO_BIN }, async () => {
  const o = new Organism(MANIFEST);
  const team = new Org({ name: 'field-ops', policy: new Policy({ toolsets: 'core' }) });
  await team.init();
  team.addGoal('map the terrain');
  o.addOrg(team);
  const hb = await o.heartbeat();
  assert.equal(hb.organs, 5);
  assert.equal(hb.results[0].org, 'field-ops');
  const s = o.status();
  assert.ok(s.sub_orgs.includes('field-ops'));
  await team.close();
});
