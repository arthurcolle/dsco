-- Chimera Router DuckDB fusion warehouse, schema v2.
--
-- This database is an analytical read model.  SQLite remains the OLTP source
-- of truth for live baseline, Chronicle, and delayed-feedback writes.  The
-- builder snapshots those databases consistently, constructs this file off to
-- the side, validates it, and atomically publishes it for readers.

CREATE TABLE IF NOT EXISTS warehouse_metadata (
    key         VARCHAR PRIMARY KEY,
    value       VARCHAR NOT NULL
);

CREATE TABLE IF NOT EXISTS source_registry (
    source_key          VARCHAR PRIMARY KEY,
    display_name        VARCHAR NOT NULL,
    source_class        VARCHAR NOT NULL,
    adapter             VARCHAR NOT NULL,
    format              VARCHAR NOT NULL,
    grain               VARCHAR NOT NULL,
    stable_ids_json     VARCHAR NOT NULL,
    source_url          VARCHAR,
    license_spdx        VARCHAR,
    license_url         VARCHAR,
    pii_risk            VARCHAR NOT NULL,
    trust_tier          VARCHAR NOT NULL,
    default_enabled     BOOLEAN NOT NULL,
    refresh_cadence     VARCHAR,
    notes               VARCHAR
);

CREATE TABLE IF NOT EXISTS source_snapshot (
    snapshot_id          VARCHAR PRIMARY KEY,
    source_key           VARCHAR NOT NULL,
    content_sha256       VARCHAR NOT NULL,
    fingerprint_mode     VARCHAR NOT NULL,
    captured_at          TIMESTAMPTZ NOT NULL,
    ingested_at          TIMESTAMPTZ NOT NULL,
    byte_size            UBIGINT NOT NULL,
    row_count            UBIGINT NOT NULL,
    high_watermark       VARCHAR,
    schema_version       VARCHAR NOT NULL,
    source_uri           VARCHAR,
    artifact_uri         VARCHAR,
    provenance_json      VARCHAR NOT NULL,
    UNIQUE(source_key, content_sha256)
);

CREATE TABLE IF NOT EXISTS baseline_instance (
    source_snapshot_id   VARCHAR NOT NULL,
    instance_id          VARCHAR NOT NULL,
    parent_instance_id   VARCHAR,
    pid                  BIGINT,
    model_id             VARCHAR,
    mode                 VARCHAR,
    started_at           TIMESTAMPTZ NOT NULL,
    ended_at             TIMESTAMPTZ,
    PRIMARY KEY(source_snapshot_id, instance_id)
);

CREATE TABLE IF NOT EXISTS baseline_event (
    source_snapshot_id   VARCHAR NOT NULL,
    event_id             BIGINT NOT NULL,
    instance_id          VARCHAR NOT NULL,
    event_at             TIMESTAMPTZ NOT NULL,
    event_epoch          DOUBLE,
    category             VARCHAR NOT NULL,
    title                VARCHAR NOT NULL,
    detail_sha256        VARCHAR,
    metadata_sha256      VARCHAR,
    provider_id          VARCHAR,
    model_id             VARCHAR,
    auth_mode            VARCHAR,
    billing_mode         VARCHAR,
    input_tokens         BIGINT,
    output_tokens        BIGINT,
    cache_read_tokens    BIGINT,
    cache_write_tokens   BIGINT,
    estimated_cost_usd   DOUBLE,
    PRIMARY KEY(source_snapshot_id, event_id)
);

CREATE TABLE IF NOT EXISTS baseline_trace_span (
    source_snapshot_id   VARCHAR NOT NULL,
    span_id              VARCHAR NOT NULL,
    trace_id             VARCHAR NOT NULL,
    parent_span_id       VARCHAR,
    name                 VARCHAR NOT NULL,
    started_at           TIMESTAMPTZ NOT NULL,
    ended_at             TIMESTAMPTZ,
    status               VARCHAR,
    metadata_sha256      VARCHAR,
    PRIMARY KEY(source_snapshot_id, span_id)
);

CREATE TABLE IF NOT EXISTS chronicle_session (
    source_snapshot_id   VARCHAR NOT NULL,
    session_id           VARCHAR NOT NULL,
    installation_id      VARCHAR,
    instance_id          VARCHAR,
    provider_id          VARCHAR,
    model_id             VARCHAR,
    mode                 VARCHAR,
    started_at           TIMESTAMPTZ NOT NULL,
    ended_at             TIMESTAMPTZ,
    policy_sha256        VARCHAR,
    PRIMARY KEY(source_snapshot_id, session_id)
);

CREATE TABLE IF NOT EXISTS chronicle_event (
    source_snapshot_id       VARCHAR NOT NULL,
    event_id                 VARCHAR NOT NULL,
    installation_id          VARCHAR,
    session_id               VARCHAR NOT NULL,
    trace_id                 VARCHAR,
    span_id                  VARCHAR,
    parent_span_id           VARCHAR,
    sequence_number          BIGINT,
    event_at                 TIMESTAMPTZ NOT NULL,
    event_type               VARCHAR NOT NULL,
    actor_type               VARCHAR,
    actor_id                 VARCHAR,
    payload_hash             VARCHAR,
    event_hash               VARCHAR,
    sensitivity              VARCHAR,
    sync_state               VARCHAR,
    provider_id              VARCHAR,
    model_id                 VARCHAR,
    tool_name                VARCHAR,
    generation_id_sha256     VARCHAR,
    finish_reason            VARCHAR,
    ok                       BOOLEAN,
    latency_ms               DOUBLE,
    cost_usd                 DOUBLE,
    input_tokens             BIGINT,
    output_tokens            BIGINT,
    cache_read_tokens        BIGINT,
    cache_write_tokens       BIGINT,
    request_blob_sha256      VARCHAR,
    output_blob_sha256       VARCHAR,
    PRIMARY KEY(source_snapshot_id, event_id)
);

CREATE TABLE IF NOT EXISTS chronicle_span (
    source_snapshot_id   VARCHAR NOT NULL,
    span_id              VARCHAR NOT NULL,
    trace_id             VARCHAR NOT NULL,
    parent_span_id       VARCHAR,
    span_type            VARCHAR NOT NULL,
    name                 VARCHAR,
    started_at           TIMESTAMPTZ NOT NULL,
    ended_at             TIMESTAMPTZ,
    status               VARCHAR,
    payload_sha256       VARCHAR,
    PRIMARY KEY(source_snapshot_id, span_id)
);

CREATE TABLE IF NOT EXISTS chronicle_blob (
    source_snapshot_id   VARCHAR NOT NULL,
    sha256               VARCHAR NOT NULL,
    byte_length          UBIGINT NOT NULL,
    content_type         VARCHAR,
    logical_type         VARCHAR,
    codec                VARCHAR,
    encryption           VARCHAR,
    sensitivity          VARCHAR,
    created_at           TIMESTAMPTZ,
    PRIMARY KEY(source_snapshot_id, sha256)
);

