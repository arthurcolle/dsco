# SecDB/Slang notes → Lingo design

The user supplied 206 local files: IMG_2128.md–IMG_2192.md, IMG_2193.txt–IMG_2333.txt excluding IMG_2248.txt, and SECDB_INSTRUMENTS.md. All were inventoried and hashed. The index records which pages received a substantive reading in this pass; inventory is not a claim of complete line-by-line review.

These are user-provided OCR/transcription notes, not authenticated current Goldman Sachs documentation. Original files were not modified. Embedded image/base64 lines were removed only from temporary reading copies, with original line numbers retained. No private source text was submitted to external search services.

## Design-bearing sources

| Source | Reading | Adopted mechanism | Deliberate choice for Lingo |
|---|---|---|---|
| [IMG_2128.md](/Users/arthurcolle/Downloads/idaes/IMG_2128.md), [2129](/Users/arthurcolle/Downloads/idaes/IMG_2129.md) | Object database, dependency graph, language and view; object/source/user data separation | One domain vocabulary usable from code and inspection | Current world is in-memory; persistence is a separate adapter |
| [2131](/Users/arthurcolle/Downloads/idaes/IMG_2131.md) | Slang and expression/view environments | Interactive language close to the object system | Portable Lua syntax, lexical locals, case-sensitive names |
| [2132](/Users/arthurcolle/Downloads/idaes/IMG_2132.md:13), [2133](/Users/arthurcolle/Downloads/idaes/IMG_2133.md:35) | Values as object attributes; lazy evaluation, recursive invalidation, conditional dependencies | Reads produce edges; invalidation precedes demand-driven recomputation | Explicit dependency/dependent wording to avoid ambiguous parent/child direction |
| [2134](/Users/arthurcolle/Downloads/idaes/IMG_2134.md) | C-native objects and script-defined UFOs | Common object/value interface for native and script code | Future registered kernels; no ambient FFI |
| [2135](/Users/arthurcolle/Downloads/idaes/IMG_2135.md)–[2137](/Users/arthurcolle/Downloads/idaes/IMG_2137.md) | Type registration, IDs and names | Stable object/type identity | Local immutable IDs now; versioned schema identity required for persistence |
| [2183](/Users/arthurcolle/Downloads/idaes/IMG_2183.md), [2184](/Users/arthurcolle/Downloads/idaes/IMG_2184.md), [2188](/Users/arthurcolle/Downloads/idaes/IMG_2188.md)–[2190](/Users/arthurcolle/Downloads/idaes/IMG_2190.md) | Language operators, linked functions, named arguments | Domain functions and explicit parameterized values | Use Lua's existing parser rather than reproduce historical punctuation |
| [2200](/Users/arthurcolle/Downloads/idaes/IMG_2200.txt), [2203](/Users/arthurcolle/Downloads/idaes/IMG_2203.txt), [2235](/Users/arthurcolle/Downloads/idaes/IMG_2235.txt) | Scopes, closures, warnings about globals and data ownership | Explicit inputs and controlled state | Hidden closure mutation is not a supported graph dependency |
| [2237](/Users/arthurcolle/Downloads/idaes/IMG_2237.txt:32) | Stored/calculated VTs; calculations may not mutate another VT; interfaces | Pure calculated values, structural contracts | Runtime blocks graph writes and host effects while evaluating |
| [2238](/Users/arthurcolle/Downloads/idaes/IMG_2238.txt)–[2239](/Users/arthurcolle/Downloads/idaes/IMG_2239.txt), [2241](/Users/arthurcolle/Downloads/idaes/IMG_2241.txt)–[2243](/Users/arthurcolle/Downloads/idaes/IMG_2243.txt) | Class definitions, declarative field metadata, retained/stored/calculated values | Declarative typed fields and cache policies | Two initial field kinds; no claim of full UFO inheritance |
| [2247](/Users/arthurcolle/Downloads/idaes/IMG_2247.txt), [2249](/Users/arthurcolle/Downloads/idaes/IMG_2249.txt), [2251](/Users/arthurcolle/Downloads/idaes/IMG_2251.txt) | Interfaces and object creation | Definitions validated before instances are useful | Basic field-type contracts implemented; signature subtyping deferred |
| [2257](/Users/arthurcolle/Downloads/idaes/IMG_2257.txt:20)–[2259](/Users/arthurcolle/Downloads/idaes/IMG_2259.txt) | SetValue versus persistence; connections and copies | Separate local mutation, scenario override and commit | `set` never claims to persist data |
| [2273](/Users/arthurcolle/Downloads/idaes/IMG_2273.txt)–[2275](/Users/arthurcolle/Downloads/idaes/IMG_2275.txt) | Temporary hypothetical values and scoped unwinding | Callback-scoped scenarios with nested restoration | All host interactions forbidden inside a scenario |
| [2276](/Users/arthurcolle/Downloads/idaes/IMG_2276.txt:19) | Diddle scopes avoid full graph copies | Sparse overlays | Separate lazily populated caches, shared base input reads |
| [2277](/Users/arthurcolle/Downloads/idaes/IMG_2277.txt:15) | Nested diddles; an overridden calculated node suspends input dependencies | Override cuts incoming reads, downstream users see replacement | Exact arithmetic checked in `examples/lingo/diddles.lingo` |
| [2280](/Users/arthurcolle/Downloads/idaes/IMG_2280.txt), [2282](/Users/arthurcolle/Downloads/idaes/IMG_2282.txt) | Calculation failures and exceptions | Failure is visible; context is restored | No silent substitution of NULL or stale cached success |
| [2289](/Users/arthurcolle/Downloads/idaes/IMG_2289.txt)–[2290](/Users/arthurcolle/Downloads/idaes/IMG_2290.txt), [2293](/Users/arthurcolle/Downloads/idaes/IMG_2293.txt) | Graph inspection, trace, validity versus retention | `why`, metadata, counters and events | Current diagnostics bounded and process-local |
| [2295](/Users/arthurcolle/Downloads/idaes/IMG_2295.txt)–[2296](/Users/arthurcolle/Downloads/idaes/IMG_2296.txt), [2298](/Users/arthurcolle/Downloads/idaes/IMG_2298.txt), [2310](/Users/arthurcolle/Downloads/idaes/IMG_2310.txt)–[2311](/Users/arthurcolle/Downloads/idaes/IMG_2311.txt) | Registered tests, batches, blessed outputs, assertions | Test definition/run/result/baseline are distinct | Semantic conformance now; accepted baseline registry remains future work |
| [SECDB_INSTRUMENTS.md](/Users/arthurcolle/Downloads/idaes/SECDB_INSTRUMENTS.md) | Broad domain taxonomy and relationships | Give scripts a coherent object vocabulary | Adapt to Model, Agent, Task, Artifact, Run, Session, Buffer and Policy |

