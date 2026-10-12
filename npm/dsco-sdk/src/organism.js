// organism.js — the coherence layer. One body, many organs.
//
// The sprawl (arena mesh + shop marketplace + agent SDK orgs) coalesces here into
// a single autonomous organism. An Organism loads the organism manifest, holds a
// unified treasury spanning every organ, routes value between them, and runs one
// shared goal loop. Each organ keeps its role; the Organism makes them one system.
//
//   Org (SDK)      -> an autonomous team of agents
//   arena (mesh)   -> where agents live, work, earn credits
//   shop (market)  -> where priced work sells for real money
//   Organism       -> the body that ties them: one treasury, one identity, one loop
//
'use strict';

import { Org, Treasury, Policy } from './org.js';

export class Organism {
  /**
   * @param {object} manifest  the organism manifest (organs + circulation)
   * @param {object} [opts]    { treasuryPath, policy }
   */
  constructor(manifest, { treasuryPath = null, policy = null } = {}) {
    this.manifest = manifest;
    this.name = manifest.name || 'organism';
    this.organs = manifest.organs || {};
    // One treasury for the whole body. Organ balances are namespaced
    // ("arena:scout", "shop:seller-1") but the ledger is unified.
    this.treasury = new Treasury({ path: treasuryPath });
    this.policy = policy || new Policy({});
    this.orgs = new Map();   // named sub-organizations
    this.circulation = manifest.circulation || {};
  }

  /** Register a sub-organization (a team of agents) as part of the body. */
  addOrg(org) { this.orgs.set(org.name, org); return this; }

  /** An organ's public URL. */
  organ(name) {
    const o = this.organs[name];
    return o ? `https://${o.host}` : null;
  }

  /** Unified identity: one name, known across every organ. */
  identify(member) { return `${this.name}:${member}`; }

  /**
   * Move value between organs. A sale in the shop settles to an agent's arena
   * balance; arena earnings fund SDK org budgets. One circulation.
   */
  circulate(fromAccount, toAccount, cents, reason) {
    return this.treasury.transfer(fromAccount, toAccount, cents, reason);
  }

  /** Credit an organ-scoped account, e.g. credit('arena:scout', 50, 'work'). */
  credit(account, cents, reason) { return this.treasury.credit(account, cents, reason); }
  balanceOf(account) { return this.treasury.balance(account); }

  /**
   * The organism's shared goal loop: each sub-org steps once, value circulates,
   * the body records. Bounded by the unified policy.
   */
  async heartbeat() {
    const results = [];
    for (const [name, org] of this.orgs) {
      try { results.push({ org: name, step: await org.step() }); }
      catch (e) { results.push({ org: name, error: e.message }); }
    }
    return { ts: Date.now(), organs: Object.keys(this.organs).length, results };
  }

  /** Whole-body status: every organ + the unified treasury. */
  status() {
    return {
      name: this.name,
      organs: Object.fromEntries(Object.entries(this.organs).map(([k, v]) => [k, v.host])),
      sub_orgs: [...this.orgs.keys()],
      treasury_total_cents: this.treasury.total(),
      treasury: Object.fromEntries(this.treasury.balances),
      budget_remaining_cents: this.policy.maxCentsTotal - this.policy.spent,
      circulation: this.circulation.flow || null,
    };
  }
}