CREATE TABLE IF NOT EXISTS chronicle_edge (
    source_snapshot_id   VARCHAR NOT NULL,
    edge_id              VARCHAR NOT NULL,
    from_id              VARCHAR NOT NULL,
    to_id                VARCHAR NOT NULL,
    relation             VARCHAR NOT NULL,
    confidence           DOUBLE,
    metadata_sha256      VARCHAR,
    created_at           TIMESTAMPTZ,
    PRIMARY KEY(source_snapshot_id, edge_id)
);

CREATE TABLE IF NOT EXISTS chronicle_training_example (
    source_snapshot_id   VARCHAR NOT NULL,
    example_id           VARCHAR NOT NULL,
    source_trace_id      VARCHAR,
    task_type            VARCHAR,
    dataset_type         VARCHAR,
    quality_score        DOUBLE,
    consent_state        VARCHAR,
    redaction_state      VARCHAR,
    input_blob_sha256    VARCHAR,
    output_blob_sha256   VARCHAR,
    label_blob_sha256    VARCHAR,
    metadata_sha256      VARCHAR,
    created_at           TIMESTAMPTZ,
    PRIMARY KEY(source_snapshot_id, example_id)
);

CREATE TABLE IF NOT EXISTS runtime_process_metric (
    source_snapshot_id   VARCHAR NOT NULL,
    source_record_sha256 VARCHAR NOT NULL,
    observed_at          TIMESTAMPTZ NOT NULL,
    pid                  BIGINT,
    parent_pid           BIGINT,
    sequence_number      BIGINT,
    event                VARCHAR,
    phase                VARCHAR,
    signal               VARCHAR,
    supervised           VARCHAR,
    cpu_user_ms          BIGINT,
    cpu_system_ms        BIGINT,
    rss_mb               DOUBLE,
    peak_rss_mb          DOUBLE,
    rss_delta_mb         DOUBLE,
    memory_pressure      BIGINT,
    memory_restart       BIGINT,
    thread_count         BIGINT,
    fd_count             BIGINT,
    voluntary_switches   BIGINT,
    involuntary_switches BIGINT,
    minor_faults         BIGINT,
    major_faults         BIGINT,
    uptime_s             DOUBLE,
    PRIMARY KEY(source_snapshot_id, source_record_sha256)
);

CREATE TABLE IF NOT EXISTS child_process_metric (
    source_snapshot_id   VARCHAR NOT NULL,
    source_record_sha256 VARCHAR NOT NULL,
    observed_at          TIMESTAMPTZ NOT NULL,
    child_pid            BIGINT,
    supervisor_pid       BIGINT,
    rss_mb               DOUBLE,
    peak_rss_mb          DOUBLE,
    memory_pressure      BIGINT,
    uptime_s             DOUBLE,
    PRIMARY KEY(source_snapshot_id, source_record_sha256)
);

CREATE TABLE IF NOT EXISTS runtime_incident (
    source_snapshot_id       VARCHAR NOT NULL,
    source_record_sha256     VARCHAR NOT NULL,
    observed_at              TIMESTAMPTZ NOT NULL,
    supervisor_pid           BIGINT,
    child_pid                BIGINT,
    incident_class           VARCHAR,
    signal_name              VARCHAR,
    signal_number            BIGINT,
    exit_code                BIGINT,
    action                   VARCHAR,
    restart_count            BIGINT,
    next_delay_ms            BIGINT,
    uptime_s                 DOUBLE,
    peak_rss_mb              DOUBLE,
    memory_budget_mb         DOUBLE,
    memory_soft_limit_mb     DOUBLE,
    memory_pressure          BIGINT,
    poll_ms                  BIGINT,
    preempted                BOOLEAN,
    tracer_reaped            BOOLEAN,
    resume_after_crash       BOOLEAN,
    memory_restart           BOOLEAN,
    last_heartbeat_pid       BIGINT,
    last_heartbeat_age_s     DOUBLE,
    last_heartbeat_pid_matches_child BOOLEAN,
    crash_log_present        BOOLEAN,
    debugger_backtrace_present BOOLEAN,
    PRIMARY KEY(source_snapshot_id, source_record_sha256)
);

CREATE TABLE IF NOT EXISTS swarm_child_result (
    source_snapshot_id   VARCHAR NOT NULL,
    source_record_sha256 VARCHAR NOT NULL,
    source_file_sha256   VARCHAR NOT NULL,
    swarm_run_key_sha256 VARCHAR NOT NULL,
    completed_at_approx  TIMESTAMPTZ NOT NULL,
    started_at_approx    TIMESTAMPTZ,
    source_file_mtime_ns UBIGINT NOT NULL,
    timestamp_basis      VARCHAR NOT NULL,
    parent_pid           BIGINT NOT NULL,
    child_id             BIGINT NOT NULL,
    id_matches_filename  BOOLEAN NOT NULL,
    task_sha256          VARCHAR NOT NULL,
    task_utf8_bytes      BIGINT NOT NULL,
    task_maybe_truncated BOOLEAN NOT NULL,
    requested_model_id   VARCHAR,
    normalized_model_id  VARCHAR,
    model_basis          VARCHAR NOT NULL,
    status               VARCHAR NOT NULL,
    exit_code            BIGINT,
    process_success      BOOLEAN NOT NULL,
    outcome_kind         VARCHAR NOT NULL,
    duration_ms          BIGINT NOT NULL,
    cost_usd_recorded    DECIMAL(18,6) NOT NULL,
    cost_basis           VARCHAR NOT NULL,
    cost_nonzero         BOOLEAN NOT NULL,
    output_present       BOOLEAN NOT NULL,
    output_sha256        VARCHAR,
    output_utf8_bytes    BIGINT NOT NULL,
    output_maybe_truncated BOOLEAN NOT NULL,
    operational_label_usable BOOLEAN NOT NULL,
    semantic_label_usable BOOLEAN NOT NULL,
    PRIMARY KEY(source_snapshot_id, source_record_sha256),
    UNIQUE(source_snapshot_id, parent_pid, child_id)
);

CREATE TABLE IF NOT EXISTS transcript_turn_metric (
    source_snapshot_id   VARCHAR NOT NULL,
    source_record_sha256 VARCHAR NOT NULL,
    source_file_sha256   VARCHAR NOT NULL,
    source_row_number    BIGINT NOT NULL,
    source_pid           BIGINT,
    observed_at          TIMESTAMPTZ NOT NULL,
    turn_number          BIGINT,
    model_id             VARCHAR,
    provider_id          VARCHAR,
    input_tokens         BIGINT,
    output_tokens        BIGINT,
    cache_read_tokens    BIGINT,
    cache_write_tokens   BIGINT,
    cost_usd             DOUBLE,
    stop_reason          VARCHAR,
    PRIMARY KEY(source_snapshot_id, source_record_sha256)
);

