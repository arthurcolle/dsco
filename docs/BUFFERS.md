# Persistent buffers and views

Buffers keep text independently of the windows displaying it. Scratch buffers, imported files and logs live in a private workspace store and survive CLI/MCP restarts. Views use owned Kitty panes, tabs or desktop windows; several views can display the same buffer.

```sh
dsco buffer new notes
dsco buffer append notes 'First thought'
dsco buffer open notes
```

Inside native DSCO, use `/buffers` to find a name and `/buffer edit notes` to open its inline editor. Click the text (or press Ctrl+G), type, press Ctrl+S to save, and Escape to return to chat. AI-managed buffers use the same canonical content and revision checks: a concurrent change rejects a stale save and retains your edits. Native editing supports complete UTF-8 buffers up to 8,191 bytes; it never saves a truncated preview.

`/buffer new notes` creates a buffer. `/buffer open notes` opens vi in a separate terminal view: press `i` to type, Escape then `:w` Enter to save, or `:wq` Enter to save and exit. These commands execute immediately at the session's current trust tier. A saved edit changes the buffer revision. Closing a vi view can discard edits not saved with `:w`.

Open a second live view, then change its layout:

```sh
dsco buffer open '{"name":"notes","mode":"follow","location":"hsplit"}'
dsco buffer views
dsco buffer layout '{"surface_id":"<returned surface_id>","layout":"tall"}'
dsco buffer resize '{"surface_id":"<returned surface_id>","axis":"horizontal","increment":4}'
dsco buffer detach '{"surface_id":"<returned surface_id>"}'
```

`open` defaults to editing scratch/file buffers and following logs. `view NAME` opens less for browsing. Follow mode uses less with filename following, so an atomic buffer update remains visible. Ctrl+C pauses following; `F` resumes it, and `q` exits. New views preserve the current pane's focus; use `"focus":true` or `buffer focus` to switch. The first view starts a separate owned Kitty workspace. `"visible":false` starts its window minimized.

`open` reuses a matching live buffer/mode view. Set `"new_view":true` to request another. Choose `"type":"tab"` or `"type":"os-window"` to create a tab or desktop window. When several views match, operations require the exact `surface_id`. `close-view` closes only that view. `close NAME` archives the buffer and retains content; `reopen NAME` enables further buffer mutations. An already open editor is an independent process and can still save into an archived buffer's file.

File buffers import a source into their own content file:

```sh
dsco buffer create '{"name":"draft","kind":"file","source_path":"/absolute/path/draft.txt"}'
dsco buffer open draft
dsco buffer save draft
```

The source changes only on explicit `save`. A source changed since import/last save causes `source_conflict`; inspect and reconcile instead of blindly overwriting it. `save` with `path` exports to a different location. An existing new target needs its current `expected_source_revision`.

## Agent contracts

Tools `buffer` and `buffer_view` are available in both the `buffers` and `terminal` MCP toolsets:

```sh
dsco mcp serve --toolsets buffers --tier trusted
```

All dispatch, including nested view-to-buffer/surface operations, goes through `tools_execute_for_tier()`. No executable, shell command or editor configuration is accepted by `buffer_view`. vi starts with initialization, modelines, swap and history disabled; less disables shell escapes, preprocessors and history. The operator can still use vi's interactive commands in the owned editor.

| Tool | Actions | Effects |
| --- | --- | --- |
| `buffer` | create, list, inspect, read, write, append, rename, fork, close, reopen, save | Filesystem only; no process launches |
| `buffer_view` | open, list, focus, resize, layout, detach, close | Owned Kitty view control; open starts editor/pager processes |

Select a buffer using its UUID `buffer_id` or unique workspace `name`. `write` requires `expected_revision`, a SHA256 of the actual file bytes. External editor saves therefore invalidate an agent's stale write. Append accepts an optional revision. Create/write/append/fork accept `request_id` and persist the payload fingerprint and receipt; changed payloads cannot reuse that identity. Pending writes fail closed rather than being replayed speculatively.

