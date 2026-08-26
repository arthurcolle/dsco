-- Chimera Router feedback store, schema v3.
-- Intentionally stores task/response hashes and structured outcomes, not raw
-- prompts or model responses. Raw material belongs in a separately governed
-- store and may be joined by hash only.

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS router_schema (
    version       INTEGER PRIMARY KEY,
    installed_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);
INSERT OR IGNORE INTO router_schema(version) VALUES (1);
INSERT OR IGNORE INTO router_schema(version) VALUES (2);
INSERT OR IGNORE INTO router_schema(version) VALUES (3);

CREATE TABLE IF NOT EXISTS catalog_snapshots (
    snapshot_id     TEXT PRIMARY KEY,
    captured_at     TEXT NOT NULL,
    source_url      TEXT NOT NULL,
    content_sha256  TEXT NOT NULL UNIQUE,
    schema_version  INTEGER NOT NULL,
    model_count     INTEGER NOT NULL CHECK(model_count >= 0)
);

CREATE TABLE IF NOT EXISTS route_decisions (
    request_id          TEXT PRIMARY KEY,
    decided_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
    request_hash        TEXT NOT NULL,
    tenant_hash         TEXT,
    policy_id           TEXT NOT NULL,
    checkpoint_id       TEXT NOT NULL,
    catalog_snapshot_id TEXT,
    explicit_override   TEXT,
    constraints_json    TEXT NOT NULL DEFAULT '{}',
    chosen_model        TEXT NOT NULL,
    chosen_provider     TEXT,
    chosen_score        REAL NOT NULL,
    confidence          REAL,
    logging_propensity  REAL CHECK(logging_propensity IS NULL OR
                                   (logging_propensity > 0 AND logging_propensity <= 1)),
    fallback_chain_json TEXT NOT NULL DEFAULT '[]',
    FOREIGN KEY(catalog_snapshot_id) REFERENCES catalog_snapshots(snapshot_id)
);