CREATE TABLE IF NOT EXISTS tool_trace_metric (
    source_snapshot_id   VARCHAR NOT NULL,
    source_record_sha256 VARCHAR NOT NULL,
    source_file_sha256   VARCHAR NOT NULL,
    source_row_number    BIGINT NOT NULL,
    trace_session_key    VARCHAR NOT NULL,
    source_pid           BIGINT,
    session_started_at   TIMESTAMPTZ,
    observed_at          TIMESTAMPTZ NOT NULL,
    tool_name            VARCHAR NOT NULL,
    ok                   BOOLEAN,
    latency_ms           DOUBLE,
    token_count          BIGINT,
    PRIMARY KEY(source_snapshot_id, source_record_sha256)
);

CREATE TABLE IF NOT EXISTS run_manifest_snapshot (
    source_snapshot_id   VARCHAR NOT NULL,
    run_id               VARCHAR NOT NULL,
    session_id           VARCHAR,
    installation_id      VARCHAR,
    status               VARCHAR,
    started_at           TIMESTAMPTZ,
    ended_at             TIMESTAMPTZ,
    updated_at           TIMESTAMPTZ,
    schema_version       VARCHAR,
    capture_mode         VARCHAR,
    instance_id          VARCHAR,
    sequence_high_water  BIGINT,
    manifest_sha256      VARCHAR NOT NULL,
    primary_chronicle    BOOLEAN NOT NULL,
    PRIMARY KEY(source_snapshot_id, run_id)
);

CREATE TABLE IF NOT EXISTS run_wal_event (
    source_snapshot_id   VARCHAR NOT NULL,
    source_file_sha256   VARCHAR NOT NULL,
    frame_offset         UBIGINT NOT NULL,
    frame_length         UBIGINT NOT NULL,
    frame_crc32          UBIGINT NOT NULL,
    record_version       BIGINT,
    event_type           VARCHAR NOT NULL,
    run_id               VARCHAR NOT NULL,
    sequence_number      BIGINT NOT NULL,
    event_at             TIMESTAMPTZ NOT NULL,
    payload_sha256       VARCHAR NOT NULL,
    event_schema         VARCHAR,
    event_id             VARCHAR,
    event_name           VARCHAR,
    status               VARCHAR,
    severity             VARCHAR,
    phase                VARCHAR,
    category             VARCHAR,
    principal_subject_sha256 VARCHAR,
    principal_tier       VARCHAR,
    agent_model          VARCHAR,
    agent_provider       VARCHAR,
    agent_topology       VARCHAR,
    agent_worker_id      VARCHAR,
    trace_id             VARCHAR,
    span_id              VARCHAR,
    parent_span_id       VARCHAR,
    cost_usd_delta       DOUBLE,
    cost_usd_total       DOUBLE,
    input_tokens         BIGINT,
    output_tokens        BIGINT,
    cache_read_tokens    BIGINT,
    cache_write_tokens   BIGINT,
    reasoning_tokens     BIGINT,
    tool_name            VARCHAR,
    tool_id              VARCHAR,
    tool_ok              BOOLEAN,
    tool_latency_ms      DOUBLE,
    tool_cached          BOOLEAN,
    tool_args_bytes      BIGINT,
    tool_result_bytes    BIGINT,
    turn_number          BIGINT,
    conversation_count   BIGINT,
    stop_reason          VARCHAR,
    duration_ms          BIGINT,
    journal_records      BIGINT,
    callback_enqueued    BIGINT,
    callback_pending     BIGINT,
    callback_delivered   BIGINT,
    callback_dead_lettered BIGINT,
    PRIMARY KEY(source_snapshot_id, run_id, sequence_number)
);

CREATE TABLE IF NOT EXISTS catalog_model_snapshot (
    source_snapshot_id       VARCHAR NOT NULL,
    captured_at              TIMESTAMPTZ NOT NULL,
    model_id                 VARCHAR NOT NULL,
    canonical_slug           VARCHAR,
    alias_target             VARCHAR,
    provider_id              VARCHAR,
    name                     VARCHAR,
    description              VARCHAR,
    created_at               TIMESTAMPTZ,
    expiration_at            TIMESTAMPTZ,
    knowledge_cutoff         VARCHAR,
    hugging_face_id          VARCHAR,
    context_length           BIGINT,
    max_completion_tokens    BIGINT,
    tokenizer                VARCHAR,
    instruct_type            VARCHAR,
    input_modalities_json    VARCHAR NOT NULL,
    output_modalities_json   VARCHAR NOT NULL,
    supported_parameters_json VARCHAR NOT NULL,
    default_parameters_json  VARCHAR NOT NULL,
    prompt_price_usd_token   DECIMAL(38,18),
    completion_price_usd_token DECIMAL(38,18),
    cache_read_price_usd_token DECIMAL(38,18),
    cache_write_price_usd_token DECIMAL(38,18),
    image_price_usd          DECIMAL(38,18),
    request_price_usd        DECIMAL(38,18),
    is_moderated             BOOLEAN,
    raw_record_sha256        VARCHAR NOT NULL,
    PRIMARY KEY(source_snapshot_id, model_id)
);

CREATE TABLE IF NOT EXISTS model_alias (
    source_snapshot_id   VARCHAR NOT NULL,
    alias_model_id       VARCHAR NOT NULL,
    canonical_model_id   VARCHAR NOT NULL,
    alias_kind           VARCHAR NOT NULL,
    PRIMARY KEY(source_snapshot_id, alias_model_id, canonical_model_id, alias_kind)
);

CREATE TABLE IF NOT EXISTS native_model_offer_snapshot (
    source_snapshot_id       VARCHAR NOT NULL,
    captured_at              TIMESTAMPTZ NOT NULL,
    inference_provider_id    VARCHAR NOT NULL,
    model_id                 VARCHAR NOT NULL,
    normalized_model_id      VARCHAR NOT NULL,
    model_namespace          VARCHAR,
    request_model_id         VARCHAR,
    display_name             VARCHAR NOT NULL,
    api                      VARCHAR NOT NULL,
    context_window           BIGINT,
    max_tokens               BIGINT,
    input_modalities_json    VARCHAR NOT NULL,
    reasoning                BOOLEAN NOT NULL,
    supports_tools_declared  BOOLEAN,
    native_tools_usable      BOOLEAN NOT NULL,
    input_price_usd_million  DECIMAL(38,18),
    output_price_usd_million DECIMAL(38,18),
    cache_read_price_usd_million DECIMAL(38,18),
    cache_write_price_usd_million DECIMAL(38,18),
    pricing_status           VARCHAR NOT NULL,
    premium_multiplier       DOUBLE,
    priority                 DOUBLE,
    thinking_mode            VARCHAR,
    thinking_efforts_json    VARCHAR NOT NULL,
    thinking_config_json     VARCHAR NOT NULL,
    compat_capabilities_json VARCHAR NOT NULL,
    apply_patch_tool_type    VARCHAR,
    omit_max_output_tokens   BOOLEAN,
    prefer_websockets        BOOLEAN,
    context_promotion_target VARCHAR,
    reasoning_mode           VARCHAR,
    use_responses_lite       BOOLEAN,
    remote_compaction_enabled BOOLEAN,
    remote_compaction_v2_streaming BOOLEAN,
    remote_compaction_api    VARCHAR,
    raw_record_sha256        VARCHAR NOT NULL,
    PRIMARY KEY(source_snapshot_id, inference_provider_id, model_id)
);

