# Explicit skills and maintained evidence

The `skill` tool operates at an explicit file path. Its body is instructions,
not a capability grant or an executable plugin. It never runs bundled scripts,
silently changes sticky selection, or promotes a learned candidate.

```json
{"action":"invoke","path":".agents/skills/dsco-harness-engineering/SKILL.md"}
```

`inspect` and `invoke` return name, description, exact SHA256, instructions, path,
and `scope="invocation"`. `activated=false` means no persistent session selection
was changed. Returned instructions remain ordinary tool-result content under the
caller's existing model/context policy.

For creation, first prepare a skill directory, then supply `action="create"`,
`expected_sha256="absent"`, and `body`. For updates, inspect the file and supply
`action="update"`, its observed `expected_sha256`, and the complete new `body`.
The new lifecycle accepts at most 32 KiB with single-line `name` and `description`
frontmatter; quoted values are supported. Folded YAML descriptions and legacy
Markdown remain accessible through the existing `/skills show` surface.

Updates retain previous exact bytes in the same directory as
`.SKILL.<sha256>.md`. Both archive and live-file edits reenter the normal governed
editor. A grant permitting only `SKILL.md` can deny its sibling archive, in which
case the live file is unchanged. History creation can precede a subsequent stale
live-file rejection; retaining an observed revision is harmless and is not a
successful update. Existing history must match its claimed digest before reuse.

Restore a revision by invoking/reading its path and updating `SKILL.md` against
the current live digest using the revision's exact body. Restoration retains the
replaced version too. Native/ANSI cards show invocation identity and short digest,
with path, full digest, instructions, and nested edit receipt in expanded view.

## Sticky selection

`/skills use <name>` explicitly saves the observed digest and current provider.
`/skills clear` clears all three selection fields. Saved sessions retain them;
changing file bytes does not silently update the selection. Older saved sessions
without a version/provider pin require explicit reselection before instructions
are loaded. Missing, changed, or different-provider selections withhold local
instructions. Provider mismatch notices disclose neither body nor content hash.

The existing Anthropic request path uses that check. Additional OpenAI-compatible
and Responses hooks are **pending explicit disclosure approval**: automatic
review rejected them twice because the user had not explicitly authorized local
skill contents to be transmitted to those providers. They have not been applied.
The local selection/save/load/clear PTY checks exercise both UI modes with
inference disabled; they do not prove the pending hooks work.

## Checkpoint cadence

Load `.agents/skills/dsco-harness-engineering/SKILL.md` for material harness work.
After each verified implementation checkpoint, write a dated note under
`memory/sessions/` with exact commands/outcomes, changed files, binary and artifact
digests, limits, and next work. Update `memory/priors/harness-engineering.md` with
observations separate from hypotheses, confidence, and invalidation conditions.
Preserve superseded observations. This is a checkpoint workflow, not a timer or
an automatic mechanism for turning guesses into operational defaults.

Tests: `make test_code_edit test_code_edit_mcp test-workspace-skills
test_skill_selection_pty test-gate-claims`. Offline evidence and CLI receipts are
under `build/code-edit-mcp/` and `build/skill-selection-pty.json`.
