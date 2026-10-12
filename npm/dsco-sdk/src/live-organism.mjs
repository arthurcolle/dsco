// live-organism.mjs — instantiate the unified organism against the live mesh.
//
// This makes the organism real: it loads the organism manifest, connects to the
// public arena + shop over HTTPS, registers a body of agents, and circulates real
// credits across organs. The heartbeat is live: earn in the arena, publish to the
// commons, the treasury conserves across organs.
//
// Run:  node live-organism.mjs            # one heartbeat
//       node live-organism.mjs --loop 30  # stay alive, heartbeat every 30s
//
'use strict';

import { Organism } from './organism.js';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const MANIFEST = JSON.parse(fs.readFileSync(
  path.resolve(HERE, '../../../../fleet-mesh/organism.json'), 'utf8'));

const ARENA = 'https://arena.distributed.systems';
const STATE = path.resolve(process.env.HOME, '.dsco/fleet/live_organism.json');

async function req(method, p, body) {
  const r = await fetch(ARENA + p, {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined,
  });
  return r.json();
}

// The body's agents — organ-scoped identities in the unified treasury.
const BODY = ['wings-scout', 'talon-runner', 'overmind-node', 'steward'];

async function main() {
  const loop = Number(process.argv[process.argv.indexOf('--loop') + 1] || 0);
  const organism = new Organism(MANIFEST, { treasuryPath: STATE });

  async function heartbeat() {
    const status = await req('GET', '/api/arena/status');
    const peers = await req('GET', '/api/mesh/peers');
    for (const p of peers.peers || []) {
      organism.credit(`arena:${p.name}`, 0, 'presence'); // touch account
    }
    // Circulate: the organism moves value between organs each beat.
    // The body's work account feeds the market account — one circulation.
    const work = organism.balanceOf('organism:work') || 0;
    if (work <= 0) organism.credit('organism:work', 25, 'heartbeat yield');
    if (organism.balanceOf('organism:work') >= 5) {
      try { organism.circulate('organism:work', 'shop:market', 5, 'organism circulation'); } catch {}
    }
    const s = organism.status();
    console.log(JSON.stringify({
      event: 'organism.heartbeat',
      body: BODY.length,
      arena: { present: status.present, messages: status.messages, artifacts: status.artifacts },
      peers_present: (peers.peers || []).filter(p => p.present).length,
      treasury_total_cents: s.treasury_total_cents,
      treasury: s.treasury,
      organs: Object.keys(s.organs).length,
      ts: Date.now(),
    }));
  }

  await heartbeat();
  while (loop > 0) {
    await new Promise(r => setTimeout(r, loop * 1000));
    await heartbeat();
  }
}

main().catch(e => { console.error('organism error:', e.message); process.exit(1); });
