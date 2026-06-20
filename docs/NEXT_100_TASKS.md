# Next 100 Weather/Trading Platform Tasks

Created: 2026-04-10

Status legend: `[ ]` proposed, `[~]` in progress, `[x]` completed in this branch.

## Ingest And Data Quality
1. [x] Add source-drift detection for upstream model/weather payload field changes.
2. [x] Persist source-drift events with missing/added fields and sample hashes.
3. [x] Add a source-drift CLI/reporting path for operators.
4. [x] Add station coverage heatmap data for all active weather markets.
5. [x] Add a station coverage heatmap script for daily operations.
6. [x] Add configurable checksum manifests for forecast artifact downloads.
7. [x] Add provider-specific checksum verification policies.
8. [x] Add replay command that reprocesses stored raw payload snapshots.
9. [x] Add payload snapshot retention pruning by provider and age.
10. [x] Add ingest schema fixtures for every supported weather API provider.
11. [x] Add strict schema mode that fails writes on external API shape changes.
12. [x] Add provider health scoring from freshness, latency, and error counters.
13. [x] Add station metadata diff report between current config and DB versions.
14. [x] Add station metadata import/export for manual operations review.
15. [x] Add city/station validation against live NWS station metadata.
16. [x] Add settlement-source URL metadata per station.
17. [x] Add daily observation gap clustering by station and hour.
18. [x] Add backfill dry-run summary grouped by missing hour ranges.
19. [x] Add observation duplicate detection parallel to forecast duplicate checks.
20. [x] Add NWS observation unit validation and out-of-range rejection.

## Forecast Calibration And Fusion
21. [x] Add 90-day rolling EMOS trainer entrypoint.
22. [x] Add season-matched EMOS trainer entrypoint.
23. [x] Add pooled-vs-station shrinkage grid-search helper.
24. [x] Add coefficient stability deployment gate.
25. [x] Add PIT histogram generation from verification rows.
26. [ ] Add CRPS daily aggregation table and updater.
27. [ ] Add Brier score bucket aggregation table and updater.
28. [ ] Add reliability diagram JSON generator per station.
29. [ ] Add sharpness score aggregation per station.
30. [ ] Add calibration drift alert based on reliability degradation.
31. [ ] Add NBM-only benchmark ingestion adapter.
32. [ ] Add persistence benchmark generator.
33. [ ] Add climatology benchmark generator from local climate data.
34. [ ] Add post-settlement miss attribution fields.
35. [ ] Add forecast error clustering by inferred regime.
36. [ ] Add top-miss root-cause tagging CLI.
37. [ ] Add automated recalibration trigger scheduler hook.
38. [ ] Add champion-challenger variant registry.
39. [ ] Add canary variant routing by city/model family.
40. [ ] Add degraded-live-metric rollback trigger helper.

## Trading Execution And Risk
41. [ ] Persist market mid-price snapshots to SQLite or Parquet.
42. [ ] Add 5-second market snapshot scheduler wrapper.
43. [ ] Add depth imbalance time-series aggregation.
44. [ ] Add spread-regime transition detector.
45. [ ] Add liquidity score history table.
46. [ ] Add slippage model calibration from fills.
47. [ ] Add passive-fill probability calibration from order outcomes.
48. [ ] Add queue-position history persistence.
49. [ ] Add volatility/spread execution throttle config file.
50. [ ] Add stale-order watchdog command.
51. [ ] Add trade reason-code taxonomy documentation.
52. [ ] Add EV-at-fill audit table.
53. [ ] Add realized-vs-expected slippage daily report.
54. [ ] Add book-shock pause state persistence.
55. [ ] Add adverse-selection alert thresholds.
56. [ ] Add market halt source adapter.
57. [ ] Add model-signal PnL attribution table.
58. [ ] Add no-trade classifier training fixture format.
59. [ ] Add live trading heartbeat endpoint in the web API.
60. [ ] Add risk compliance JSON export.

## Workflow Reliability And Operations
61. [ ] Persist workflow plans instead of process-local memory only.
62. [ ] Add workflow migration registry for contract versions.
63. [ ] Add activity registration audit log at worker boot.
64. [ ] Add deterministic retry schedule calculator with jitter toggle.
65. [ ] Add non-retryable exception taxonomy file.
66. [ ] Add workflow dead-letter replay audit trail.
67. [ ] Add structured failure-envelope JSON schema.
68. [ ] Add workflow input/output schema registry.
69. [ ] Add worker liveness metrics endpoint.
70. [ ] Add queue lag alert helper.
71. [ ] Add workflow concurrency cap config.
72. [ ] Add high-cost activity rate-limit config.
73. [ ] Add activity timeout policy defaults file.
74. [ ] Add lock-contention-aware DB retry wrapper.
75. [ ] Add workflow replay test fixtures for CI.
76. [ ] Add long-history workflow truncation guard.
77. [ ] Add long-activity heartbeat helper API.
78. [ ] Add graceful cancellation test harness.
79. [ ] Add schedule jitter policy docs.
80. [ ] Add schedule dependency graph validator.

## Web, Reporting, And Governance
81. [ ] Add station profile panel API backing data.
82. [ ] Add historical skill API endpoint per station.
83. [ ] Add variant comparison API endpoint.
84. [ ] Add feature-flag API endpoint for model toggles.
85. [ ] Add per-market alert subscription persistence.
86. [ ] Add anomaly detection API for large model shifts.
87. [ ] Add saved layout preference persistence.
88. [ ] Add screenshot export server metadata bundle.
89. [ ] Add UI telemetry ingestion endpoint.
90. [ ] Add endpoint contract tests for weather exports.
91. [ ] Add websocket protocol fixtures for dashboard pushes.
92. [ ] Add property tests for probability normalization.
93. [ ] Add property tests for calibration monotonicity.
94. [ ] Add concurrent DB write race tests for ingest stores.
95. [ ] Add provider timeout chaos tests.
96. [ ] Add malformed upstream payload chaos tests.
97. [ ] Add historical stream replay test harness.
98. [ ] Add release checklist generated from changed subsystems.
99. [ ] Add data dictionary updates for new ingest tables.
100. [ ] Add KPI dashboard metadata for accuracy, PnL, risk, and reliability.
