# Kitty graphics lab

`dsco-kitty-lab` is a deterministic native-pixel gallery for iterating on
agent-facing surfaces without a provider, tool call, or network connection.
It renders live operations, an agent topology, resource meters, a sparkline,
command palette, permission card, and turn trace.

```sh
make dsco-kitty-lab
./dsco-kitty-lab --ppm /tmp/dsco-kitty-lab.ppm
./dsco-kitty-lab --animate
./dsco-kitty-lab --view plan --ppm /tmp/dsco-plan.ppm
./dsco-kitty-lab --view actions --ppm /tmp/dsco-actions.ppm
```

The `--animate` command emits a Kitty RGB animation with terminal-owned frame timing.
The PPM path is the headless artifact used by `make test_kitty_lab`; live
`plan_t` rendering is covered by `make test_pixel_plan`.

The `plan` view makes goal → step → action decomposition explicit and pairs it
with a ready frontier, dependency graph, critical-path meter, and next-action
decision log. The `actions` view focuses on action-level dependencies, policy
gates, ready work, and a three-lane execution timeline. Both views use the same
animated native-pixel transport as the overview.

The live renderer can use the action view for real `plan_t` / `step_t` / `atom_t`
state without changing the caller:

```sh
DSCO_PLAN_VIEW=actions ./dsco --structured-plan-tree "ship a safe release"
```

The default remains the hierarchy tree. `pixel_tui_render_plan_view()` and
`pixel_tui_write_plan_view_ppm()` expose the explicit C API for embedding or
snapshot tests.

The transport uses a conservative terminal hint by default. Use
`DSCO_KITTY_GRAPHICS=force` when running through a multiplexer that hides the
terminal identity, or `DSCO_KITTY_GRAPHICS=off` to keep the pixel path dark.

The transport shared by the lab, banner, and pixel workspace lives in
`include/kitty_graphics.h` / `src/kitty_graphics.c`. It owns zlib compression,
base64 chunking, terminal hints, and the official Kitty query sequence. The
workspace keeps semantic state in the existing pixel/native UI layers; the lab
is intentionally a fixture, not a second agent state machine.