View `request_id` identifies one launch for the same buffer and mode. Retrying reconciles that view rather than reapplying layout options. A closed, pending or unobserved view returns `verified:false`; it is never silently recreated by that retry. Use a fresh identity for an explicit new launch. Without a supplied identity, ordinary open reuses a live matching view.

Read returns `text`, lossless `base64`, byte offsets and `truncated`; pagination preserves UTF-8 boundaries. Inspect/read/mutation receipts include buffer metadata and current revision. Buffers are limited to 1 MiB, read chunks to 32 KiB, and a workspace to 256 buffers and 4096 durable mutation records. Completed internal records also count; a full store reports `store_full`. CLI/slash requests have a separate 64 KiB input limit. Errors are JSON with `ok:false`, `error` and `detail`; capability-gate denials preserve their original reason. A view's `ok:true` receipt is not proof of a live window: check `verified` and `views[].exists`.

The store defaults to `~/.dsco/buffers/<workspace>` (`main` by default), with a private SQLite index and owned UTF-8 content files. `DSCO_BUFFER_DIR` overrides its root. View metadata uses the existing independent `DSCO_SURFACE_DIR` registry. Observation does not initialize a missing store. Invalid, foreign-owned or nonprivate metadata fails closed. Persisted sensitivity survives restarts; explicit `sensitive:true` or an obvious credential source path requires the secrets capability. No keychain reset, deletion or initialization is part of this system.

## Native compositor boundary

The native compositor and `--tui` are separate renderers. Buffer storage and Kitty views work independently of either. Native `ui_render` now retains one scene through typing, tool activity and resizing; `{"action":"close"}` or `/scene close` dismisses it. Menus temporarily hide that scene. Native phase and tool updates now repaint during active editing, and multi-chunk pixel patches preserve their target frame on Kitty 0.47.4. This fixes scene lifetime and blank composer/menu updates, but native drag/resize controls and independent native panel focus/scrolling remain future work. Dynamic editable subwindows currently use Kitty.

## Contract review and compatibility

This adds two tools and one CLI/slash command family without changing existing surface identities. The `ui_render` default remains render; its new optional close action is additive. Buffer tools are excluded from result caching because metadata and file contents can change externally.

| Adversarial case | Required result | Verification |
| --- | --- | --- |
| Stale revision after editor save | Reject write, retain current content | Store tests and real vi/MCP test |
| Duplicate mutation retry | Apply once; changed payload rejected | Store persistence tests |
| Two views, buffer-only close/focus | Require exact surface ID | View adapter and live MCP tests |
| Closed launch retry | Retained receipt, verified false, no new process | View adapter and live MCP tests |
| Foreign surface or buffer mismatch | Reject before mutation | View adapter tests |
| Unicode, control text, oversized/binary file | Valid bounded UTF-8 or explicit rejection | Store and formatter tests |
| Source modified outside DSCO | Save conflict, no source overwrite | Store and live MCP tests |
| Run/write/secrets capability disabled | Denial preserved through public entrypoint | Live MCP tests |
| Native scene followed by input/resize/tool | Retain/reflow scene; explicit close clears it | Native lifecycle fixture |

Run `make test_buffer_cli test_buffer_store test_buffer_view test_buffer_ui`, `make test_buffer_views`, and `make test-gate-claims`. Live view tests start and close only their own Kitty fixtures. They preserve the real HOME and keychains.

## Copyable macOS drafts

When the human asks for a draft to copy or edit, use TextEdit rather than a native pixel snapshot:

```sh
dsco buffer open '{"name":"draft","mode":"textedit"}'
```

This opt-in route opens the complete canonical buffer file, preserves an already-open unsaved document, and verifies its path through TextEdit. Cmd-A selects all, Cmd-C copies, Cmd-V pastes, and Cmd-S saves. It does not replace clipboard contents automatically. Only action=open, mode=textedit, workspace and buffer_id/name are accepted; manage the application window directly, not through Kitty surface controls. No unsolicited desktop surface is authorized by this feature. Both the buffer read and launch go through the current capability gate; Apple Events permission may be required. A verification failure is not permission to duplicate or overwrite the document. Native pixel panels still do not provide standard text selection/copy.
