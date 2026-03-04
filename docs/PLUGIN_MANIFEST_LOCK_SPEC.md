# Plugin Manifest + Lockfile Spec

Date: March 3, 2026
Owner: Platform
Backlog item: `P0-EXT-001`

## Goal

Define a minimal, deterministic metadata contract for plugin packaging and pinning:

- `plugin-manifest.json` defines plugin identity and declared capabilities.
- `plugins.lock` pins allowed plugin name/version/hash tuples.

Runtime validation entrypoints:

- Slash command: `/plugins validate [manifest_path] [lock_path]`
- Tool: `plugin_validate`

If paths are omitted, defaults are used:

- `~/.dsco/plugins/plugin-manifest.json`
- `~/.dsco/plugins/plugins.lock`

## `plugin-manifest.json`

Required fields:

1. `name` (string)
2. `version` (string)
3. `hash` (string, 64-char hex sha256 digest)
4. `signer` (string)
5. `capabilities` (non-empty array of strings)

Example:

```json
{
  "name": "demo-plugin",
  "version": "1.2.3",
  "hash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "signer": "acme-signing",
  "capabilities": ["read_file", "code_search"]
}
```

## `plugins.lock`

Required top-level fields:

1. `schema_version` (integer, `>= 1`)
2. `plugins` (non-empty array)

Each `plugins[]` entry must include:

1. `name` (string)
2. `version` (string)
3. `hash` (string, 64-char hex sha256 digest)

Plugin names must be unique in the lockfile.

Example:

```json
{
  "schema_version": 1,
  "plugins": [
    {
      "name": "demo-plugin",
      "version": "1.2.3",
      "hash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    }
  ]
}
```

## Cross-File Rule

Validation requires the manifest tuple `(name, version, hash)` to be present in `plugins.lock`.
If not present, validation fails closed.