CREATE TABLE IF NOT EXISTS provider_endpoint_snapshot (
    source_snapshot_id       VARCHAR NOT NULL,
    captured_at              TIMESTAMPTZ NOT NULL,
    model_id                 VARCHAR NOT NULL,
    provider_id              VARCHAR NOT NULL,
    endpoint_tag             VARCHAR,
    quantization             VARCHAR,
    context_length           BIGINT,
    max_prompt_tokens        BIGINT,
    max_completion_tokens    BIGINT,
    prompt_price_usd_token   DECIMAL(38,18),
    completion_price_usd_token DECIMAL(38,18),
    uptime_5m                DOUBLE,
    uptime_30m               DOUBLE,
    uptime_1d                DOUBLE,
    latency_30m_ms           DOUBLE,
    throughput_30m_tps       DOUBLE,
    status                   VARCHAR,
    supported_parameters_json VARCHAR NOT NULL,
    raw_record_sha256        VARCHAR NOT NULL,
    PRIMARY KEY(source_snapshot_id, model_id, provider_id, endpoint_tag)
);

CREATE TABLE IF NOT EXISTS provider_capability_snapshot (
    source_snapshot_id       VARCHAR NOT NULL,
    captured_at              TIMESTAMPTZ NOT NULL,
    provider_id              VARCHAR NOT NULL,
    display_name             VARCHAR,
    confidence               DOUBLE,
    last_reviewed_date       DATE,
    provider_profile         VARCHAR,
    risk                     VARCHAR,
    implemented_features_json VARCHAR NOT NULL,
    missing_features_json    VARCHAR NOT NULL,
    test_count               BIGINT,
    wire_api_count           BIGINT,
    supports_text_input      BOOLEAN,
    supports_text_output     BOOLEAN,
    supports_image_input     BOOLEAN,
    supports_image_output    BOOLEAN,
    supports_audio_input     BOOLEAN,
    supports_audio_output    BOOLEAN,
    supports_streaming       BOOLEAN,
    supports_tools           BOOLEAN,
    supports_parallel_tools  BOOLEAN,
    supports_strict_schema   BOOLEAN,
    reasoning_supported      BOOLEAN,
    reasoning_field          VARCHAR,
    reasoning_efforts_json   VARCHAR NOT NULL,
    rejects_unsupported_reasoning BOOLEAN,
    prompt_cache_status      VARCHAR,
    prompt_cache_min_tokens  BIGINT,
    prompt_cache_ttl         VARCHAR,
    prompt_cache_mechanism_count BIGINT,
    request_max_retries      BIGINT,
    stream_max_retries       BIGINT,
    recommended_idle_timeout_ms BIGINT,
    cost_management_status   VARCHAR,
    cost_lever_count         BIGINT,
    reports_cached_tokens    BOOLEAN,
    reports_reasoning_tokens BOOLEAN,
    reports_audio_tokens     BOOLEAN,
    reports_image_tokens     BOOLEAN,
    endpoint_group_count     BIGINT,
    hosted_tool_count        BIGINT,
    raw_record_sha256        VARCHAR NOT NULL,
    PRIMARY KEY(source_snapshot_id, provider_id)
);

CREATE TABLE IF NOT EXISTS benchmark_observation (
    source_snapshot_id   VARCHAR NOT NULL,
    observation_id       VARCHAR NOT NULL,
    observed_at          TIMESTAMPTZ,
    model_id             VARCHAR NOT NULL,
    provider_id          VARCHAR,
    benchmark            VARCHAR NOT NULL,
    task                 VARCHAR,
    metric_name          VARCHAR NOT NULL,
    metric_value         DOUBLE NOT NULL,
    metric_unit          VARCHAR,
    split                VARCHAR,
    sample_count         BIGINT,
    standard_error       DOUBLE,
    evaluation_version   VARCHAR,
    provenance_url       VARCHAR,
    license_spdx         VARCHAR,
    raw_record_sha256    VARCHAR NOT NULL,
    metadata_json        VARCHAR NOT NULL,
    PRIMARY KEY(source_snapshot_id, observation_id)
);

CREATE TABLE IF NOT EXISTS preference_observation (
    source_snapshot_id   VARCHAR NOT NULL,
    comparison_id        VARCHAR NOT NULL,
    observed_at          TIMESTAMPTZ,
    request_hash         VARCHAR,
    task_type            VARCHAR,
    domain               VARCHAR,
    language             VARCHAR,
    model_a              VARCHAR NOT NULL,
    model_b              VARCHAR NOT NULL,
    winner               VARCHAR NOT NULL,
    turn_count           BIGINT,
    sample_weight        DOUBLE,
    confidence           DOUBLE,
    provenance_url       VARCHAR,
    license_spdx         VARCHAR,
    raw_record_sha256    VARCHAR NOT NULL,
    metadata_json        VARCHAR NOT NULL,
    PRIMARY KEY(source_snapshot_id, comparison_id)
);

CREATE TABLE IF NOT EXISTS openrouter_eval_model (
    source_snapshot_id       VARCHAR NOT NULL,
    model_id                 VARCHAR NOT NULL,
    name                     VARCHAR,
    provider_id              VARCHAR,
    created_at               TIMESTAMPTZ,
    context_length           BIGINT,
    prompt_price_usd_token   DOUBLE,
    completion_price_usd_token DOUBLE,
    last_seen_at             TIMESTAMPTZ,
    PRIMARY KEY(source_snapshot_id, model_id)
);

CREATE TABLE IF NOT EXISTS openrouter_eval (
    source_snapshot_id   VARCHAR NOT NULL,
    eval_id              BIGINT NOT NULL,
    model_id             VARCHAR NOT NULL,
    run_at               TIMESTAMPTZ,
    tier                 BIGINT,
    target_tokens        BIGINT,
    actual_tokens        BIGINT,
    latency_ms           DOUBLE,
    http_status          BIGINT,
    ok                   BOOLEAN,
    snippet_sha256       VARCHAR,
    error_sha256         VARCHAR,
    PRIMARY KEY(source_snapshot_id, eval_id)
);

CREATE TABLE IF NOT EXISTS route_decision (
    source_snapshot_id       VARCHAR NOT NULL,
    request_id               VARCHAR NOT NULL,
    decided_at               TIMESTAMPTZ NOT NULL,
    request_hash             VARCHAR NOT NULL,
    tenant_hash              VARCHAR,
    policy_id                VARCHAR NOT NULL,
    checkpoint_id            VARCHAR NOT NULL,
    catalog_snapshot_id      VARCHAR,
    explicit_override        VARCHAR,
    constraints_json         VARCHAR NOT NULL,
    chosen_model             VARCHAR NOT NULL,
    chosen_provider          VARCHAR,
    chosen_score             DOUBLE,
    confidence               DOUBLE,
    logging_propensity       DOUBLE,
    fallback_chain_json      VARCHAR NOT NULL,
    task_feature_version     VARCHAR,
    task_dimension           BIGINT,
    task_feature_encoding    VARCHAR,
    task_feature_sha256      VARCHAR,
    task_features_hex        VARCHAR,
    PRIMARY KEY(source_snapshot_id, request_id)
);

