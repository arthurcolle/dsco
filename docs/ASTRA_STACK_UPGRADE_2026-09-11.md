# Astra prompt/skill upgrade — September 11, 2026

## Retrieved primary sources

- Eric Provencher's [September 11 post](https://x.com/pvncher/status/2098533620306579925), 22:06 UTC, links his OpenAI guide and invites readers to apply it to their setup. Retrieved direct X HTML, including post body and publication metadata.
- [OpenAI Developers announcement](https://x.com/OpenAIDevs/status/2098480213244117065), retrieved as the quoted post in Eric's page: specific triggers, relevant guidance, define done. This is the source author's announcement, not independent community validation.
- [OpenAI: Rethinking skills and prompts for GPT-6 Astra](https://developers.openai.com/blog/rethinking-skills-and-prompts-for-gpt-6-astra).
- [OpenAI current model guide](https://developers.openai.com/api/docs/guides/latest-model).
- [OpenAI skill authoring](https://developers.openai.com/codex/build-skills).

Saved source copies: `.workspace/astra-stack-20260911/`. Search results surfaced discussion, but no independent community benchmark was verified. Do not treat social enthusiasm as a measured DSCO improvement.

## Applied scope

1. Fix `src/workspace.c` skill-summary extraction: YAML `---` was emitted as the description for frontmatter-based skills. Prefer the top-level description; support common plain, quoted, folded/literal scalars; retain legacy Markdown fallback and bounded output. Read only the first 4096 bytes, not whole skills. This is a small metadata reader, not a general YAML interpreter.
2. Replace one existing shared prompt instruction in `include/config.h`: select by description/task, load only needed references, avoid irrelevant checklists and fixed tool/testing quotas; preserve authority and capability boundaries.
3. Upgrade installed `prompt-engineer/SKILL.md` to v2 with narrow invocation, outcome-based completion, proportional verification, and source-linked model-specific notes. Remove stale universal temperature ranges, old token-price examples, mandatory three-revision workflow, and obsolete legacy model shortcuts. Large references are separated from the root.
4. Add catalog regression fixtures and extend the real-binary provider-wire fixture to check selective skill disclosure.

Not changed: model choice, account, provider credentials, governance/capability gates, evaluator or promotion logic, all 1,000+ installed skills, unrelated source work. No blanket copy of Codex-specific folder/config conventions into DSCO. No new private-chain-of-thought requirement.

## Native compositor readability — recommendation, not installed UI changes

The existing user-approved `DSCO_PIXEL_TUI_DPR=3` override remains untouched. It makes the UI 50% larger than automatic 2x, but it overloads device-density calibration as a zoom setting. It is a useful immediate workaround, not a complete typography design.

| Priority | Improvement | Implementation direction and acceptance |
|---|---|---|
| P0 | Separate readable text size from device density | Keep actual Retina backing scale; add persisted semantic text/zoom scale. Start with 15–17 point body and 1.35–1.5 line spacing, adjustable rather than mandatory. `src/pixel_tui.c` currently fixes body at 11.5 and headings at 12.5/13.5. Preserve input, scroll anchor and pointer hit testing on resize. |
| P0 | Reflow header and controls instead of shrinking labels | Current header/chips have many absolute dimensions in `pixel_tui.c`; derive height and wrapping from font measurements. Keep model, activity and composer legible; move lower-value metrics into an expandable detail row. Test narrow and wide windows at 100/125/150/200% zoom. |
| P1 | Strengthen secondary text contrast | `src/px_theme.c` has distinct `text`/`dim` colors; `pixel_tui.c` further attenuates some dim text. Measure final composited colors, not just theme swatches. Aim for at least 4.5:1 for ordinary text. Never communicate failure solely through color. |
| P1 | Make startup and tool output readable | Collapse ASCII startup telemetry into a compact summary; expand only on request. Give human prose comfortable measure/spacing, preserve monospace and horizontal navigation for code. Label connection status separately from agent activity. |
| P1 | Predictable controls and accessibility | Expose zoom in the existing window; support increase/decrease/reset and persist it. Distinguish font size from whole-window scale. Check OS text selection/accessibility support explicitly—pixel rendering does not imply selectable or screen-reader-readable text. |

Verify proposed UI changes with headless geometry/clip tests and screenshots of this window at the target sizes, then a user-readable live check. No unsolicited panels, additional windows, widgets or font/DPR changes were made during this task.

A bounded native review worker failed before producing a verified review. Its process exited; the coordinator inspected the relevant compositor/font/theme sources directly. No worker finding is presented as independent verification.

## Verification and rollback

Scoped before-images, installed-binary rollback, source snapshots, build/unit/wire logs, and final receipt live in `.workspace/astra-stack-20260911/`. Installation and final verification are recorded there. A fresh launch is needed for compiled runtime changes; the existing session is not restarted. Skill files are saved immediately but active-skill caches may require reselection/new session.

Readability proposals and prompt-efficiency benefits are not measured performance improvements. The proven result is correct metadata disclosure and delivery of the updated instructions, not guaranteed model behavior.
