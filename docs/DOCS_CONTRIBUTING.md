# Docs Contributing Guide

## Goal

Keep documentation synchronized with behavior, not intent.

## Required Updates Per Change Type

- Tool registry changes (`tools.c`) -> regenerate [TOOL_CATALOG.md](TOOL_CATALOG.md)
- Header declaration changes (`*.h`) -> regenerate [API_REFERENCE.md](API_REFERENCE.md)
- External app-directory/cache changes -> regenerate [EXTERNAL_TOOL_CATALOG.md](EXTERNAL_TOOL_CATALOG.md)
- Tracked file additions/removals/moves -> regenerate [REPO_COVERAGE.md](REPO_COVERAGE.md)
- Runtime flow changes (`main.c`, `agent.c`, `llm.c`, `provider.c`, `swarm.c`, `ipc.c`) -> update [ARCHITECTURE.md](ARCHITECTURE.md)
- Env/config/storage changes (`setup.c`, `baseline.c`, provider keys, paths) -> update [OPERATIONS.md](OPERATIONS.md)
- New supported scripts/web assets -> update [INDEX.md](INDEX.md) and the relevant runbook or reference doc
- User-facing command changes -> update `README.md` and operations command list

## Generation Commands

```bash
./scripts/gen_api_reference.sh
./scripts/gen_tool_catalog.sh
python3 scripts/gen_external_tool_catalog.py --root .
python3 scripts/gen_repo_coverage.py --root .
```

## Validation Commands

```bash
make docs-check
```

## Style Rules

- Use present tense and concrete language.
- Prefer short sections with explicit headings.
- Include runnable command examples.
- Avoid stale roadmap wording in reference docs.
- Keep diagrams aligned with current call flow.

## PR Checklist

- [ ] Docs regenerated where applicable
- [ ] Architecture and operations docs updated for behavior changes
- [ ] Links resolve (internal docs links valid)
- [ ] Changelog updated in `CHANGELOG.md`

## CI Enforcement

`docs` workflow should fail if:

- generated docs drift
- generated repo/tool coverage drift
- markdown lint fails
- link check fails
