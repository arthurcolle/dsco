# Kitty pane controls — visibility is not lifecycle

Profile: `config/kitty/dsco.conf`. Keys below apply only when that profile is
loaded. `super` means Command on macOS; `alt` means Option.

| Shortcut | Action |
|---|---|
| Command–Option–Z | Toggle active-pane-only (`stack`) and the preceding layout. Other panes and their processes remain alive. |
| Command–Option–[ / ] | Previous/next pane, including panes not currently visible in stack layout. |
| Command–Option–1 … 9 | Select a pane by its tab-local position, not by worker ID. |
| Command–Option–G | Grid layout. |
| Command–Option–T | Tall layout: primary pane with companions beside it. |
| Command–Option–S | Split layout. |
| Command–Option–L | Cycle enabled layouts. |
| Command–Option–arrows | Focus a visible neighbouring pane. |
| Command–Option–Shift–arrows | Move a pane. |
| Command–Control–arrows | Resize a pane. |
| Command–Option–0 | Reset pane sizes. |

These bindings operate within the current Kitty tab. They do not bring an
unrelated OS window into the workspace, change a process budget or grant tool
permissions. Selecting an absent numbered pane should not launch anything;
this boundary needs live validation on the installed Kitty version.

## Two separate kinds of window

- A **Kitty pane** is a terminal/process view with native title bars and borders.
  Kitty handles these shortcuts before application input.
- An **inline DSCO swarm card** is drawn inside one terminal. Ctrl+G, then Tab,
  arrows/modifiers, Z and R control those cards. They are not separate Kitty
  panes. Esc returns to the draft. Kitty stack toggling affects the enclosing
  terminal, not individual cards.

## Supported now versus next

The profile has active-pane-only/restore, switching, sizing and layout controls.
It does NOT implement individually hiding arbitrary subsets of native panes,
named saved workspaces, per-worker pinning, or process-aware pane resurrection.
Those require a separately tested view registry; don't represent stack mode as
an individual-pane visibility API.

The next registry should separate:

- view identity from worker/process identity;
- visible, hidden, collapsed, pinned and closed view states;
- running, waiting, failed and completed worker states;
- user-closed views from transient renderer failures;
- requested layout from last verified layout;
- local view refresh from source-data freshness.

Never recreate a deliberately closed view without an explicit operator action.
Never stop a worker merely because its view is hidden. Do not bind bare Q to
worker termination, and do not equate a tail/log viewer's exit with worker exit.

## Validation status

The additional bindings were written and read back. The operator subsequently
requested arbitrary sidechat/review panes. Inspection found that the real
`~/.config/kitty/kitty.conf` included a separate older `dsco.conf`, not the updated
repository profile. A reversible include now connects the real user config to
`/Users/arthurcolle/dsco/dsco-cli/config/kitty/prompt-panes.conf`. The repository
profile also includes that controls file for standalone launches.

Runtime loading, shortcut-conflict checking, focus behaviour, resize/restore
fidelity and interaction testing remain pending: Kitty-control execution was
denied, while filesystem writes were permitted. The installed configuration's
documented reload shortcut is Control–Command–comma. No reload or sidechat
launch was performed by this agent after the denial.

The named screenshot file was confirmed to exist, but the filesystem reader
returned PNG bytes, not a rendered image. No visual conclusion about that
specific screenshot is claimed here.
