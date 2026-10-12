// index.js — @distributed.systems/dsco-sdk public surface.
// One stack: runtime (governed tools) -> SDK (MCP/ACP) -> Org (autonomous team)
// -> Organism (the unified body). See ARCHITECTURE.md.
'use strict';
export { StdioTransport, TransportError } from './transport.js';
export { resolveBinary, platformTripleHint } from './resolve-binary.js';
export { McpClient } from './mcp.js';
export { AcpClient } from './acp.js';
export { Org, OrgAgent, Treasury, Policy } from './org.js';
export { Organism } from './organism.js';