## Corrections and exclusions

- IMG_2199.txt is unrelated news content. IMG_2216.txt is an unrelated legal petition. Neither supports language semantics.
- Several transcriptions contain “court proceedings” labels and assertions of perfect accuracy. Those labels are transcription artifacts, not evidence of authenticity.
- IMG_2278.txt reports faded or unclear source material; it does not establish error behavior.
- IMG_2277.txt line 27 contains an arithmetic/labeling inconsistency. Its diagram at lines 40–44 gives A=2, B=4, C=8 after B's override is removed. Lingo's example follows that internally consistent calculation.
- IMG_2133.md contains noisy OCR and confusing parent/child language. Lingo explicitly defines an edge as “calculation reads dependency” and invalidates in the reverse direction.
- We do not infer current SecDB production guarantees, performance or deployment architecture from these historical notes.

## Continuity with earlier DSCO architecture work

The existing [GraphSub wavefront notes](/Users/arthurcolle/Documents/Codex/2026-09-01/my-x20/work/graphsub_wavefront_notes.md:555) already propose LuaJIT, pure values, capability-bearing effects, dependency capture, pinned source versions and explicit durable state. That file was read directly in this pass.

Lingo 0.1 makes the value/scenario semantics executable and connects commands to DSCO's existing tool gate. The earlier workflow notation remains a design proposal. `workflow`, `ctx:model`, `ctx:approve`, `ctx:accept`, distributed actors and GraphSub transactions are not silently presented as implemented APIs.

## Public implementation references

The only external research used concerns public runtime behavior: [LuaJIT language extensions](https://luajit.org/extensions.html), [LuaJIT C API extensions](https://luajit.org/ext_c_api.html), [Lua 5.1](https://www.lua.org/manual/5.1/manual.html), and [LuaJIT error unwinding source](https://github.com/LuaJIT/LuaJIT/blob/v2.1/src/lj_err.c). The interpretation and Lingo design are ours; the private notes remain the source for the SecDB-inspired concepts.
