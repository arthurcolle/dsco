# Execution Profiling

`dsco` has an isolated object-code instrumentation lane. It does not change the
default release binary.

```sh
make profile-instrumented
python3 scripts/dsco_profile.py -- ./dsco-instrumented --version
python3 scripts/dsco_profile.py --sample-rate 100 -- ./dsco-instrumented --profile lite --tool-exec cwd '{}'
```

The instrumented binary uses LLVM SanitizerCoverage edge hooks and function
entry/exit hooks. The wrapper writes run artifacts under `build/profiles/`:

- `summary.json`
- `lines.tsv`
- `edges.symbolized.tsv`
- `functions.symbolized.tsv`
- `stacks.symbolized.folded`
- `flamegraph.svg`

Useful runtime controls:

- `DSCO_INSTRUMENT=0` disables hooks in an instrumented binary.
- `DSCO_INSTRUMENT_DIR=<dir>` writes process artifacts to a fixed directory.
- `DSCO_INSTRUMENT_STACK_SAMPLE_RATE=<n>` records one folded stack per `n` edge hits.
- `DSCO_INSTRUMENT_STREAM=1` writes raw per-event JSONL and is intentionally expensive.
