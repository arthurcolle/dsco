// acp.js — ACP session-plane client (initialize / session/new / session/prompt).
'use strict';
import { StdioTransport } from './transport.js';
import { resolveBinary } from './resolve-binary.js';

export class AcpClient {
  /**
   * @param {{bin?:string, cwd?:string, env?:Record<string,string>,
   *          onUpdate?:(u:any)=>void, onStderr?:(s:string)=>void}} [opts]
   */
  constructor(opts = {}) {
    const bin = opts.bin || resolveBinary();
    this.opts = opts;
    // ACP updates stream as notifications on the shared transport
    const tOpts = { ...opts, onNotification: (m) => {
      if (m.method === 'session/update' && opts.onUpdate) opts.onUpdate(m.params);
      else if (m.method && this._methodHandler) this._methodHandler(m);
    }};
    this.t = new StdioTransport([bin, 'acp', 'serve'], tOpts);
  }

  async initialize(timeoutMs) {
    return this.t.send('initialize', { protocolVersion: 1 }, timeoutMs);
  }

  /** @returns {Promise<{sessionId:string}>} */
  async createSession(cwd) {
    return this.t.send('session/new', { cwd: cwd || process.cwd(), mcpServers: [] });
  }

  /**
   * Send a prompt; resolves with the agent's stop reason payload.
   * Streaming content arrives via the onUpdate callback.
   */
  async prompt(sessionId, text, timeoutMs = 300000) {
    return this.t.send('session/prompt', {
      sessionId,
      prompt: [{ type: 'text', text }],
    }, timeoutMs);
  }

  async cancel(sessionId) {
    await this.t.send('session/cancel', { sessionId });
  }

  close() { return this.t.close(); }
}
