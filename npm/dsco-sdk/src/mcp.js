// mcp.js — MCP tool-plane client over a managed stdio transport.
'use strict';
import { StdioTransport } from './transport.js';
import { resolveBinary } from './resolve-binary.js';

/** @typedef {{name:string, description?:string, inputSchema?:any}} ToolDef */

export class McpClient {
  /**
   * @param {{bin?:string, cwd?:string, env?:Record<string,string>, toolsets?:string, tier?:string,
   *          onNotification?:(m:any)=>void, onStderr?:(s:string)=>void}} [opts]
   */
  constructor(opts = {}) {
    const bin = opts.bin || resolveBinary();
    const argv = [bin, 'mcp', 'serve'];
    if (opts.toolsets) argv.push('--toolsets', opts.toolsets);
    if (opts.tier) argv.push('--tier', opts.tier);
    this.t = new StdioTransport(argv, opts);
  }

  async initialize(timeoutMs) {
    const res = await this.t.send('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: '@distributed.systems/dsco-sdk', version: '1.1.0' },
    }, timeoutMs);
    await this.notify('notifications/initialized', {});
    return res; // serverInfo + capabilities
  }

  notify(method, params) {
    if (this.t.closed) throw new Error('closed');
    this.t.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params }) + '\n');
  }

  /** @returns {Promise<ToolDef[]>} */
  async listTools(timeoutMs) {
    const res = await this.t.send('tools/list', {}, timeoutMs);
    return (res && res.tools) || [];
  }

  /**
   * @param {string} name
   * @param {Record<string,any>} [args]
   * @returns {Promise<{content:Array<any>, isError?:boolean}>}
   */
  callTool(name, args = {}, timeoutMs = 120000) {
    return this.t.send('tools/call', { name, arguments: args }, timeoutMs);
  }

  close() { return this.t.close(); }
}
