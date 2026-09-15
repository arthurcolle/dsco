# SecView lineage for the DSCO operator

This document grounds the [DSCO–GraphSub operator definition](../DSCO_GRAPHSUB_OPERATOR.md) in the supplied SecDb/Slang notes. It defines a target operator experience, not implemented functionality. The executable subset and its limits are documented in [Lingo](../LINGO.md). The recommendations below are DSCO design decisions informed by the notes; they are not claims about GraphSub's current service guarantees.

The source is a local OCR corpus of a historical primer. Links point to the original files and original line numbers. Spelling, punctuation and some examples contain transcription errors. Transcriber assertions about accuracy are not treated as evidence.

## The operator is the continuity between activities

The notes describe SecView as an integrated environment for navigating database objects, editing and evaluating Slang, debugging calculations, and examining dependency graphs and inheritance. They also describe SecExpr as the command-line evaluator. This establishes the important relationship: the interactive environment and the language operate on the same world of objects and values. [IMG_2156.md:21–25](/Users/arthurcolle/Downloads/idaes/IMG_2156.md:21), [IMG_2131.md:13–27](/Users/arthurcolle/Downloads/idaes/IMG_2131.md:13).

DSCO should preserve that continuity. An operator selects an object, opens a value, follows a dependency, inspects its implementation, evaluates a source change in a workspace, compares a scenario, and commits an explicit change. Each transition carries stable object, node, source and context identities. The system must not require reconstructing those identities from a conversation transcript.

The interactive terminal, headless commands and Lingo scripts should invoke the same underlying operations. Presentation can differ; calculation semantics, authorization, source resolution and operation receipts cannot. A saved evaluation must contain enough context to reproduce what was inspected interactively.

## Separate object context from source context

The primer distinguishes object, source and user databases. An object database supplies objects. A source database supplies scripts to load or link. Both can be ordered unions: resolution checks the first database, then successive databases until it finds the requested item. A user database holds a developer's objects and scripts. [IMG_2129.md:17–37](/Users/arthurcolle/Downloads/idaes/IMG_2129.md:17), [IMG_2130.md:5–11](/Users/arthurcolle/Downloads/idaes/IMG_2130.md:5).

The notes explicitly allow development source from a user database to run against objects from another database. They also allow viewing the user database alone, without its fallback databases. [IMG_2174.md:1–5](/Users/arthurcolle/Downloads/idaes/IMG_2174.md:1).

DSCO therefore needs an explicit context rather than one ambiguous “current database”:

| Context component | Operator meaning |
|---|---|
| Principal | Identity under which reads, calculations and commands execute |
| Object roots | Ordered sources used to resolve named objects |
| Source roots | Ordered sources used to resolve Lingo modules and class definitions |
| Write workspace | Explicit destination for durable edits |
| Data snapshot | Revision or coherent snapshot against which values are evaluated |
| Source manifest | Exact resolved module and class versions |
| Scenario stack | Temporary overrides active for this view or evaluation |
| Execution policy | Effective capabilities, resource limits and command authority |

The first matching root wins. DSCO should show the selected origin and allow inspection of shadowed candidates. Resolving two objects with the same name must not silently merge their fields. An existing run retains its pinned context; changing the session selection does not change that run's meaning or silently reuse incompatible cached values.

Nested workspace names must not imply source inheritance. The primer explicitly says a subdatabase does not inherit scripts merely because its name includes the parent's name. Inheritance belongs in the configured roots. [IMG_2130.md:11](/Users/arthurcolle/Downloads/idaes/IMG_2130.md:11).

## A workspace protects the shared world

The historical user-database workflow reads shared objects through a parent database. Editing a parent object creates a copy in the user database; the parent remains unchanged. [IMG_2172.md:41–43](/Users/arthurcolle/Downloads/idaes/IMG_2172.md:41), [IMG_2173.md:1](/Users/arthurcolle/Downloads/idaes/IMG_2173.md:1).

For GraphSub, the corresponding target is a workspace overlay with explicit base revisions. The operator needs both an effective view, combining workspace and fallback objects, and a workspace-only view showing exactly what has changed. Committing or promoting those changes requires an explicit destination and conflict handling. A shared name is insufficient proof that a local version can overwrite the current shared version.

The notes' progression from user development to development and production databases informs source promotion, but its historical copy/CVS commands are not the new contract. DSCO promotion should identify the exact source manifest, relevant tests, object/schema changes and destination revision. [IMG_2166.md:7–9](/Users/arthurcolle/Downloads/idaes/IMG_2166.md:7), [IMG_2174.md:9–35](/Users/arthurcolle/Downloads/idaes/IMG_2174.md:9).

## Inspect values, not merely object relationships

The notes emphasize that calculation dependencies exist between particular value nodes, not whole objects. Requesting one value does not require evaluating every field of its object. [IMG_2154.md:5–15](/Users/arthurcolle/Downloads/idaes/IMG_2154.md:5).

The object inspector must therefore lead into a value inspector. The historical interface exposes a value's dependencies, cached values, users and ancestors. Its terminal-node view adds value type, database context, calculation implementation, arguments, data type, value, dependency edges and override information. It supports navigating those edges, evaluating a selected value and explicitly invalidating a node. [IMG_2161.md:1–13](/Users/arthurcolle/Downloads/idaes/IMG_2161.md:1), [IMG_2290.txt:9–43](/Users/arthurcolle/Downloads/idaes/IMG_2290.txt:9).

DSCO's corresponding view should display:

- Stable object ID, field or calculation name, normalized arguments and evaluation-context ID.
- Current result or failure, result type, validity, freshness and cache state.
- Actual dependencies and dependents from the relevant evaluation.
- Definition/source revision and a direct link to the calculation.
- Active override, its scope and the value or calculation it masks.
- Evaluation timing, resource usage and the event that last invalidated the value.