CREATE TABLE IF NOT EXISTS route_candidate_exposure (
    source_snapshot_id   VARCHAR NOT NULL,
    request_id           VARCHAR NOT NULL,
    rank                 BIGINT NOT NULL,
    model_id             VARCHAR NOT NULL,
    provider_id          VARCHAR,
    feasible             BOOLEAN NOT NULL,
    rejection_reason     VARCHAR,
    predicted_quality    DOUBLE,
    predicted_failure    DOUBLE,
    predicted_latency_ms DOUBLE,
    predicted_cost_usd   DOUBLE,
    score                DOUBLE,
    logging_propensity   DOUBLE,
    chosen               BOOLEAN NOT NULL,
    PRIMARY KEY(source_snapshot_id, request_id, rank)
);

CREATE TABLE IF NOT EXISTS route_outcome_revision (
    source_snapshot_id       VARCHAR NOT NULL,
    revision_id              BIGINT NOT NULL,
    request_id               VARCHAR NOT NULL,
    revision_number          BIGINT NOT NULL,
    recorded_at              TIMESTAMPTZ NOT NULL,
    supersedes_revision_id   BIGINT,
    completed_at             TIMESTAMPTZ NOT NULL,
    intended_model           VARCHAR NOT NULL,
    actual_model             VARCHAR,
    actual_provider          VARCHAR,
    generation_id_sha256     VARCHAR,
    http_success             BOOLEAN,
    task_success             BOOLEAN,
    provider_failure         BOOLEAN,
    tool_valid               BOOLEAN,
    schema_valid             BOOLEAN,
    refusal                  BOOLEAN,
    user_retry               BOOLEAN,
    accepted_or_used         BOOLEAN,
    prompt_tokens            BIGINT,
    completion_tokens        BIGINT,
    reasoning_tokens         BIGINT,
    cache_read_tokens        BIGINT,
    cache_write_tokens       BIGINT,
    cost_usd                 DOUBLE,
    ttft_ms                  DOUBLE,
    e2e_ms                   DOUBLE,
    finish_reason            VARCHAR,
    label_source             VARCHAR NOT NULL,
    label_confidence         DOUBLE NOT NULL,
    censored                 BOOLEAN NOT NULL,
    PRIMARY KEY(source_snapshot_id, revision_id)
);

CREATE TABLE IF NOT EXISTS route_pairwise_comparison (
    source_snapshot_id   VARCHAR NOT NULL,
    comparison_id        VARCHAR NOT NULL,
    request_hash         VARCHAR NOT NULL,
    catalog_snapshot_id  VARCHAR,
    model_a              VARCHAR NOT NULL,
    model_b              VARCHAR NOT NULL,
    winner               VARCHAR,
    score_a              DOUBLE,
    score_b              DOUBLE,
    source               VARCHAR NOT NULL,
    confidence           DOUBLE NOT NULL,
    created_at           TIMESTAMPTZ NOT NULL,
    PRIMARY KEY(source_snapshot_id, comparison_id)
);

CREATE TABLE IF NOT EXISTS data_quality_result (
    run_id               VARCHAR NOT NULL,
    checked_at           TIMESTAMPTZ NOT NULL,
    check_name           VARCHAR NOT NULL,
    dimension            VARCHAR NOT NULL,
    severity             VARCHAR NOT NULL,
    passed               BOOLEAN NOT NULL,
    observed_value       DOUBLE,
    expected_value       VARCHAR,
    detail               VARCHAR NOT NULL,
    PRIMARY KEY(run_id, check_name)
);

CREATE OR REPLACE VIEW latest_source_snapshot AS
SELECT * EXCLUDE(snapshot_rank)
FROM (
    SELECT *, row_number() OVER (
        PARTITION BY source_key ORDER BY captured_at DESC, snapshot_id DESC
    ) AS snapshot_rank
    FROM source_snapshot
)
WHERE snapshot_rank = 1;

CREATE OR REPLACE VIEW latest_catalog_model AS
SELECT model.*
FROM catalog_model_snapshot AS model
JOIN latest_source_snapshot AS source
  ON source.snapshot_id = model.source_snapshot_id
 AND source.source_key = 'openrouter_catalog';

CREATE OR REPLACE VIEW latest_native_model_offer AS
SELECT offer.*
FROM native_model_offer_snapshot AS offer
JOIN latest_source_snapshot AS source
  ON source.snapshot_id = offer.source_snapshot_id
 AND source.source_key = 'oh_my_pi_model_catalog';

CREATE OR REPLACE VIEW latest_provider_endpoint AS
SELECT endpoint.* EXCLUDE(snapshot_rank)
FROM (
    SELECT provider_endpoint_snapshot.*,
           row_number() OVER (
               PARTITION BY model_id, provider_id, endpoint_tag
               ORDER BY captured_at DESC, source_snapshot_id DESC
           ) AS snapshot_rank
    FROM provider_endpoint_snapshot
) AS endpoint
WHERE snapshot_rank = 1;

CREATE OR REPLACE VIEW model_provider_offer AS
SELECT
    'openrouter_endpoint' AS source_family,
    endpoint.source_snapshot_id,
    endpoint.captured_at,
    endpoint.model_id,
    endpoint.provider_id,
    endpoint.endpoint_tag,
    endpoint.context_length AS context_window,
    endpoint.max_completion_tokens AS max_tokens,
    endpoint.prompt_price_usd_token,
    endpoint.completion_price_usd_token,
    NULL::DECIMAL(38,18) AS cache_read_price_usd_token,
    NULL::DECIMAL(38,18) AS cache_write_price_usd_token,
    catalog.input_modalities_json,
    catalog.output_modalities_json,
    json_contains(
        CAST(catalog.supported_parameters_json AS JSON), CAST('"tools"' AS JSON)
    ) AS native_tools_usable,
    NULL::BOOLEAN AS reasoning,
    endpoint.status AS operational_status,
    CASE
        WHEN endpoint.prompt_price_usd_token IS NULL
             AND endpoint.completion_price_usd_token IS NULL THEN 'unavailable'
        WHEN coalesce(endpoint.prompt_price_usd_token, 0) > 0
             OR coalesce(endpoint.completion_price_usd_token, 0) > 0
             THEN 'quoted_nonzero'
        ELSE 'zero_unspecified'
    END AS pricing_status,
    endpoint.raw_record_sha256