-- Fixed-width request features make delayed outcomes trainable without
-- retaining prompt text.  They are data-minimizing, not anonymous, and must be
-- governed as sensitive telemetry.  A separate table keeps schema-v1 databases
-- forward-migratable without rewriting route_decisions.
CREATE TABLE IF NOT EXISTS route_request_features (
    request_id            TEXT PRIMARY KEY,
    task_feature_version  TEXT NOT NULL,
    task_dim              INTEGER NOT NULL CHECK(task_dim > 0),
    encoding              TEXT NOT NULL CHECK(encoding = 'float32-le'),
    feature_sha256        TEXT NOT NULL,
    task_features         BLOB NOT NULL,
    FOREIGN KEY(request_id) REFERENCES route_decisions(request_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS candidate_exposures (
    request_id          TEXT NOT NULL,
    rank                INTEGER NOT NULL CHECK(rank >= 0),
    model_id            TEXT NOT NULL,
    provider            TEXT,
    feasible            INTEGER NOT NULL CHECK(feasible IN (0,1)),
    rejection_reason    TEXT,
    predicted_quality   REAL,
    predicted_failure   REAL,
    predicted_latency_ms REAL,
    predicted_cost_usd  REAL,
    score               REAL,
    logging_propensity  REAL CHECK(logging_propensity IS NULL OR
                                   (logging_propensity >= 0 AND logging_propensity <= 1)),
    chosen              INTEGER NOT NULL CHECK(chosen IN (0,1)),
    PRIMARY KEY(request_id, rank),
    FOREIGN KEY(request_id) REFERENCES route_decisions(request_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS route_outcomes (
    request_id            TEXT PRIMARY KEY,
    completed_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
    intended_model        TEXT NOT NULL,
    actual_model          TEXT,
    actual_provider       TEXT,
    generation_id         TEXT,
    http_success          INTEGER CHECK(http_success IS NULL OR http_success IN (0,1)),
    task_success          INTEGER CHECK(task_success IS NULL OR task_success IN (0,1)),
    provider_failure      INTEGER CHECK(provider_failure IS NULL OR provider_failure IN (0,1)),
    tool_valid            INTEGER CHECK(tool_valid IS NULL OR tool_valid IN (0,1)),
    schema_valid          INTEGER CHECK(schema_valid IS NULL OR schema_valid IN (0,1)),
    refusal               INTEGER CHECK(refusal IS NULL OR refusal IN (0,1)),
    user_retry            INTEGER CHECK(user_retry IS NULL OR user_retry IN (0,1)),
    accepted_or_used      INTEGER CHECK(accepted_or_used IS NULL OR accepted_or_used IN (0,1)),
    prompt_tokens         INTEGER,
    completion_tokens     INTEGER,
    reasoning_tokens      INTEGER,
    cache_read_tokens     INTEGER,
    cache_write_tokens    INTEGER,
    cost_usd              REAL,
    ttft_ms               REAL,
    e2e_ms                REAL,
    finish_reason         TEXT,
    label_source          TEXT NOT NULL,
    label_confidence      REAL NOT NULL CHECK(label_confidence >= 0 AND label_confidence <= 1),
    censored              INTEGER NOT NULL DEFAULT 0 CHECK(censored IN (0,1)),
    FOREIGN KEY(request_id) REFERENCES route_decisions(request_id) ON DELETE CASCADE
);

-- Append-only source of truth for delayed labels.  route_outcomes above is a
-- compatibility projection containing only the current revision.  Consumers
-- that need a reproducible dataset must pin a revision_id high-water and read
-- the latest revision for each request at or below that high-water.
CREATE TABLE IF NOT EXISTS route_outcome_revisions (
    revision_id           INTEGER PRIMARY KEY AUTOINCREMENT,
    request_id            TEXT NOT NULL,
    revision_number       INTEGER NOT NULL CHECK(revision_number > 0),
    recorded_at           TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
    supersedes_revision_id INTEGER,
    completed_at          TEXT NOT NULL,
    intended_model        TEXT NOT NULL,
    actual_model          TEXT,
    actual_provider       TEXT,
    generation_id         TEXT,
    http_success          INTEGER CHECK(http_success IS NULL OR http_success IN (0,1)),
    task_success          INTEGER CHECK(task_success IS NULL OR task_success IN (0,1)),
    provider_failure      INTEGER CHECK(provider_failure IS NULL OR provider_failure IN (0,1)),
    tool_valid            INTEGER CHECK(tool_valid IS NULL OR tool_valid IN (0,1)),
    schema_valid          INTEGER CHECK(schema_valid IS NULL OR schema_valid IN (0,1)),
    refusal               INTEGER CHECK(refusal IS NULL OR refusal IN (0,1)),
    user_retry            INTEGER CHECK(user_retry IS NULL OR user_retry IN (0,1)),
    accepted_or_used      INTEGER CHECK(accepted_or_used IS NULL OR accepted_or_used IN (0,1)),
    prompt_tokens         INTEGER,
    completion_tokens     INTEGER,
    reasoning_tokens      INTEGER,
    cache_read_tokens     INTEGER,
    cache_write_tokens    INTEGER,
    cost_usd              REAL,
    ttft_ms               REAL,
    e2e_ms                REAL,
    finish_reason         TEXT,
    label_source          TEXT NOT NULL,
    label_confidence      REAL NOT NULL CHECK(label_confidence >= 0 AND label_confidence <= 1),
    censored              INTEGER NOT NULL DEFAULT 0 CHECK(censored IN (0,1)),
    UNIQUE(request_id, revision_number),
    FOREIGN KEY(request_id) REFERENCES route_decisions(request_id) ON DELETE CASCADE,
    FOREIGN KEY(supersedes_revision_id) REFERENCES route_outcome_revisions(revision_id)
);

-- Non-destructive v1/v2 migration.  A legacy current-outcome row becomes the
-- first immutable revision.  Ordering makes assigned revision IDs stable for
-- the same legacy database contents, and the unique key makes this idempotent.
INSERT OR IGNORE INTO route_outcome_revisions(
    request_id, revision_number, recorded_at, supersedes_revision_id,
    completed_at, intended_model, actual_model, actual_provider,
    generation_id, http_success, task_success, provider_failure, tool_valid,
    schema_valid, refusal, user_retry, accepted_or_used, prompt_tokens,
    completion_tokens, reasoning_tokens, cache_read_tokens,
    cache_write_tokens, cost_usd, ttft_ms, e2e_ms, finish_reason,
    label_source, label_confidence, censored
)
SELECT
    request_id, 1, completed_at, NULL, completed_at, intended_model,
    actual_model, actual_provider, generation_id, http_success, task_success,
    provider_failure, tool_valid, schema_valid, refusal, user_retry,
    accepted_or_used, prompt_tokens, completion_tokens, reasoning_tokens,
    cache_read_tokens, cache_write_tokens, cost_usd, ttft_ms, e2e_ms,
    finish_reason, label_source, label_confidence, censored
FROM route_outcomes
ORDER BY request_id;

-- Enforce append-only behavior even for callers that bypass FeedbackStore.
CREATE TRIGGER IF NOT EXISTS route_outcome_revisions_no_update
BEFORE UPDATE ON route_outcome_revisions
BEGIN
    SELECT RAISE(ABORT, 'route_outcome_revisions is append-only');
END;

CREATE TRIGGER IF NOT EXISTS route_outcome_revisions_no_delete
BEFORE DELETE ON route_outcome_revisions
BEGIN
    SELECT RAISE(ABORT, 'route_outcome_revisions is append-only');
END;

CREATE TABLE IF NOT EXISTS pairwise_comparisons (
    comparison_id     TEXT PRIMARY KEY,
    request_hash      TEXT NOT NULL,
    catalog_snapshot_id TEXT,
    model_a           TEXT NOT NULL,
    model_b           TEXT NOT NULL,
    winner            TEXT CHECK(winner IN ('a','b','tie')),
    score_a           REAL,
    score_b           REAL,
    source            TEXT NOT NULL,
    confidence        REAL NOT NULL CHECK(confidence >= 0 AND confidence <= 1),
    created_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
    FOREIGN KEY(catalog_snapshot_id) REFERENCES catalog_snapshots(snapshot_id)
);

CREATE INDEX IF NOT EXISTS idx_route_decisions_time
    ON route_decisions(decided_at);
CREATE INDEX IF NOT EXISTS idx_route_decisions_model
    ON route_decisions(chosen_model);
CREATE INDEX IF NOT EXISTS idx_candidate_exposures_model
    ON candidate_exposures(model_id, feasible, chosen);
CREATE INDEX IF NOT EXISTS idx_route_outcomes_model
    ON route_outcomes(actual_model, completed_at);
CREATE INDEX IF NOT EXISTS idx_route_outcome_revisions_request
    ON route_outcome_revisions(request_id, revision_id);
CREATE INDEX IF NOT EXISTS idx_route_outcome_revisions_model
    ON route_outcome_revisions(actual_model, completed_at, revision_id);
CREATE INDEX IF NOT EXISTS idx_pairwise_request
    ON pairwise_comparisons(request_hash);