Inspecting metadata must not silently evaluate an expensive value. “Show cached result,” “evaluate,” “refresh observations” and “invalidate local cache” are separate operations. Invalidating a local cache entry does not edit the durable object.

Origin belongs in every object view. The primer exposes class identity, creation/update information, reference count, loaded database and transaction number. DSCO should preserve that provenance with stable revisions rather than presenting raw memory addresses as identities. [IMG_2160.md:35–41](/Users/arthurcolle/Downloads/idaes/IMG_2160.md:35), [IMG_2161.md:1](/Users/arthurcolle/Downloads/idaes/IMG_2161.md:1).

## Editing, committing and scenarios are different operations

The clearest semantic instruction is the distinction between local mutation, persistence and a diddle. `SetValue` changes stored data locally without saving it. `UpdateSecurity` persists it. A diddle retains the original beneath a temporary override and can target a stored or calculated value. [IMG_2257.txt:20–26](/Users/arthurcolle/Downloads/idaes/IMG_2257.txt:20), [IMG_2155.md:3–5](/Users/arthurcolle/Downloads/idaes/IMG_2155.md:3).

DSCO should make three operations visibly distinct:

| Operation | Meaning | Required visible state |
|---|---|---|
| Edit draft | Change proposed stored fields in the workspace | Base revision, changed fields, validation and unsaved state |
| Commit changes | Persist an explicit changeset | Destination, preconditions, conflict outcome and commit receipt |
| Override in scenario | Evaluate a temporary hypothetical value | Scope, override origin, baseline and restoration behavior |

A generic “set” or “save” action must not blur these meanings. A scenario result can inform a proposed changeset, but committing the result must not silently persist all overrides. The historical SecView UI itself offers separate edit and diddle operations. [IMG_2162.md:7–43](/Users/arthurcolle/Downloads/idaes/IMG_2162.md:7).

Object forms should be generated from class metadata: editable stored fields, computed fields, required inputs, validation and available operations. The notes describe class-specific creation dialogs and noneditable fields. [IMG_2161.md:17–25](/Users/arthurcolle/Downloads/idaes/IMG_2161.md:17), [IMG_2162.md:13–19](/Users/arthurcolle/Downloads/idaes/IMG_2162.md:13).

## Buffers and output are first-class operator material

The editor distinguishes Object, File, Scratch and Output buffers. Modified buffers are marked, and an object buffer can be saved into the database. The scratchpad evaluates short scripts and allows output to become a file or an editor buffer. [IMG_2167.md:5–7](/Users/arthurcolle/Downloads/idaes/IMG_2167.md:5), [IMG_2168.md:11–19](/Users/arthurcolle/Downloads/idaes/IMG_2168.md:11), [IMG_2163.md:13–39](/Users/arthurcolle/Downloads/idaes/IMG_2163.md:13).

DSCO should retain those distinctions. A source-object buffer targets a GraphSub source revision; a file buffer targets a local path; scratch is explicitly unsaved; output records an evaluation. A buffer's kind, target, dirty state and source origin remain visible. Evaluation of unsaved text records its content hash so that a result is traceable even before the text is saved.

Search should span objects, classes, value definitions, source, current buffers and transaction history. Results should carry navigable IDs and source locations. This follows the notes' combined search criteria and utilities that turn search output into a buffer from which the operator jumps to a script. [IMG_2176.md:13–45](/Users/arthurcolle/Downloads/idaes/IMG_2176.md:13), [IMG_2177.md:21–37](/Users/arthurcolle/Downloads/idaes/IMG_2177.md:21).

## Tracing explains how a value became what it is

The debugger exposes local/global variables, the call stack, associated source, output and the last error. Trace functions expose reads, writes and diddles. Event breakpoints can target an object, value, arguments and message, including the operation that installs an override. [IMG_2284.txt:11–19](/Users/arthurcolle/Downloads/idaes/IMG_2284.txt:11), [IMG_2287.txt:19–31](/Users/arthurcolle/Downloads/idaes/IMG_2287.txt:19), [IMG_2288.txt:10–33](/Users/arthurcolle/Downloads/idaes/IMG_2288.txt:10).

DSCO should make “why this value?”, “what invalidated it?”, “who changed this input?” and “which source produced it?” resolvable questions. Errors should link to the exact source and evaluation, not only a log line. Command effects require receipts separate from calculation traces.

Continuous live watches are a DSCO extension: these pages do not establish a reliable subscription protocol. Define a watch as a subscription to changes affecting a selected value, with explicit policy for recomputation. Invalidation notification does not itself authorize external observation refreshes or command execution. Reconnection, missed-event recovery, coalescing, backpressure and resource budgets belong in the new contract.

Graph statistics are useful but have limits. The notes support object, branch and whole-world statistics and comparison of saved outputs; they also warn that memory measurements can undercount or overcount. DSCO should label estimates and distinguish cached data, dependency metadata and total process memory. [IMG_2290.txt:51–55](/Users/arthurcolle/Downloads/idaes/IMG_2290.txt:51), [IMG_2292.txt:29–31](/Users/arthurcolle/Downloads/idaes/IMG_2292.txt:29).

## Boundaries of the inheritance

The durable lesson is one inspectable environment over shared objects, programmable values and explicit operational context. The historical access rules, mutable global scope, force-update switches, database synchronization and implementation details are not inherited guarantees. GraphSub permissions, snapshot consistency, concurrency control and durable command recovery require their own defined contracts. `IMG_2283.txt` contains transcription quality claims rather than substantive debugger text and is not used as evidence here.