FROM latest_provider_endpoint AS endpoint
LEFT JOIN latest_catalog_model AS catalog USING(model_id)
UNION ALL
SELECT
    'native_catalog' AS source_family,
    native.source_snapshot_id,
    native.captured_at,
    native.model_id,
    native.inference_provider_id AS provider_id,
    NULL AS endpoint_tag,
    native.context_window,
    native.max_tokens,
    native.input_price_usd_million / 1000000::DECIMAL(38,18),
    native.output_price_usd_million / 1000000::DECIMAL(38,18),
    native.cache_read_price_usd_million / 1000000::DECIMAL(38,18),
    native.cache_write_price_usd_million / 1000000::DECIMAL(38,18),
    native.input_modalities_json,
    NULL::VARCHAR AS output_modalities_json,
    native.native_tools_usable,
    native.reasoning,
    NULL::VARCHAR AS operational_status,
    native.pricing_status,
    native.raw_record_sha256
FROM latest_native_model_offer AS native;

CREATE OR REPLACE VIEW model_identity AS
WITH identities AS (
    SELECT model_id, provider_id, 'catalog' AS source FROM catalog_model_snapshot
    UNION ALL
    SELECT model_id, provider_id, 'openrouter_eval' FROM openrouter_eval_model
    UNION ALL
    SELECT model_id, provider_id, 'baseline' FROM baseline_event WHERE model_id IS NOT NULL
    UNION ALL
    SELECT model_id, provider_id, 'chronicle' FROM chronicle_event WHERE model_id IS NOT NULL
    UNION ALL
    SELECT chosen_model, chosen_provider, 'route_decision' FROM route_decision
    UNION ALL
    SELECT actual_model, actual_provider, 'route_outcome' FROM route_outcome_revision
        WHERE actual_model IS NOT NULL
    UNION ALL
    SELECT model_id, provider_id, 'benchmark' FROM benchmark_observation
), normalized AS (
    SELECT lower(trim(model_id)) AS model_id, provider_id, source
    FROM identities
    WHERE model_id IS NOT NULL AND trim(model_id) <> ''
)
SELECT model_id,
       coalesce(
           max(provider_id) FILTER (WHERE provider_id IS NOT NULL AND provider_id <> ''),
           CASE WHEN contains(model_id, '/') THEN split_part(model_id, '/', 1) END,
           'unknown'
       ) AS provider_id,
       count(*) AS observation_count,
       list_sort(list_distinct(list(source))) AS evidence_sources
FROM normalized
GROUP BY model_id;

CREATE OR REPLACE VIEW model_universe AS
WITH observations AS (
    SELECT model_id, 'openrouter_catalog' AS source, NULL::VARCHAR AS inference_provider_id
    FROM catalog_model_snapshot
    UNION ALL
    SELECT model_id, 'native_catalog', inference_provider_id
    FROM native_model_offer_snapshot
    UNION ALL
    SELECT model_id, 'openrouter_endpoint', provider_id
    FROM provider_endpoint_snapshot
    UNION ALL
    SELECT model_id, 'local_eval', NULL FROM openrouter_eval_model
    UNION ALL
    SELECT model_id, 'baseline', NULL FROM baseline_event WHERE model_id IS NOT NULL
    UNION ALL
    SELECT model_id, 'benchmark', NULL FROM benchmark_observation
    UNION ALL
    SELECT chosen_model, 'route_decision', chosen_provider FROM route_decision
    UNION ALL
    SELECT actual_model, 'route_outcome', actual_provider
    FROM route_outcome_revision WHERE actual_model IS NOT NULL
), valid AS (
    SELECT trim(model_id) AS model_id,
           lower(trim(model_id)) AS normalized_model_id,
           source,
           inference_provider_id
    FROM observations
    WHERE model_id IS NOT NULL AND trim(model_id) <> ''
)
SELECT model_id,
       normalized_model_id,
       count(*) AS observation_count,
       list_sort(list_distinct(list(source))) AS evidence_sources,
       coalesce(
           list_sort(list_distinct(list(inference_provider_id)
               FILTER (WHERE inference_provider_id IS NOT NULL))),
           []::VARCHAR[]
       ) AS inference_providers
FROM valid
GROUP BY model_id, normalized_model_id;

CREATE OR REPLACE VIEW current_routable_model_universe AS
WITH observations AS (
    SELECT model_id, 'openrouter_catalog' AS source,
           NULL::VARCHAR AS inference_provider_id
    FROM latest_catalog_model
    UNION ALL
    SELECT model_id, 'native_catalog', inference_provider_id
    FROM latest_native_model_offer
    UNION ALL
    SELECT model_id, 'openrouter_endpoint', provider_id
    FROM latest_provider_endpoint
), valid AS (
    SELECT trim(model_id) AS model_id,
           lower(trim(model_id)) AS normalized_model_id,
           source,
           inference_provider_id
    FROM observations
    WHERE model_id IS NOT NULL AND trim(model_id) <> ''
)
SELECT model_id,
       normalized_model_id,
       count(*) AS observation_count,
       list_sort(list_distinct(list(source))) AS evidence_sources,
       coalesce(
           list_sort(list_distinct(list(inference_provider_id)
               FILTER (WHERE inference_provider_id IS NOT NULL))),
           []::VARCHAR[]
       ) AS inference_providers
FROM valid
GROUP BY model_id, normalized_model_id;

CREATE OR REPLACE VIEW latest_route_outcome AS
SELECT * EXCLUDE(revision_rank)
FROM (
    SELECT route_outcome_revision.*,
           row_number() OVER (
               PARTITION BY request_id ORDER BY revision_id DESC
           ) AS revision_rank
    FROM route_outcome_revision
)
WHERE revision_rank = 1;

CREATE OR REPLACE VIEW route_training_fact AS
SELECT
    decision.request_id,
    decision.decided_at,
    decision.request_hash,
    decision.policy_id,
    decision.checkpoint_id,
    decision.catalog_snapshot_id,
    decision.chosen_model,
    decision.chosen_provider,
    decision.chosen_score,
    decision.confidence,
    decision.logging_propensity,
    decision.task_feature_version,
    decision.task_dimension,
    decision.task_feature_encoding,
    decision.task_feature_sha256,
    decision.task_features_hex,
    outcome.revision_id AS outcome_revision_id,
    outcome.completed_at,
    outcome.actual_model,
    outcome.actual_provider,
    outcome.http_success,
    outcome.task_success,
    outcome.provider_failure,
    outcome.tool_valid,
    outcome.schema_valid,
    outcome.refusal,
    outcome.user_retry,
    outcome.accepted_or_used,
    outcome.prompt_tokens,
    outcome.completion_tokens,
    outcome.reasoning_tokens,
    outcome.cache_read_tokens,
    outcome.cache_write_tokens,
    outcome.cost_usd,
    outcome.ttft_ms,
    outcome.e2e_ms,
    outcome.label_source,
    outcome.label_confidence,
    outcome.censored,
    catalog.source_snapshot_id AS asof_catalog_snapshot_id,
    catalog.captured_at AS asof_catalog_captured_at,
    catalog.context_length,
    catalog.prompt_price_usd_token,
    catalog.completion_price_usd_token
