# Named worlds, source identity and GraphSub artifacts

Implemented in core API 0.2. `lingo.platform` and `lingo.workspace` module APIs are
0.1. See [the platform language definition](../../../LINGO.md) for the full model.

## World and declaration identity

`l.world{id="company:review"}` creates an empty world. IDs are 1–128 bytes, with
no ASCII control characters. Omitted IDs become invocation-local `local:N` names;
use an explicit stable ID when persisting or exchanging addresses. Two handles
with the same textual ID are still separate worlds; object handles cannot cross
between them. Loading reconstructs new handles with the same stored identities.

`w:define(name,fields,{version="1",immutable=true})` declares a class. `version`
is a descriptive string, 1–128 bytes. `immutable` rejects every base `set`, even
an equal-value set. A scenario can still override fields for hypothetical analysis.
An Observation is immutable because new evidence deserves a new identity.

`w:manifest()` returns a defensive record:

```text
format: "lingo.definitions/1"
runtime: {api, compiler, runtime_sha256, world_io_sha256}
classes: {
ClassName: {
    version, immutable, implements, source: {module, sha256, line, last_line},
    fields: {
      field: {kind, type, params, cache,
              has_default/default OR source: {..., prototype_sha256}}
    }
  }
}
```

Source identity comes from the native host's private registry, not a caller-supplied
version string or filename. Entry source has a host-generated label containing
its source SHA. Embedded modules have registered module identities. Native C
functions cannot be installed directly as derived values; an explicit kernel
binding would need its own contract.

