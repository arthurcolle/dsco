// unit.test.mjs — framing, correlation, error mapping against a fake NDJSON server.
'use strict';
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));

// Fixture: fake dsco that speaks our exact wire protocol.
const fakeServer = `
const lines = [];
process.stdin.setEncoding('utf8');
let buf='';
process.stdin.on('data', c => { buf += c; let i;
  while ((i = buf.indexOf('\\n')) >= 0) {
    const line = buf.slice(0, i).trim(); buf = buf.slice(i+1);
    if (line) handle(JSON.parse(line));
  }
});
function handle(msg){
  if (msg.method === 'ping_with_delay') {
    setTimeout(() => reply(msg.id, { pong: true }), 400); return;
  }
  if (msg.method === 'boom') return send({jsonrpc:'2.0',id:msg.id,error:{code:-32601,message:'method not found'}});
  if (msg.method && msg.id === undefined) return void 0; // notification
  if (msg.method === 'greet') {
    send({jsonrpc:'2.0',method:'hello',params:{from:'server'}});
    return reply(msg.id, { hi: msg.params.name });
  }
  reply(msg.id, { echoed: msg });
}
function send(o){ process.stdout.write(JSON.stringify(o)+'\\n'); }
function reply(id,r){ send({jsonrpc:'2.0',id,result:r}); }
`;

test('transport: response correlation, notifications, rpc errors, timeout', async () => {
  const serverPath = path.join(here, '.tmp-fake-server.mjs');
  writeFileSync(serverPath, fakeServer);

  const mod = await import('../src/transport.js');
  const { StdioTransport } = mod;

  let notified = null;
  const t = new StdioTransport(['node', serverPath], {
    onNotification: (m) => { if (m.method === 'hello') notified = m.params; },
  });

  const greet = await t.send('greet', { name: 'arthur' }, 5000);
  assert.equal(greet.hi, 'arthur');
  assert.deepEqual(notified, { from: 'server' }); // notification delivered with response

  await assert.rejects(() => t.send('boom'), /-32601.*not found/);
  await assert.rejects(() => t.send('ping_with_delay', {}, 100), /timed out after 100ms/);

  // timeout must not corrupt the pending map: late ping still resolves cleanly for new calls
  const again = await t.send('greet', { name: 'second' }, 5000);
  assert.equal(again.hi, 'second');

  await t.close();
});

test('resolveBinary honors DSCO_BIN and vendored layout', async () => {
  delete process.env.DSCO_BIN;
  const m = await import('../src/resolve-binary.js');
  assert.equal(m.resolveBinary(), 'dsco'); // falls through to PATH
  process.env.DSCO_BIN = '/opt/dsco/bin/dsco';
  try { m.resolveBinary(); assert.fail('should throw'); }
  catch (e) { assert.match(e.message, /does not exist/); }
});
