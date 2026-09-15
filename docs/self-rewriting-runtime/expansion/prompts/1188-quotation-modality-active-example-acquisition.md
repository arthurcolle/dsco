# 1188 — Quotation and modality interpreter: Active example acquisition

Implement the active example acquisition feature for the Quotation and modality interpreter RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a root RLM to preserve who asserted what, with negation, uncertainty, quotation, and endorsement intact.

External DOCUMENT_SPANS and UTTERANCE_GRAPH store exact bytes, speaker candidates, nesting, modality, and local discourse context. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions discourse, recursively asks children for attribution alternatives, and verifies buffered quotations against original spans. Role output: Return ATTRIBUTED_PROPOSITIONS with modal operators, endorsement relations, quote handles, and unresolved ambiguity. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to select the next development example or oracle query using uncertainty, task importance, and expected decision benefit. Record selection probabilities and the population from which examples were chosen. Keep a fixed independent evaluation distribution for measuring actual improvement.

Training includes controlled nested quotations, conditional statements, denials, satire labels, and expert-adjudicated ambiguous passages. Template-generated examples provide exact labels; independent blinded reviewers adjudicate natural examples with disagreement preserved. Hold out discourse templates, speaker arrangements, and source genres. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A paper quoting an unsafe recommendation to refute it must not endorse that recommendation. A policy that repeatedly samples easy successes must lose coverage credit. A highly uncertain but irrelevant example must not outrank a decisive missing case merely because it produces a larger confidence change.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/session_memory.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` (`KnowledgeExtractionSignature`, `ChunkedDocumentAnalyzer._aggregate_results`). Donor baseline: Extraction includes utterances and beliefs, but aggregation discards utterances and keeps only coarse chunk positions. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
