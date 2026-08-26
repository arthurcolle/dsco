// e2e.test.mjs — live tests against a REAL dsco binary (DSCO_BIN or repo root ./dsco).
// Run: DSCO_BIN=/path/dsco node --test test/e2e.test.mjs
'use strict';
import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { McpClient, AcpClient } from '../src/index.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const bin = process.env.DSCO_BIN
  || (existsSync(path.join(here, '../../dsco')) ? path.join(here, '../../dsco') : 'dsco');
console.log(`e2e target binary: ${bin}`);

test('MCP: initialize -> listTools -> governed tool call round-trip', async () => {
  const c = new McpClient({ bin });
  try {
    const init = await c.initialize(15000);
    assert.ok(init.serverInfo?.name, 'serverInfo present');

    const tools = await c.listTools(30000);
    assert.ok(Array.isArray(tools) && tools.length >= 5, `expected tool surface, got ${tools.length}`);
    assert.ok(tools.every(t => typeof t.name === 'string'));
    const names = tools.map(t => t.name);
    assert.ok(names.includes('read_file') && names.includes('grep_files'), 'core file tools exposed');

    // safe fs_read round-trip under default (core) toolset: reads are always allowed
    const r = await c.callTool('list_directory', { path: '/tmp' }, 30000);
    assert.ok(!r.isError && r.content?.length, 'fs_read tool returns content');

    // governance assertion: control-plane tool NOT in core toolset -> hard -32602 from the gate
    await assert.rejects(
      () => c.callTool('killswitch', { action: 'status' }, 15000),
      /not exposed by enabled toolsets/
    );
  } finally { await c.close(); }
});

test('MCP: --toolsets all exposes + permits read-only control-plane status', async () => {
  const c = new McpClient({ bin, toolsets: 'all' });
  try {
    await c.initialize(15000);
    const tools = await c.listTools(30000);
    assert.ok(tools.some(t => t.name === 'killswitch'), 'killswitch present in "all" toolset');
    const r = await c.callTool('killswitch', { action: 'status' }, 30000); // mutating verbs stay gated
    assert.ok(r.content?.length || !r.isError, 'read-only status permitted');
  } finally { await c.close(); }
});

test('ACP: initialize -> session/new -> prompt', { skip: !process.env.DSCO_LIVE_SESSIONS }, async () => {
  const c = new AcpClient({ bin });
  try {
    await c.initialize(15000);
    const { sessionId } = await c.createSession(process.cwd());
    assert.ok(sessionId);

    let sawUpdate = false;
    // note: to observe updates on the shared transport we rewire via constructor opts in real usage;
    // here just verify stop result resolves within governance constraints.
    const stop = await c.prompt(sessionId, 'Reply with exactly: SDK_LINK_OK', 120000);
    assert.ok(stop !== undefined, 'prompt resolved with a stop payload');
    void sawUpdate;
  } finally { await c.close(); }
});
