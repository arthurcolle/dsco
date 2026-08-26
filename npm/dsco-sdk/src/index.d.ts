// Type declarations for @distributed.systems/dsco-sdk (hand-written, zero deps).

export class TransportError extends Error {
  code: string;
}

export interface RpcMessage {
  jsonrpc: '2.0';
  id?: string | number | null;
  method?: string;
  params?: unknown;
  result?: unknown;
  error?: { code: number; message: string; data?: unknown };
}

export interface StdioTransportOptions {
  cwd?: string;
  env?: Record<string, string>;
  onNotification?: (m: RpcMessage) => void;
  onStderr?: (s: string) => void;
  onExit?: (code: number | null, signal: string | null) => void;
}

export declare class StdioTransport {
  constructor(argv: string[], opts?: StdioTransportOptions);
  send(method: string, params?: unknown, timeoutMs?: number): Promise<unknown>;
  close(): Promise<void>;
  readonly closed: boolean;
}

export declare function resolveBinary(): string;
export declare function platformTripleHint(): string;

export interface ToolDef {
  name: string;
  description?: string;
  inputSchema?: unknown;
}

export interface McpClientOptions extends Omit<StdioTransportOptions, never> {
  bin?: string;
  toolsets?: string;
  tier?: string;
}

export interface ToolCallResult {
  content: Array<{ type: string; text?: string } & Record<string, unknown>>;
  isError?: boolean;
}

export declare class McpClient {
  constructor(opts?: McpClientOptions);
  initialize(timeoutMs?: number): Promise<{ serverInfo?: { name: string; version: string } } & Record<string, unknown>>;
  listTools(timeoutMs?: number): Promise<ToolDef[]>;
  callTool(name: string, args?: Record<string, unknown>, timeoutMs?: number): Promise<ToolCallResult>;
  close(): Promise<void>;
}

export interface AcpClientOptions {
  bin?: string;
  cwd?: string;
  env?: Record<string, string>;
  onUpdate?: (u: { sessionId: string; update: any }) => void;
  onStderr?: (s: string) => void;
}

export declare class AcpClient {
  constructor(opts?: AcpClientOptions);
  initialize(timeoutMs?: number): Promise<Record<string, unknown>>;
  createSession(cwd?: string): Promise<{ sessionId: string }>;
  prompt(sessionId: string, text: string, timeoutMs?: number): Promise<Record<string, unknown>>;
  cancel(sessionId: string): Promise<void>;
  close(): Promise<void>;
}
