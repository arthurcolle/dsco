# Snapshot editing

`code_read` and `code_edit` complement the existing fuzzy `edit_file`, unified
`apply_patch`, AST edits, symbol inspection, and diagnostics. They use exact
original file versions and line anchors, with no fuzzy fallback.

```json
{"path":"src/example.c","start_line":20,"limit":40}
```

Call `code_read`. It returns the whole-file `sha256`, total line count, bounded
`lines` containing exact newline bytes, and anchors such as
`20:0123456789abcdef`. Hashes include CRLF and the absence of a final newline.
`next_line=0` indicates the end of the file. `include_content=true` additionally
returns complete content for files up to 32 KiB.

```json
{"path":"src/example.c","expected_sha256":"<sha256 from code_read>",
 "edits":[
   {"kind":"replace","start":"<anchor>","new_text":"return measured;\n"},
   {"kind":"delete","start":"<anchor>","end":"<anchor>"}
 ],"dry_run":true}
```

Call `code_edit`. All edits address the original snapshot; they may arrive in
any order. `end` defaults to `start`. Insertions use `insert_before` or
`insert_after` at one anchor. Supply exact replacement bytes, including desired
newlines. Reread and rebuild the entire edit set after `stale_snapshot`.
Overlapping edits and ambiguous insertions at the same offset are rejected.

Alternatively, `content` replaces all bytes under the same expected digest.
`expected_sha256="absent"` exclusively creates a new file in an existing parent
directory. New files use mode 0600. Creation never overwrites an existing file.

## Receipts and bounds

Every successful edit returns an explicit `written`, `preview`, or `unchanged`
status, before/after hashes, a bounded unified diff, readback verification, and
directory-sync confirmation. The receipt is prepared before effects. A committed
write whose readback fails returns `verified=false` with an inspection warning;
it does not report a retryable precommit failure. A preview is unverified because
no write occurs. Diff tails carry an explicit truncation marker.

Limits: 4 MiB UTF-8 text, 200,000 lines, 64 edits, 200 lines per read, and an
8,000-byte diff preview. Binary/NUL text, invalid UTF-8, symlinks, hard links,
duplicate JSON fields, unknown fields, and malformed anchors are rejected.

Same-directory temporary files are synced and atomically installed. Cooperating
editors take a directory lock across reread and rename. Arbitrary shell writers
do not participate in that protocol; there is no claim of CAS isolation from
them. macOS copies existing file metadata, including ACLs and extended
attributes. The Linux path preserves ownership/mode and rejects extended
attributes it cannot retain. Do not use these tools for special files.

Both tools enter through the normal tier/capability/path gate. `code_edit` remains
a write-class tool even for previews. Nested skill operations use the same gate.
Native and ANSI inspection preserve original JSON while displaying action,
version, verification, and literal diff details.

## Offline verification

```sh
make test_code_edit test_code_edit_mcp test_presentation_blocks
make test_native_compositor test_harness_receipt_scene test-gate-claims
```

The C fixture runs with address/undefined-behavior sanitizers. The production MCP
fixture covers stale/overlap rejection, exact bytes, metadata, scope denials,
binary/link rejection, and skill history/rollback without provider inference.
