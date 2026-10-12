// org.js — autonomous organization primitive for @distributed.systems/dsco-sdk.
//
// An Org is a self-directing group of named agents operating under a shared
// treasury, a budget policy, and a goal loop — all over the governed MCP tool
// plane. This is the code shape of an autonomous organization:
//
//   treasury  — a ledger; agents earn and spend against a balance
//   policy    — what the org may do (budget caps, allowed toolsets)
//   agents    — named members with roles, earning/spending from the treasury
//   goals     — objectives decomposed into tasks the loop executes
//   loop      — sense -> decide -> act -> verify -> record, bounded by policy
//
'use strict';

import fs from 'node:fs';
import path from 'node:path';
import { McpClient } from './mcp.js';

/** In-memory + optional file-backed ledger. 1 unit = 1 cent USD. */
export class Treasury {
  constructor({ path = null } = {}) {
    this.path = path;
    this.balances = new Map();   // agent -> cents
    this.events = [];            // {ts, agent, delta, reason}
    this._load();
  }
  _load() {
    if (!this.path) return;
    try {
      const d = JSON.parse(fs.readFileSync(this.path, 'utf8'));
      this.balances = new Map(Object.entries(d.balances || {}));
      this.events = d.events || [];
    } catch { /* fresh */ }
  }
  _save() {
    if (!this.path) return;
    fs.mkdirSync(path.dirname(this.path), { recursive: true });
    fs.writeFileSync(this.path, JSON.stringify({
      balances: Object.fromEntries(this.balances), events: this.events.slice(-1000),
    }, null, 2));
  }
  balance(agent) { return this.balances.get(agent) || 0; }
  credit(agent, cents, reason) {
    const b = this.balance(agent) + cents;
    this.balances.set(agent, b);
    this.events.push({ ts: Date.now(), agent, delta: cents, reason });
    this._save();
    return b;
  }
  debit(agent, cents, reason) { return this.credit(agent, -cents, reason); }
  transfer(from, to, cents, reason) {
    if (this.balance(from) < cents) throw new Error(`insufficient: ${from} has ${this.balance(from)}, needs ${cents}`);
    this.debit(from, cents, reason);
    this.credit(to, cents, `from:${from}`);
    return this.balance(from);
  }
  total() { return [...this.balances.values()].reduce((a, b) => a + b, 0); }
}

/** Budget + capability policy governing the org. */
export class Policy {
  constructor({ maxCentsPerTask = 500, maxCentsTotal = 10000, toolsets = 'core',
                allowTools = null, denyTools = ['delete_file'] } = {}) {
    this.maxCentsPerTask = maxCentsPerTask;
    this.maxCentsTotal = maxCentsTotal;
    this.toolsets = toolsets;
    this.allowTools = allowTools;   // null = all in toolsets
    this.denyTools = new Set(denyTools);
    this.spent = 0;
  }
  authorize(tool, estCents) {
    if (this.denyTools.has(tool)) return { ok: false, why: `tool denied: ${tool}` };
    if (this.allowTools && !this.allowTools.includes(tool)) return { ok: false, why: `tool not allowed: ${tool}` };
    if (estCents > this.maxCentsPerTask) return { ok: false, why: `over per-task budget` };
    if (this.spent + estCents > this.maxCentsTotal) return { ok: false, why: `over total budget` };
    return { ok: true };
  }
  recordSpend(cents) { this.spent += cents; }
}

/** A named member of the org with a role. */
export class OrgAgent {
  constructor(org, name, role) {
    this.org = org; this.name = name; this.role = role;
  }
  /** Call a governed tool, charging the org treasury per the cost model. */
  async act(tool, args = {}, estCents = 1) {
    return this.org.execute(this, tool, args, estCents);
  }
}

/** The autonomous organization. */
export class Org {
  constructor({ name = 'org', treasury = null, policy = null, client = null, toolsets } = {}) {
    this.name = name;
    this.treasury = treasury || new Treasury();
    this.policy = policy || new Policy(toolsets ? { toolsets } : {});
    this.client = client || new McpClient({ toolsets: this.policy.toolsets });
    this.agents = new Map();
    this.goals = [];
    this.log = [];
  }
  async init() { await this.client.initialize(); return this; }

  hire(name, role, seedCents = 0) {
    const a = new OrgAgent(this, name, role);
    this.agents.set(name, a);
    if (seedCents) this.treasury.credit(name, seedCents, 'seed');
    return a;
  }

  /** Governed execution: authorize against policy, call the tool, charge treasury. */
  async execute(agent, tool, args, estCents) {
    const auth = this.policy.authorize(tool, estCents);
    if (!auth.ok) {
      this._record(agent, tool, null, `denied: ${auth.why}`);
      return { ok: false, denied: auth.why };
    }
    const res = await this.client.callTool(tool, args);
    this.policy.recordSpend(estCents);
    this.treasury.debit(agent.name, estCents, `tool:${tool}`);
    this._record(agent, tool, res, 'ok');
    return { ok: true, result: res };
  }

  /** Pay an agent for completed, verified work. */
  reward(agent, cents, reason) {
    return this.treasury.credit(agent.name, cents, reason);
  }

  addGoal(goal) { this.goals.push({ goal, status: 'open', tasks: [] }); return this; }

  /** One bounded loop iteration: pick an open goal, decompose, act, verify, record. */
  async step() {
    const g = this.goals.find(x => x.status === 'open');
    if (!g) return { idle: true };
    // Decomposition is the org's own reasoning; here a simple dispatch to the
    // first ready agent. Real orgs route by role and cost.
    this._record(null, 'goal.step', { goal: g.goal }, 'open');
    return { goal: g.goal, status: g.status };
  }

  _record(agent, tool, result, note) {
    this.log.push({ ts: Date.now(), agent: agent?.name || null, tool, note,
                    ok: result != null || note === 'open' });
  }

  /** Org health snapshot. */
  status() {
    return {
      name: this.name,
      agents: [...this.agents.keys()],
      treasury_total: this.treasury.total(),
      treasury: Object.fromEntries(this.treasury.balances),
      spent_cents: this.policy.spent,
      budget_remaining: this.policy.maxCentsTotal - this.policy.spent,
      open_goals: this.goals.filter(g => g.status === 'open').length,
      actions: this.log.length,
    };
  }

  async close() { await this.client.close(); }
}