Function prototypes are hashed using LuaJIT's deterministic bytecode dump mode
`d`. Ordinary dumps can vary between processes due to VM hardening; the
[LuaJIT documentation](https://luajit.org/extensions.html#string_dump) specifies
the deterministic mode. Programs cannot access the dump function or load bytecode.
The dump is used for identity only; world files contain no bytecode. Compiler
identity and the two core module hashes are also part of the compatibility check.

The fingerprints do not serialize mutable captured variables, authenticate an
author, prove purity, or constitute a package signature. Custom calculations must
read changing data through stored fields. Arbitrary shared-library loading and
package resolution are outside this runtime profile.

Compatibility also requires maintaining the core API version when the native host
changes semantics. The manifest includes the Lua core sources and compiler version,
not a hash of every C implementation or shared library. Retain the executed binary
identity alongside artifacts for stronger historical reproduction.

## ValueAddress

Create an address with `w:address(object,field,args)`. It is a JSON record:

```text
{format:"lingo.value/1", world_id, object_id, class, field,
 args:{...}, definition_sha256, runtime:{...}}
```

Arguments are validated against the field's declared parameter types. Reference
arguments encode as object ID strings. Lists, records and optionals recurse by
their declared types. Ordinary JSON is preserved as ordinary JSON, even when it
contains a field named `$ref`. Missing and extra calculation arguments are errors.

`definition_sha256` hashes the core's canonical representation of the complete
class manifest. It is not the hash of an arbitrary JSON encoding. `w:read(address)`
validates world, runtime, class and definition identity, resolves reference
arguments, then uses normal `get` semantics. `w:explain(address)` uses normal `why`
semantics. `address` creation requires base script scope. Reads work in calculated
values and scenarios and capture dependencies normally; inspection stays outside
calculated values.

This address selects a logical value, not an input revision. Changing a stored
input can change its answer. A scenario can produce a different answer at the same
address. Retain the WorldArtifact address alongside it to select a stored version.
No live service call occurs merely because an address is read.

## Snapshot and atomic load

`w:snapshot()` returns:

```text
{format:"lingo.world/1", world_id, runtime, classes,
 objects:[{id,class,revision,values:{stored_field:encoded_value,...}},...]}
```

Object records are ordered by ID. Portable IDs are at most 512 bytes and class
names at most 1,024 bytes, without ASCII control characters. Stored
references encode by ID according to the declared type. Revisions are exact
positive integers. Derived values, memo state, dependency edges, events, scenarios,
Lua stacks, closures, host credentials and tool handles are not serialized by the
runtime. A user can put sensitive data in a JSON stored field, so explicitly
select the data appropriate to the destination before publishing.

`w:load(snapshot)` requires an empty, already-declared world. It validates the
exact format/fields, world ID, runtime identity and class manifests, then stages
all object identities. A second pass validates every stored value and resolves
references against those identities. Only after every check passes does it install
the new object map and empty calculation cache. Forward and cyclic stored
references are valid; cycles encountered while calculating a value still fail.
Failed admission leaves the world empty. Load neither merges objects nor runs
stored executable code. Empty serialized lists must be JSON arrays. Base writes
reject revision overflow before changing values; an equal-value no-op remains valid.

Portable class manifests reject defaults containing concrete object references:
an empty declared world cannot recreate those referenced objects before loading.
Use explicit references in `w:new` values instead. Optional null references and
empty lists remain valid defaults.

The core load limit is 4,096 objects; JSON conversion has its normal 256 KiB,
16,384-value and depth-64 limits. The GraphSub publication limit is smaller. A snapshot
need not be publishable if it exceeds that storage boundary. Export and load are
forbidden inside calculations and scenarios. A hypothetical result can be returned
as data, but the scenario itself is never silently saved as the base world.

Source changes fail admission rather than silently rebinding historical state.
Embedded platform classes can be loaded by a different entry program because their
source is the embedded module. Custom classes use the exact defining script;
invoke the same source with different arguments to choose save/open behavior.
Even comment changes in that script change its source identity. Keep a compatible
runtime and definitions for historical replay. There is no automatic migration
or legacy-source download. Data migration requires a separately reviewed program
that explicitly creates a new world under the new definitions.

## GraphSub transport

```lua
local p=require("lingo.platform")
local workspace=require("lingo.workspace")
local w=p.world{id="company:review"}
-- Populate w explicitly, or supply an existing empty artifact's address.
local artifact, published=workspace.publish(w,{connection="default",shard_id=0})
local address_record=artifact:inspect()
local reopened=p.world{id="company:review"}
workspace.open(address_record,reopened)
return {artifact=address_record,world=reopened:snapshot()}
```

`workspace.address(record)` validates and wraps an existing locator in an immutable
handle. Its `inspect()` method returns a copy. The record fields are:

```text
{connection:"default", shard_id, node_id:<opaque GraphSub ID>,
 sha256:<lowercase SHA256 of exact stored UTF-8 JSON>, world_id}
```

The native `graphsub_world` tool supports exactly two actions:

| Action | Required inputs | Operations and guarantees |
|---|---|---|
| `publish` | `snapshot` exact JSON string; optional default connection/shard | Validate before network; inspect persistence; POST a new `State` node; GET/readback; compare exact text and digest; inspect commit/WAL evidence |
| `read` | `node_id`, `sha256`, `world_id`; optional default connection/shard | Inspect persistence; GET exact node; validate type/world/format/digest; return stored text |

Successful results contain `ok=true`, `profile="world_artifact"`, action,
`consistency="content_verified"`, `persistence="server_reported"`, the artifact,
snapshot text and before/after persistence summaries. The adapter uses existing
GraphSub REST and WAL-backed creation; it does not create a second storage engine.
Publication uses one node because the native payload boundary is 65,535 bytes.
The admitted image limit is **49,152 UTF-8 bytes**, not characters.

Native input is bounded to 128 KiB; HTTP responses to 128 KiB; each request has a
3-second connection and 10-second total timeout. A publication makes multiple
requests, so the whole tool can take longer than 10 seconds. Origins come from
`GRAPHSUB_HOST`, with optional host credential `GRAPHSUB_API_KEY`. URL/userinfo/
query/fragment overrides are not script inputs. HTTPS verifies certificates;
plaintext is loopback-only. Redirects, proxies and netrc credential lookup are off.
Destination checks preserve the action so a read under `DSCO_ALLOW_WRITE=0` remains
a read while still respecting the configured network allowlist.

Every call goes through `tools_execute_for_tier`. Publication requires `net`,
`untrusted_in` and `fs_write`; reads require `net` and `untrusted_in`. The existing
session taint/control/grant rules continue to apply. A Policy object does not
change any of these capabilities. The outer Lingo tool separately requires exec.

## Failure and retry semantics

The native error record is `{ok:false,error:{code,message,effect,...}}`. A failure
before any POST is attempted reports `effect="none"`. Once a POST is attempted,
subsequent failures conservatively report `effect="unknown"`, including HTTP
errors, timeouts, bad readback and missing persistence progress. If the created
node ID was accepted in a valid creation receipt, the error retains its artifact
locator for reconciliation.

There is no automatic retry or deduplication key. Repeating a publication can
create another version. Losing the response can leave an artifact that the caller
does not know how to locate; this binding supplies no reliable world/name lookup
for reconciliation. Do not treat transport failure as a rollback. The Lua facade
raises an error containing the native result; use `l.call("graphsub_world",...)`
directly when a program needs to inspect its structured uncertain outcome.

A digest mismatch rejects a modified artifact. It does not prove who modified it,
that the service will retain it, or that all observations inside it were captured
at one database revision. Current receipts do not establish mutable-head CAS,
atomic multi-node changes, live invalidation streams or resumable workflows.

## Verification

```sh
make -j6 dsco
python3 tests/test_lingo_world.py ./dsco
python3 tests/test_lingo_platform.py ./dsco
python3 tests/test_lingo_workspace.py ./dsco
python3 scripts/verify_lingo_workspace.py ./dsco
```

The last script launches an isolated actual GraphSub engine, publishes two worlds,
kills only its owned process, restarts with the same data, and reopens both under
read-only grants. The [retained real restart proof](../../reports/lingo-world-20260910/real-restart-proof.json)
and [adversarial transport checks](../../reports/lingo-world-20260910/transport-tests.json)
separate server-path evidence from controlled failure fixtures.
