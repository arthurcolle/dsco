# Integration Catalog

DSCO treats external app marketplaces as versioned integration catalogs, not as proof of live callable tools.

## Codex app directory importer

The importer reads JSON from:

1. `DSCO_CODEX_APP_DIRECTORY`, if set
2. `~/.dsco/codex_app_directory.json`, otherwise

Accepted top-level array keys are `apps`, `connectors`, or `items`; a bare array is also accepted. Each entry may use either snake_case or camelCase fields:

- `id`, `app_id`, `name`
- `connector_id`, `connectorId`, `slug`
- `display_name`, `displayName`, `title`
- `distribution_channel`, `distributionChannel`, `distribution`, `source`
- `categories`, `category`
- `labels`
- `scope`
- `catalog_status`, `catalogStatus`, `status`
- `isEnabled` / `is_enabled`
- `isAccessible` / `is_accessible`
- `interactive`, `consequential`, `retrievable`, `sync`

## Label-to-policy mapping

Catalog labels map directly into DSCO governance metadata:

| Catalog label | DSCO action flags |
|---|---|
| `retrievable` | `read`, `untrusted_content` |
| `sync` | `read`, `untrusted_content`, `sync_capable` status |
| `consequential` | `write`, `requires_confirmation` |
| `interactive` | `interactive` |

Mutating connectors are therefore confirmation-gated by metadata, not by hardcoded provider names.

## Tools

- `discover_integrations` — returns cached, installed, connected, live, inaccessible, stale, OAuth-gated, mutating, and sync-capable status.
- `dsco_doctor_integrations` — compares live MCP registrations to the cached catalog and flags stale connector IDs, missing catalog/auth/install state, dangerous mutating connectors, and governance/control-plane candidates.

## Vertical profiles

Discovery exposes install profiles for:

- `engineering`
- `gtm`
- `finance`
- `enterprise_knowledge`
- `governed_agent_runtime`

These are sales/install bundles with default allow/deny posture, confirmation policy, and audit expectations.

## Control-plane integrations

Governance/meta integrations such as ToolCheck, Agent Ready, HAPI MCP Registry, app monitoring, and enterprise-context systems are control-plane tools. They should not be treated like ordinary business apps.
