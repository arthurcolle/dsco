// context_fabric.thrift — the typed contract for the Context Fabric.
//
// Single source of truth for the fabric's types across every language in the
// stack: C (the existing runtime), Rust (the new typed core), and Erlang
// (agents.erl / OTP orchestration). Generate with:
//
//   thrift --gen rs      idl/context_fabric.thrift   # Rust core
//   thrift --gen erl     idl/context_fabric.thrift   # agents.erl orchestration
//   thrift --gen c_glib  idl/context_fabric.thrift   # C runtime bridge
//
// Note what this eliminates: there are no `char id[128]` / `preview[160]`
// magic sizes here. A string is a string; the generated code carries a real
// length. Sizes only appear where they are a genuine invariant (see CID_HEX_LEN
// below), named and derived, never a literal chosen by vibes.

namespace cpp   dsco.context
namespace rs    dsco_context
namespace erl   dsco_context

// A content id is the lowercase hex of a sha256 digest: exactly 64 bytes.
// This is the ONE size that is a real invariant, so it is named and derived
// here and everywhere downstream — not redefined per call site.
const i32 CID_HEX_LEN = 64

enum CtxKind {
  BLOB  = 0,   // content-addressed immutable bytes (default)
  FILE  = 1,   // snapshot of a file's contents
  MSG   = 2,   // a conversation message
  TOOL  = 3,   // a tool result
  MEM   = 4,   // a named memory-tier entry
  PLAN  = 5,   // a cached plan
  RUN   = 6,   // a chronicle run artifact
  SCOPE = 7,   // a bundle of ctxkeys (unit of delegation)
}

enum SliceKind {
  NONE  = 0,   // whole object
  BYTES = 1,   // [start, end)  — end < 0 means "to end"
  LINES = 2,   // [start, end]  — 1-based inclusive; end < 0 means "to end"
}

struct CtxSlice {
  1: SliceKind kind  = SliceKind.NONE,
  2: i64       start = 0,
  3: i64       end   = -1,
}

// The universal handle. Immutable kinds carry a sha256-hex `id`; named kinds
// (MEM/SCOPE) carry a caller-chosen id.
struct CtxKey {
  1: required CtxKind  kind,
  2: required string   id,
  3: optional CtxSlice slice,
}

struct PutOpts {
  1: CtxKind         kind       = CtxKind.BLOB,
  2: optional string source,      // provenance: file path, tool name, session id
  3: optional string tags,        // comma-separated
  4: double          importance = 0.5,
  5: i32             embed      = -1,  // -1 auto (embed if textual), 0 never, 1 force
}

struct PutResult {
  1: required CtxKey key,
  2: required bool   deduped,
  3: required i64    size,
}

struct Hit {
  1: required CtxKey key,
  2: required double score,
  3: required i64    size,
  4: optional string source,
  5: optional string preview,
}

struct Stat {
  1: i32  blobs,
  2: i64  bytes,
  3: i32  vectors,
  4: i32  pinned,
  5: i32  scopes,
  6: bool embeddings_available,
}

exception CtxError {
  1: required string message,
}

// The broker as an RPC service. This is what agents.erl calls to pass scopes
// (not content) between orchestrated agents, and what the Rust core implements
// over the C storage backend.
service ContextFabric {
  PutResult    put         (1: binary content, 2: PutOpts opts) throws (1: CtxError err),
  binary       get         (1: CtxKey key)                       throws (1: CtxError err),
  // kind_filter < 0 ⇒ any kind (matches the C ctx_search contract).
  list<Hit>    search      (1: string query, 2: i32 kind_filter, 3: i32 max)
                                                                 throws (1: CtxError err),
  CtxKey       scope_create(1: list<CtxKey> keys)                throws (1: CtxError err),
  list<CtxKey> scope_resolve(1: CtxKey scope)                    throws (1: CtxError err),
  bool         pin         (1: CtxKey key),
  bool         unpin       (1: CtxKey key),
  Stat         stat        (),
}
