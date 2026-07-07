# DSCO Performance Rollout Baseline

Generated: `2026-07-02T21:34:32.936912+00:00`

## Host

- Host: `chrysalis`
- Platform: `macOS-26.5.1-arm64-arm-64bit-Mach-O`

## Baseline critical paths

![Baseline latency](baseline_latency.svg)

| Case | Runs | OK | Failures | p50 ms | p95 ms | max ms | median stdout bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| help | 5 | 5 | 0 | 14.97 | 16.63 | 16.63 | 0 |
| models_json | 5 | 5 | 0 | 13.89 | 18.24 | 18.91 | 31800 |
| route_explain_sonnet | 5 | 5 | 0 | 20.04 | 23.60 | 23.73 | 379 |
| tool_cwd | 5 | 5 | 0 | 15.61 | 19.32 | 20.01 | 66 |
| tools_json | 5 | 5 | 0 | 15.02 | 15.58 | 15.72 | 97746 |
| version | 5 | 5 | 0 | 14.49 | 17.04 | 17.32 | 50 |

## Rollout projection

![Rollout projection](rollout_projection.svg)