FROM route_decision AS decision
LEFT JOIN latest_route_outcome AS outcome USING(request_id)
LEFT JOIN LATERAL (
    SELECT candidate.*
    FROM catalog_model_snapshot AS candidate
    WHERE lower(candidate.model_id) = lower(decision.chosen_model)
      AND candidate.captured_at <= decision.decided_at
    ORDER BY candidate.captured_at DESC, candidate.source_snapshot_id DESC
    LIMIT 1
) AS catalog ON true;

CREATE OR REPLACE VIEW model_evidence AS
WITH operational AS (
    SELECT lower(coalesce(actual_model, intended_model)) AS model_id,
           count(*) AS outcome_count,
           sum(CASE WHEN http_success THEN 1 ELSE 0 END) AS http_successes,
           sum(CASE WHEN task_success THEN 1 ELSE 0 END) AS task_successes,
           sum(CASE WHEN provider_failure THEN 1 ELSE 0 END) AS provider_failures,
           avg(cost_usd) FILTER (WHERE cost_usd IS NOT NULL) AS mean_cost_usd,
           quantile_cont(e2e_ms, 0.5) FILTER (WHERE e2e_ms IS NOT NULL) AS p50_e2e_ms,
           quantile_cont(e2e_ms, 0.95) FILTER (WHERE e2e_ms IS NOT NULL) AS p95_e2e_ms
    FROM latest_route_outcome
    GROUP BY 1
), historical AS (
    SELECT lower(model_id) AS model_id,
           count(*) AS baseline_turn_count,
           avg(estimated_cost_usd) FILTER (WHERE estimated_cost_usd IS NOT NULL) AS baseline_mean_cost_usd,
           sum(input_tokens) AS baseline_input_tokens,
           sum(output_tokens) AS baseline_output_tokens
    FROM baseline_event
    WHERE category = 'turn' AND title = 'turn_done' AND model_id IS NOT NULL
    GROUP BY 1
), evals AS (
    SELECT lower(model_id) AS model_id,
           count(*) AS eval_count,
           sum(CASE WHEN ok THEN 1 ELSE 0 END) AS eval_successes,
           avg(latency_ms) FILTER (WHERE latency_ms IS NOT NULL) AS eval_mean_latency_ms
    FROM openrouter_eval
    GROUP BY 1
), benchmark AS (
    SELECT lower(model_id) AS model_id,
           count(*) AS benchmark_metric_count,
           max(metric_value) FILTER (WHERE metric_name = 'intelligence_index') AS intelligence_index,
           max(metric_value) FILTER (WHERE metric_name = 'coding_index') AS coding_index,
           max(metric_value) FILTER (WHERE metric_name = 'agentic_index') AS agentic_index
    FROM benchmark_observation
    GROUP BY 1
)
SELECT identity.model_id,
       identity.provider_id,
       identity.evidence_sources,
       catalog.name,
       catalog.context_length,
       catalog.max_completion_tokens,
       catalog.prompt_price_usd_token,
       catalog.completion_price_usd_token,
       catalog.input_modalities_json,
       catalog.output_modalities_json,
       catalog.supported_parameters_json,
       coalesce(operational.outcome_count, 0) AS outcome_count,
       coalesce(operational.http_successes, 0) AS http_successes,
       coalesce(operational.task_successes, 0) AS task_successes,
       coalesce(operational.provider_failures, 0) AS provider_failures,
       operational.mean_cost_usd,
       operational.p50_e2e_ms,
       operational.p95_e2e_ms,
       coalesce(historical.baseline_turn_count, 0) AS baseline_turn_count,
       historical.baseline_mean_cost_usd,
       coalesce(historical.baseline_input_tokens, 0) AS baseline_input_tokens,
       coalesce(historical.baseline_output_tokens, 0) AS baseline_output_tokens,
       coalesce(evals.eval_count, 0) AS eval_count,
       coalesce(evals.eval_successes, 0) AS eval_successes,
       evals.eval_mean_latency_ms,
       coalesce(benchmark.benchmark_metric_count, 0) AS benchmark_metric_count,
       benchmark.intelligence_index,
       benchmark.coding_index,
       benchmark.agentic_index
FROM model_identity AS identity
LEFT JOIN latest_catalog_model AS catalog USING(model_id)
LEFT JOIN operational USING(model_id)
LEFT JOIN historical USING(model_id)
LEFT JOIN evals USING(model_id)
LEFT JOIN benchmark USING(model_id);

CREATE OR REPLACE VIEW process_health_hourly AS
SELECT
    time_bucket(INTERVAL '1 hour', observed_at) AS hour,
    count(*) AS observations,
    count(DISTINCT pid) AS processes,
    avg(rss_mb) AS mean_rss_mb,
    max(peak_rss_mb) AS max_peak_rss_mb,
    avg(cpu_user_ms + cpu_system_ms) AS mean_cpu_ms,
    sum(CASE WHEN memory_restart > 0 THEN 1 ELSE 0 END) AS memory_restart_events,
    sum(CASE WHEN memory_pressure > 0 THEN 1 ELSE 0 END) AS memory_pressure_events
FROM runtime_process_metric
GROUP BY 1;

CREATE OR REPLACE VIEW incident_daily AS
SELECT
    time_bucket(INTERVAL '1 day', observed_at) AS day,
    incident_class,
    count(*) AS incidents,
    sum(CASE WHEN memory_restart THEN 1 ELSE 0 END) AS memory_restarts,
    quantile_cont(peak_rss_mb, 0.5) FILTER (WHERE peak_rss_mb IS NOT NULL)
        AS p50_peak_rss_mb,
    quantile_cont(peak_rss_mb, 0.95) FILTER (WHERE peak_rss_mb IS NOT NULL)
        AS p95_peak_rss_mb,
    count(DISTINCT child_pid) FILTER (WHERE child_pid IS NOT NULL)
        AS distinct_child_pids
FROM runtime_incident
GROUP BY 1,2;

CREATE OR REPLACE VIEW incident_instance_match AS
SELECT incident.*,
       child.instance_id AS child_instance_id,
       child.model_id AS child_instance_model_id,
       supervisor.instance_id AS supervisor_instance_id,
       supervisor.model_id AS supervisor_instance_model_id,
       CASE
           WHEN child.instance_id IS NOT NULL AND supervisor.instance_id IS NOT NULL
                AND child.instance_id = supervisor.instance_id THEN 'both_same_instance'
           WHEN child.instance_id IS NOT NULL AND supervisor.instance_id IS NOT NULL
                THEN 'conflicting_pid_matches'
           WHEN child.instance_id IS NOT NULL THEN 'child_pid_active_interval'
           WHEN supervisor.instance_id IS NOT NULL THEN 'supervisor_pid_active_interval'
           ELSE 'unmatched'
       END AS match_role,
       CASE
           WHEN child.instance_id IS NOT NULL THEN 1.0
           WHEN supervisor.instance_id IS NOT NULL THEN 0.5
           ELSE 0.0
       END AS match_confidence,
       child.instance_id IS NOT NULL AS direct_model_attribution_allowed
