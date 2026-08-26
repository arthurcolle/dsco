// transport.js — newline-delimited JSON-RPC 2.0 framing over a child process stdio.
'use strict';
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';

/** @typedef {{ jsonrpc:"2.0", id?:string|number|null, method?:string, params?:any, result?:any, error?:{code:number,message:string,data?:any} }} RpcMessage */

export class TransportError extends Error {
  constructor(msg, code = 'ETRANSPORT') { super(msg); this.name = 'TransportError'; this.code = code; }
}

export class StdioTransport {
  /**
   * @param {string[]} argv full command line, e.g. ["/path/dsco","mcp","serve"]
   * @param {{cwd?:string, env?:Record<string,string>, onNotification?:(m:any)=>void}} [opts]
   */
  constructor(argv, opts = {}) {
    this.argv = argv;
    this.opts = opts;
    /** @type {Map<string,{resolve:(v:any)=>void,reject:(e:any)=>void,timer:NodeJS.Timeout}>} */
    this.pending = new Map();
    this.buf = '';
    this.closed = false;
    this.child = null;
    this.stderrTail = [];
  }

  start() {
    if (this.child) return;
    this.child = spawn(this.argv[0], this.argv.slice(1), {
      cwd: this.opts.cwd,
      env: { ...process.env, ...(this.opts.env || {}) },
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    const out = this.child.stdout;
    out.setEncoding('utf8');
    out.on('data', (chunk) => {
      this.buf += chunk;
      let nl;
      while ((nl = this.buf.indexOf('\n')) >= 0) {
        const line = this.buf.slice(0, nl).trim();
        this.buf = this.buf.slice(nl + 1);
        if (!line) continue;
        let msg;
        try { msg = JSON.parse(line); } catch (e) {
          // tolerate non-JSON chatter lines; surface them on stderr tail for diagnostics
          this._noteStderr(`non-json frame: ${line.slice(0, 200)}`);
          continue;
        }
        this._dispatch(msg);
      }
    });
    this.child.stderr.setEncoding('utf8');
    this.child.stderr.on('data', (c) => this._noteStderr(c));
    this.child.on('exit', (code, signal) => {
      this.closed = true;
      const err = new TransportError(
        `dsco exited (code=${code}${signal ? `, signal=${signal}` : ''}) stderr-tail: ${this.stderrTail.join('').slice(-800)}`
      );
      for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(err); }
      this.pending.clear();
      if (this.opts.onExit) this.opts.onExit(code, signal);
    });
  }

  _noteStderr(s) {
    if (this.opts.onStderr) return void this.opts.onStderr(s);
    this.stderrTail.push(s);
    if (this.stderrTail.join('').length > 4096) this.stderrTail.splice(0, 8);
  }

  _dispatch(msg) {
    if (msg.id === undefined || msg.id === null || msg.method !== undefined) {
      // notification / server-initiated request
      if (msg.method && msg.id !== undefined) return; // server->client requests unsupported in v0
      if (this.opts.onNotification) this.opts.onNotification(msg);
      return;
    }
    const key = String(msg.id);
    const p = this.pending.get(key);
    if (!p) return; // late response for a timed-out call
    clearTimeout(p.timer);
    this.pending.delete(key);
    if (msg.error) p.reject(new TransportError(`rpc ${msg.error.code}: ${msg.error.message}`, String(msg.error.code)));
    else p.resolve(msg.result);
  }

  /** Send and await response. */
  send(method, params, timeoutMs = 30000) {
    if (this.closed) throw new TransportError('transport closed');
    this.start();
    const id = randomUUID();
    const frame = JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n';
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new TransportError(`${method} timed out after ${timeoutMs}ms`, 'ETIMEOUT'));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.child.stdin.write(frame, (err) => { if (err) { clearTimeout(timer); this.pending.delete(id); reject(new TransportError(`write failed: ${err.message}`)); } });
    });
  }

  async close() {
    if (!this.child || this.closed) return;
    const c = this.child;
    c.stdin.end();
    await new Promise((res) => {
      const t = setTimeout(() => { try { c.kill('SIGKILL'); } catch {} res(); }, 2000);
      c.once('exit', () => { clearTimeout(t); res(); });
    });
  }
}
