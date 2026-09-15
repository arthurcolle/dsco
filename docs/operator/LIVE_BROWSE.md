# Live GraphSub browsing from Lingo

The implemented `lingo.operator` source provides a small read-only attachment to the native GraphSub HTTP API. It exposes the backend's current health, dynamic-schema listing, node pages and individual nodes through DSCO's governed tool bridge. It does not claim the full [operator language target](LINGO_OPERATOR_LANGUAGE.md) is implemented.

This document describes the implemented source contract. The checkout binary passed 65 operator checks, including a read against an isolated native GraphSub engine. See the [retained validation and compatibility review](../../reports/lingo-operator-20260909/REVIEW.md) for the tested binary identities and boundaries. This does not certify another installed binary or deployment.

## Configure the host and run

The DSCO process reads `GRAPHSUB_HOST`; its default is `http://127.0.0.1:7879`. `GRAPHSUB_API_KEY` is optional host configuration when the selected server requires it. Scripts do not receive the key and cannot supply another endpoint or authentication value in `attach`. Avoid printing credential environment variables.

The configured URL must be an origin, with no user information, base path, query or fragment. Plain HTTP is permitted only on loopback; remote origins require HTTPS with certificate and hostname verification. The native transport uses fixed GET routes, disables redirects and ambient proxies, and ignores netrc credentials. Server authentication and authorization remain the selected server's responsibility; choosing a shard is not a tenant permission grant.

Only the named connection `default` exists in this implementation:

```lua
local o = require("lingo.operator")
local scope = o.attach {connection="default", shard_id=0}
local status = scope:status()
local schema = scope:schema()
local page = scope:list {limit=5, offset=0}
```

`attach` performs a health request through the same governed bridge; it does not merely construct an unchecked URL. The returned `BrowseScope` is immutable. Its private connection and shard cannot be changed by editing a returned response.

From the DSCO checkout, with an existing native GraphSub service selected:

```sh
GRAPHSUB_HOST=http://127.0.0.1:7879 ./dsco lingo run \
  examples/lingo/graphsub-browse.lingo '{"shard_id":0,"limit":5}'
```

The [executable example](../../examples/lingo/graphsub-browse.lingo) returns health, schema and node-page envelopes. It reads the first returned node, or the opaque ID supplied through `args.node_id`. An empty page produces an explicit skipped-read result. The selected node's payload is summarized to at most 256 bytes; the example labels that projection.

## The four operations

| Lingo method | Native read | Return payload in `envelope.data` |
|---|---|---|
| `scope:status()` | `GET /health` | Backend health JSON |
| `scope:schema()` | `GET /api/v1/dynamic-schemas` | Dynamic extraction-schema listing, including `schemas` |
| `scope:list{limit, offset}` | `GET /api/v1/shards/{shard_id}/nodes` | `nodes`, `total`, `offset`, `limit` |
| `scope:read(node_id)` | `GET /api/v1/shards/{shard_id}/nodes/{encoded_node_id}` | One native node projection |

`schema()` is the existing **dynamic-schema listing**. An empty `schemas` list is a legitimate response. It does not mean the graph has no ontology, registered types or built-in objects, and this endpoint is not a complete unified source/class catalog.

The wrapper defaults to `limit=25`, `offset=0`; the example defaults to five rows. Allowed limits are 1–100, offsets 0–10,000,000, and shard IDs 0–2,147,483,647. Unknown arguments fail. No class/type filter is offered by this binding. Selecting a shard identifies the backend graph to read; a health response alone does not certify that the shard is populated.

Use a node's opaque string `id` exactly as returned by GraphSub:

```lua
local page = scope:list {limit=1, offset=0}
if page.data.nodes[1] then
  local node = scope:read(page.data.nodes[1].id)
  return node
end
return {state="empty_page"}
```

The native adapter encodes the path segment, and the backend validates the ID and its embedded shard. Do not substitute the numeric `node_id`, decode and reconstruct the string, or assume IDs from another shard are interchangeable.

Native list and read projections differ. The current list includes fields such as `type_id`, `confidence` and `epistemic_status` at the node's top level. The individual read returns `payload` and a `metadata` object containing confidence and epistemic status. Both provide the canonical `id`, name and type. The facade preserves these backend shapes rather than pretending they are one richer persistent object interface.

## What an envelope means

A successful operation returns an envelope with these fields:

```lua
return {
  ok=true,
  profile="browse",
  consistency="live",
  snapshot=false,
  connection="default",
  shard_id=0,
  action="list",
  observed_at="...",
  data={nodes={}, total=0, offset=0, limit=25},
}
```