FROM runtime_incident AS incident
LEFT JOIN LATERAL (
    SELECT candidate.*
    FROM baseline_instance AS candidate
    WHERE candidate.pid = incident.child_pid
      AND candidate.started_at <= incident.observed_at
      AND coalesce(candidate.ended_at, incident.observed_at) >= incident.observed_at
    ORDER BY candidate.started_at DESC
    LIMIT 1
) AS child ON true
LEFT JOIN LATERAL (
    SELECT candidate.*
    FROM baseline_instance AS candidate
    WHERE candidate.pid = incident.supervisor_pid
      AND candidate.started_at <= incident.observed_at
      AND coalesce(candidate.ended_at, incident.observed_at) >= incident.observed_at
    ORDER BY candidate.started_at DESC
    LIMIT 1
) AS supervisor ON true;

CREATE OR REPLACE VIEW swarm_comparison_group AS
WITH grouped AS (
    SELECT
        task_sha256,
        count(*) AS child_results,
        count(DISTINCT requested_model_id)
            FILTER (WHERE requested_model_id IS NOT NULL) AS model_count,
        sum(CASE WHEN process_success THEN 1 ELSE 0 END) AS process_success_count,
        avg(CASE WHEN process_success THEN 1.0 ELSE 0.0 END) AS process_success_rate,
        quantile_cont(duration_ms, 0.5) FILTER (WHERE duration_ms IS NOT NULL)
            AS p50_duration_ms,
        quantile_cont(duration_ms, 0.95) FILTER (WHERE duration_ms IS NOT NULL)
            AS p95_duration_ms,
        avg(CASE WHEN output_present THEN 1.0 ELSE 0.0 END)
            AS output_present_rate,
        sum(CASE WHEN cost_nonzero THEN 1 ELSE 0 END) AS positive_cost_count,
        avg(CASE WHEN cost_nonzero THEN 1.0 ELSE 0.0 END)
            AS positive_cost_coverage,
        sum(CASE WHEN status='done' AND NOT output_present THEN 1 ELSE 0 END)
            AS empty_done_count,
        avg(CASE WHEN status='done' AND NOT output_present THEN 1.0 ELSE 0.0 END)
            AS empty_done_rate,
        sum(CASE WHEN status='done' AND NOT output_present
                      AND coalesce(duration_ms,0) < 100 AND NOT cost_nonzero
                 THEN 1 ELSE 0 END) AS short_empty_done_count,
        avg(CASE WHEN status='done' AND NOT output_present
                     AND coalesce(duration_ms,0) < 100 AND NOT cost_nonzero
                 THEN 1.0 ELSE 0.0 END) AS short_empty_done_rate,
        sum(CASE WHEN task_maybe_truncated THEN 1 ELSE 0 END)
            AS maybe_truncated_task_count
    FROM swarm_child_result
    GROUP BY task_sha256
)
SELECT *,
       empty_done_rate >= 0.95
           AND coalesce(p50_duration_ms,0) < 100
           AND positive_cost_count = 0 AS degenerate_group,
       model_count > 1
           AND maybe_truncated_task_count = 0
           AND NOT (
               empty_done_rate >= 0.95
               AND coalesce(p50_duration_ms,0) < 100
               AND positive_cost_count = 0
           ) AS comparative_operational_usable
FROM grouped
WHERE model_count > 1;

CREATE OR REPLACE VIEW deduplicated_tool_trace AS
SELECT * EXCLUDE(duplicate_rank)
FROM (
    SELECT tool_trace_metric.*,
           row_number() OVER (
               PARTITION BY source_file_sha256, observed_at, tool_name, ok,
                            latency_ms, token_count
               ORDER BY source_row_number
           ) AS duplicate_rank
    FROM tool_trace_metric
)
WHERE duplicate_rank = 1;

CREATE OR REPLACE VIEW enriched_transcript_turn AS
SELECT metric.*,
       instance.instance_id,
       instance.mode AS instance_mode,
       instance.started_at AS instance_started_at,
       instance.ended_at AS instance_ended_at
FROM transcript_turn_metric AS metric
LEFT JOIN LATERAL (
    SELECT candidate.*
    FROM baseline_instance AS candidate
    WHERE candidate.pid = metric.source_pid
      AND candidate.started_at <= metric.observed_at
      AND coalesce(candidate.ended_at, metric.observed_at) >= metric.observed_at
    ORDER BY candidate.started_at DESC
    LIMIT 1
) AS instance ON true;

CREATE OR REPLACE VIEW run_execution_fact AS
WITH wal AS (
    SELECT run_id,
           count(*) AS wal_events,
           sum(CASE WHEN event_type = 'rl.trajectory.event' THEN 1 ELSE 0 END)
               AS trajectory_events,
           sum(CASE WHEN event_type = 'tool.call' THEN 1 ELSE 0 END) AS tool_calls,
           sum(CASE WHEN event_type = 'tool.result' AND tool_ok = false
                    THEN 1 ELSE 0 END) AS tool_failures,
           min(event_at) AS first_wal_event_at,
           max(event_at) AS last_wal_event_at
    FROM run_wal_event
    GROUP BY run_id
), receipt AS (
    SELECT * EXCLUDE(receipt_rank)
    FROM (
        SELECT run_wal_event.*,
               row_number() OVER (
                   PARTITION BY run_id ORDER BY sequence_number DESC
               ) AS receipt_rank
        FROM run_wal_event
        WHERE event_type = 'run.receipt'
    )
    WHERE receipt_rank = 1
)
SELECT manifest.*,
       CASE
           WHEN session.ended_at IS NOT NULL OR receipt.run_id IS NOT NULL
               THEN 'completed'
           ELSE manifest.status
       END AS derived_status,
       manifest.status = 'running'
           AND manifest.updated_at < source.captured_at - INTERVAL '24 hours'
           AND session.ended_at IS NULL
           AND receipt.run_id IS NULL AS stale_running_at_snapshot,
       session.provider_id AS chronicle_provider_id,
       session.model_id AS chronicle_model_id,
       wal.wal_events,
       wal.trajectory_events,
       wal.tool_calls,
       wal.tool_failures,
       receipt.input_tokens,
       receipt.output_tokens,
       receipt.cache_read_tokens,
       receipt.cache_write_tokens,
       receipt.reasoning_tokens,
       receipt.cost_usd_total AS final_cost_usd,
       receipt.duration_ms,
       receipt.journal_records,
       wal.first_wal_event_at,
       wal.last_wal_event_at,
       instance.pid AS baseline_pid,
       instance.model_id AS baseline_model_id
FROM run_manifest_snapshot AS manifest
JOIN source_snapshot AS source
  ON source.snapshot_id = manifest.source_snapshot_id
LEFT JOIN chronicle_session AS session
  ON session.session_id = manifest.session_id
LEFT JOIN baseline_instance AS instance
  ON instance.instance_id = manifest.instance_id
LEFT JOIN wal USING(run_id)
LEFT JOIN receipt USING(run_id);
