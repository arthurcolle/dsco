# Security Policy

## Supported Versions

Security fixes are applied to the active default branch.

## Reporting a Vulnerability

Preferred path:

1. Use GitHub Private Vulnerability Reporting in the repository Security tab.
2. Include reproduction steps, impacted components, and severity estimate.
3. Provide proof-of-concept only when necessary.

If private reporting is unavailable, open a minimal issue without exploit details and request a private channel.

## Response Targets

- Initial triage: within 5 business days
- Status update cadence: at least weekly until resolution
- Coordinated disclosure after patch availability

## Scope Notes

High-priority areas include:

- tool execution and shell command pathways
- API key handling and env persistence
- IPC/shared SQLite coordination
- plugin and MCP process boundaries
- static file serving and path traversal controls