This is a shape illustration, not an observed result. The observation timestamp identifies when the wrapper observed the response; it is not a GraphSub snapshot token or a claim about every underlying field's source time.

Each operation is an independent live read. List offset pagination is not a stable snapshot cursor. Concurrent changes can affect the next page or a subsequent individual read. `total` is what the backend reported for that request, not a guarantee that the script has retrieved every node. The example does not infer completeness from a short or empty page.

The native backend clamps an offset beyond the end to `total` and returns an empty page. The envelope preserves that returned offset. Display names and type names may be empty strings; opaque IDs remain nonempty.

Fresh-engine ontology population is configuration/version dependent. One current test setup contained 243 automatically created ontology nodes; no example or API contract relies on that number. A schema listing can still be empty in a populated graph because it lists dynamic extraction schemas separately.

There is no `scope:context()`, pinned evaluation context, persistent Lingo class binding, scenario service, transaction or write method in this profile. Calling an unsupported method fails explicitly. Data can be copied into an explicit local `l.world()` as an observation for a separate experiment, but that world is not a live or automatically synchronized GraphSub attachment.

## Governing calls and handling errors

The facade captures `lingo.call`, `lingo.decode` and `lingo.try`. Every request invokes the native `graphsub_operator` tool through `tools_execute_for_tier()` with the script's tier and session taint. Network permissions apply to these reads. Loading the module does not grant network access.

The existing Lingo runtime rejects all DSCO interactions from inside a calculated value or scenario. That includes `attach`, `status`, `schema`, `list` and `read`. Gather observations before entering local calculations/scenarios; invoking the read-only facade is still a real external interaction.

Capability denials, failed HTTP/backend operations, invalid JSON, mismatched response scope and malformed browse provenance fail the script operation. Use the existing `l.try` when the caller has a meaningful recovery path:

```lua
local l = require("lingo")
local o = require("lingo.operator")
local ok, result = l.try(function()
  return o.attach {connection="default", shard_id=0}:list {limit=5}
end)
if not ok then return {state="unavailable", error=result} end
return result
```

This does not catch terminal execution-budget exhaustion. It does not reinterpret a denied or failed read as an empty graph. Native tool/JSON/output bounds still apply; a truncated response must not become a successful partial JSON value.

Each native request has a three-second connection limit, ten-second total limit and 128 KiB response-body limit. Larger replies fail with `response_too_large`; complete accepted JSON is preserved through the tool bridge. Invalid structure, duplicate object keys, nesting beyond 64 levels, or HTTP 200 error bodies fail with `malformed_response`. Numbers outside the adapter's supported exact-integer magnitude (2^53−1) fail with `precision_unsupported` rather than silently rounding IDs. Lingo's separate source, memory, instruction, tool-call and encoded-result budgets still apply.

Other native error codes include `invalid_request`, `bad_configuration`, `allocation_failed`, `transport_error`, `timeout`, `http_error`, `clock_error` and `response_buffer_too_small`. The native result is `{ok=false, error={code, message, http_status?}}`; the facade raises it through the existing Lingo error path. Capability denials remain recognizable gate errors. Authentication configuration and untrusted response bodies are not copied into native error messages.

## Start an isolated native engine

The existing sibling binary can be started with a fresh working directory and fresh storage, leaving any existing database untouched. Choose unused loopback ports; the example uses 17878/17879 so it does not assume the default listener is free.

Run this in a dedicated terminal:

```sh
graphsub_bin=/Users/arthurcolle/Dsco/dsco-graphsub-complex/dsco-graphsub/target/release/graphsub
browse_dir="$(mktemp -d "${TMPDIR:-/tmp}/dsco-graphsub-browse.XXXXXX")"
cd "$browse_dir"
"$graphsub_bin" start \
  --bind 127.0.0.1:17878 \
  --http 127.0.0.1:17879 \
  --persist "$browse_dir/data" \
  --memory-max 64MB \
  --shard-count 1
```

In another terminal:

```sh
cd /Users/arthurcolle/Dsco/dsco-cli
GRAPHSUB_HOST=http://127.0.0.1:17879 ./dsco lingo run \
  examples/lingo/graphsub-browse.lingo '{"shard_id":0,"limit":5}'
```

The server remains in the foreground; stop that dedicated process with Ctrl-C when finished. The browse script sends only the four reads above. Starting a fresh engine may create its own ontology/storage state, which is separate from a script writing domain objects. The launch command requests persistence; its actual durability behavior remains the backend's advertised and tested behavior, not a promise added by this facade.
