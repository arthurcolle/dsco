# API Reference

This file is auto-generated from header declarations in `include/`.

- Generator: `./scripts/gen_api_reference.sh`
- Headers scanned: 194

## Regeneration

```bash
./scripts/gen_api_reference.sh
```

## `acp_server.h`

Function-like declarations: 1

### Declarations

- `int acp_server_run(const char *dsco_argv0);`

## `activation_lease.h`

Function-like declarations: 12

### Declarations

- `bool activation_lease_default_path(char *out, size_t out_len);`
- `activation_lease_status_t activation_lease_validate(const activation_lease_t *lease, int64_t now_unix, char *err, size_t err_len);`
- `int activation_lease_to_json(const activation_lease_t *lease, char *out, size_t out_len);`
- `activation_lease_status_t activation_lease_from_json(const char *json, activation_lease_t *out);`
- `activation_lease_status_t activation_lease_save_file(const char *path, const activation_lease_t *lease);`
- `activation_lease_status_t activation_lease_load_file(const char *path, activation_lease_t *out, char *err, size_t err_len);`
- `activation_lease_status_t activation_lease_verify_signature(const activation_lease_t *lease, char *err, size_t err_len);`
- `activation_lease_status_t activation_lease_load_file_verified(const char *path, activation_lease_t *out, char *err, size_t err_len);`
- `activation_lease_status_t activation_lease_remove_file(const char *path);`
- `activation_lease_status_t activation_lease_acquire_file(const char *path, const activation_lease_t *lease, bool replace_expired, char *err, size_t err_len);`
- `activation_lease_status_t activation_lease_renew_file(const char *path, const char *lease_id, int64_t new_expires_at, char *err, size_t err_len);`
- `activation_lease_status_t activation_lease_release_file(const char *path, const char *lease_id, char *err, size_t err_len);`

## `agent.h`

Function-like declarations: 4

### Declarations

- `void agent_set_launch_argv(int argc, char **argv);`
- `void agent_set_initial_prompt(const char *prompt);`
- `bool agent_run(const char *api_key, const char *model, const char *topology_name, bool topology_auto, const char *provider_override);`
- `bool agent_run_orchestrated(const char *api_key, const char *chat_model, const char *worker_model, const char *provider_override);`

## `agent_event.h`

Function-like declarations: 2

### Declarations

- `bool agent_event_emit(const agent_event_ctx_t *ctx, const char *event_name, const char *status, const char *payload_json, agent_event_flags_t flags);`
- `bool agent_event_emit_simple(const char *event_name, const char *status, const char *payload_json, agent_event_flags_t flags);`

## `agent_profile.h`

Function-like declarations: 16

### Declarations

- `void agent_profiles_init(void);`
- `int agent_profiles_count(void);`
- `agent_profile_t *agent_profile_get(int idx);`
- `agent_profile_t *agent_profile_find(const char *name);`
- `bool agent_profile_save(const agent_profile_t *p);`
- `bool agent_profile_delete(const char *name);`
- `bool agent_profiles_load(void);`
- `bool agent_profiles_persist(void);`
- `int agent_profile_to_json(const agent_profile_t *p, char *buf, size_t len);`
- `int agent_profiles_all_json(char *buf, size_t len);`
- `bool agent_profile_from_json(agent_profile_t *p, const char *json);`
- `void agent_profile_set_active(const char *name);`
- `const char *agent_profile_active_name(void);`
- `const agent_profile_t *agent_profile_active(void);`
- `bool agent_profile_tool_allowed(const char *tool_name, const char *group_hint);`
- `bool agent_profile_tool_allowed_strict(const char *tool_name, const char *group_hint);`

## `agent_ui_canvas.h`

Function-like declarations: 12

### Declarations

- `agent_ui_canvas_t *agent_ui_canvas_create(int physical_width, int physical_height, int backing_scale, const agent_ui_theme_t *theme);`
- `void agent_ui_canvas_destroy(agent_ui_canvas_t *canvas);`
- `bool agent_ui_canvas_set_theme(agent_ui_canvas_t *canvas, const agent_ui_theme_t *theme);`
- `bool agent_ui_canvas_render(agent_ui_canvas_t *canvas, const native_ui_scene_t *scene, const native_ui_scene_t *previous);`
- `bool agent_ui_canvas_write_ppm(const agent_ui_canvas_t *canvas, const char *path);`
- `int agent_ui_canvas_physical_width(const agent_ui_canvas_t *canvas);`
- `int agent_ui_canvas_physical_height(const agent_ui_canvas_t *canvas);`
- `int agent_ui_canvas_logical_width(const agent_ui_canvas_t *canvas);`
- `int agent_ui_canvas_logical_height(const agent_ui_canvas_t *canvas);`
- `int agent_ui_canvas_backing_scale(const agent_ui_canvas_t *canvas);`
- `const uint8_t *agent_ui_canvas_pixels(const agent_ui_canvas_t *canvas);`
- `size_t agent_ui_canvas_size(const agent_ui_canvas_t *canvas);`

## `agent_ui_components.h`

Function-like declarations: 21

### Declarations

- `bool agent_ui_builder_init(agent_ui_builder_t *builder, native_ui_scene_t *scene, const agent_ui_theme_t *theme);`
- `uint64_t agent_ui_component_key(agent_ui_component_kind_t kind, uint32_t instance);`
- `uint64_t agent_ui_component_part_key(uint64_t root_key, agent_ui_component_part_t part);`
- `int agent_ui_add_section_header(agent_ui_builder_t *builder, int parent, uint64_t key, const char *title, const char *caption, const char *meta);`
- `int agent_ui_add_status_badge(agent_ui_builder_t *builder, int parent, uint64_t key, const char *label, agent_ui_tone_t tone);`
- `int agent_ui_add_message(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_message_model_t *model);`
- `int agent_ui_add_reasoning(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_reasoning_model_t *model);`
- `int agent_ui_add_tool(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_tool_model_t *model);`
- `int agent_ui_add_plan_step(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_plan_step_model_t *model);`
- `int agent_ui_add_agent_card(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_agent_model_t *model);`
- `int agent_ui_add_topology(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_topology_model_t *model);`
- `int agent_ui_add_artifact(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_artifact_model_t *model);`
- `int agent_ui_add_permission(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_permission_model_t *model);`
- `int agent_ui_add_metric(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_metric_model_t *model);`
- `int agent_ui_add_queue_item(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_queue_model_t *model);`
- `int agent_ui_add_timeline_event(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_timeline_model_t *model);`
- `int agent_ui_add_notification(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_notification_model_t *model);`
- `int agent_ui_add_command(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_command_model_t *model);`
- `int agent_ui_add_composer(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_composer_component_model_t *model);`
- `int agent_ui_add_code_block(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_code_model_t *model);`
- `int agent_ui_add_theme_swatch(agent_ui_builder_t *builder, int parent, uint64_t key, const agent_ui_theme_t *theme, bool selected);`

## `agent_ui_gallery.h`

Function-like declarations: 7

### Declarations

- `const char *agent_ui_gallery_page_name(agent_ui_gallery_page_t page);`
- `bool agent_ui_gallery_build(native_ui_scene_t *scene, int logical_width, int logical_height, agent_ui_gallery_page_t page, const agent_ui_theme_t *theme);`
- `bool agent_ui_gallery_write_ppm(const char *path, int physical_width, int physical_height, int backing_scale, agent_ui_gallery_page_t page, const agent_ui_theme_t *theme);`
- `bool agent_ui_gallery_session_begin(agent_ui_gallery_session_t *session, FILE *out, int forced_width, int forced_height, int forced_scale);`
- `bool agent_ui_gallery_session_present(agent_ui_gallery_session_t *session, FILE *out, agent_ui_gallery_page_t page, const agent_ui_theme_t *theme);`
- `bool agent_ui_gallery_session_geometry_changed(agent_ui_gallery_session_t *session, FILE *out);`
- `void agent_ui_gallery_session_end(agent_ui_gallery_session_t *session, FILE *out);`

## `agent_ui_theme.h`

Function-like declarations: 7

### Declarations

- `size_t agent_ui_theme_count(void);`
- `const agent_ui_theme_t *agent_ui_theme_at(size_t index);`
- `const agent_ui_theme_t *agent_ui_theme_find(const char *id);`
- `const agent_ui_theme_t *agent_ui_theme_default(void);`
- `const agent_ui_theme_t *agent_ui_theme_next(const agent_ui_theme_t *theme, int direction);`
- `bool agent_ui_theme_validate(const agent_ui_theme_t *theme, char *message, size_t message_capacity);`
- `double agent_ui_theme_contrast(px_backend_color_t foreground, px_backend_color_t background);`

## `anim.h`

Function-like declarations: 17

### Declarations

- `anim_opts_t anim_opts_default(void);`
- `int anim_life(const anim_opts_t *o);`
- `int anim_rule(const anim_opts_t *o);`
- `int anim_attractor(const anim_opts_t *o);`
- `int anim_rain(const anim_opts_t *o);`
- `int anim_dispatch(const char *json);`
- `void anim_begin(anim_term_t *t);`
- `void anim_end(anim_term_t *t);`
- `bool anim_tick(int fps);`
- `bool anim_interrupted(void);`
- `long anim_frame_budget(long gens);`
- `void anim_resolve_size(int width, int height, int *W, int *H);`
- `void anim_paint(FILE *out, const tui_braille_t *bc, bool color, const int *cell_color);`
- `void anim_paint_rgb(FILE *out, const tui_braille_t *bc, const tui_rgb_t *cell_rgb);`
- `void anim_raw_enable(anim_raw_t *r);`
- `void anim_raw_restore(anim_raw_t *r);`
- `int anim_poll_key(void);`

## `arena_alloc.h`

Function-like declarations: 14

### Declarations

- `arena_t *arena_scratch(void);`
- `arena_t *arena_session(void);`
- `void arena_subsystem_init(void);`
- `void arena_subsystem_shutdown(void);`
- `void arena_scratch_reset(void);`
- `void *scratch_alloc(size_t size);`
- `char *scratch_strdup(const char *s);`
- `char *scratch_sprintf(const char *fmt, ...);`
- `void *session_alloc(size_t size);`
- `char *session_strdup(const char *s);`
- `char *session_sprintf(const char *fmt, ...);`
- `arena_temp_t arena_temp_begin(void);`
- `void arena_temp_end(arena_temp_t mark);`
- `arena_stats_t arena_get_stats(void);`

## `ast.h`

Function-like declarations: 11

### Declarations

- `ast_file_t *ast_parse_file(const char *path);`
- `void ast_free_file(ast_file_t *f);`
- `ast_node_t *ast_find_function(ast_file_t *f, const char *name);`
- `int ast_count_type(ast_file_t *f, ast_node_type_t type);`
- `ast_project_t *ast_introspect(const char *project_dir);`
- `void ast_free_project(ast_project_t *p);`
- `int ast_summary_json(ast_project_t *p, char *buf, size_t len);`
- `int ast_file_summary_json(ast_file_t *f, char *buf, size_t len);`
- `int ast_function_list_json(ast_file_t *f, char *buf, size_t len);`
- `int ast_dependency_graph(ast_project_t *p, char *buf, size_t len);`
- `int ast_call_graph(ast_project_t *p, const char *func_name, char *buf, size_t len);`

## `audit_log.h`

Function-like declarations: 8

### Declarations

- `audit_log_t *audit_log_open(const char *path, const uint8_t key[32]);`
- `int64_t audit_log_write(audit_log_t *al, const char *tag, const char *msg);`
- `bool audit_log_verify(audit_log_t *al, int64_t *bad_seq);`
- `typedef bool (*audit_log_iter_fn)(int64_t seq, int64_t ts, const char *tag, const char *msg, void *ctx);`
- `void audit_log_iter(audit_log_t *al, int64_t seq_from, int64_t seq_to, audit_log_iter_fn cb, void *ctx);`
- `void audit_log_close(audit_log_t *al);`
- `void audit_log_global_init(const char *path);`
- `int64_t audit_log(const char *tag, const char *msg);`

## `autoresearch.h`

Function-like declarations: 1

### Declarations

- `int autoresearch_cli(int argc, char **argv);`

## `avatar.h`

Function-like declarations: 3

### Declarations

- `bool avatar_is_kind(const char *kind);`
- `int avatar_plot(char *out, size_t cap, const char *json);`
- `int avatar_anim(const char *json);`

## `avian.h`

Function-like declarations: 17

### Declarations

- `void avian_init(avian_engine_t *a);`
- `void avian_destroy(avian_engine_t *a);`
- `int avian_nest_create(avian_engine_t *a, const char *name, const char *purpose, double warmth, double stability);`
- `bool avian_nest_add_material(avian_engine_t *a, int nest_id, const char *material, double quality, bool lining);`
- `bool avian_nest_roost(avian_engine_t *a, int nest_id, const char *reason, double cooldown);`
- `bool avian_nest_molt(avian_engine_t *a, int nest_id, const char *reason);`
- `int avian_brood_lay(avian_engine_t *a, int nest_id, const char *name, const char *kind, const char *lineage, double risk, int required_cycles);`
- `bool avian_brood_tend(avian_engine_t *a, int egg_id, double warmth, const char *evidence);`
- `bool avian_brood_fledge(avian_engine_t *a, int egg_id, char *reason, size_t reason_len);`
- `bool avian_brood_abandon(avian_engine_t *a, int egg_id, const char *reason);`
- `const avian_nest_t *avian_nest_get(const avian_engine_t *a, int nest_id);`
- `const avian_egg_t *avian_egg_get(const avian_engine_t *a, int egg_id);`
- `const char *avian_nest_state_name(avian_nest_state_t s);`
- `const char *avian_egg_state_name(avian_egg_state_t s);`
- `int avian_status_json(const avian_engine_t *a, char *buf, size_t len);`
- `int avian_nest_json(const avian_engine_t *a, int nest_id, char *buf, size_t len);`
- `int avian_egg_json(const avian_engine_t *a, int egg_id, char *buf, size_t len);`

## `baseline.h`

Function-like declarations: 16

### Declarations

- `bool baseline_start(const char *model, const char *mode);`
- `void baseline_stop(void);`
- `bool baseline_log(const char *category, const char *title, const char *detail, const char *metadata_json);`
- `bool baseline_log_usage(const char *category, const char *title, const char *detail, const char *metadata_json, int input_tokens, int output_tokens, int cache_read_tokens, int cache_write_tokens);`
- `bool baseline_log_usage_with_cost(const char *category, const char *title, const char *detail, const char *metadata_json, int input_tokens, int output_tokens, int cache_read_tokens, int cache_write_tokens, double reported_cost_usd);`
- `char *baseline_credit_report(const char *root_instance_id);`
- `double baseline_daily_cost(void);`
- `const char *baseline_instance_id(void);`
- `const char *baseline_db_path(void);`
- `int baseline_serve_http(int port, const char *default_instance_filter);`
- `void trace_new_id(char *out, size_t out_len);`
- `bool trace_span_begin(const char *trace_id, const char *name, const char *parent_span, char *span_id_out);`
- `bool trace_span_end(const char *span_id, const char *status, const char *metadata_json);`
- `void trace_query_recent(int limit);`
- `void trace_print_waterfall(const char *trace_id);`
- `void baseline_set_vfs(vfs_db_t *vfs);`

## `bg_learn.h`

Function-like declarations: 6

### Declarations

- `void bg_learn_set_enabled(bool enabled);`
- `bool bg_learn_is_enabled(void);`
- `bool bg_learn_start(void);`
- `void bg_learn_stop(void);`
- `int bg_learn_run_once(void);`
- `void bg_learn_stats(bg_learn_stats_t *out);`

## `callbacks.h`

Function-like declarations: 4

### Declarations

- `bool callback_policy_from_env(callback_policy_t *out);`
- `bool callback_event_matches(const callback_policy_t *policy, const char *event_name);`
- `bool callback_outbox_enqueue(const callback_policy_t *policy, const char *run_id, const char *event_name, const char *event_json);`
- `int callbacks_cli(int argc, char **argv);`

## `capability.h`

Function-like declarations: 9

### Declarations

- `unsigned dsco_caps_for_tool(const char *name, const char *input_json);`
- `bool dsco_cap_granted(dsco_cap_t cap, const char *tier);`
- `void dsco_flow_reset(void);`
- `void dsco_flow_note(unsigned caps);`
- `bool dsco_flow_tainted_untrusted(void);`
- `bool dsco_flow_accessed_private(void);`
- `bool dsco_flow_would_exfiltrate(unsigned caps);`
- `dsco_cap_decision_t dsco_capability_gate(const char *name, const char *input_json, const char *tier, char *reason, size_t reason_len);`
- `void dsco_capability_to_string(unsigned caps, char *out, size_t out_len);`

## `chronicle.h`

Function-like declarations: 33

### Declarations

- `bool chronicle_start(const chronicle_start_opts_t *opts);`
- `void chronicle_stop(void);`
- `bool chronicle_ready(void);`
- `chronicle_mode_t chronicle_mode(void);`
- `const char *chronicle_installation_id(void);`
- `const char *chronicle_session_id(void);`
- `const char *chronicle_root(void);`
- `const char *chronicle_db_path(void);`
- `const char *chronicle_run_id(void);`
- `const char *chronicle_journal_path(void);`
- `const char *chronicle_run_dir(void);`
- `bool chronicle_journal_append(const char *record_type, const char *payload_json, bool durable);`
- `int chronicle_runs_cli(int argc, char **argv);`
- `bool chronicle_cost_totals_for_session(const char *session_id, chronicle_cost_totals_t *out);`
- `bool chronicle_cost_totals_today(chronicle_cost_totals_t *out);`
- `void chronicle_new_id(char *out, size_t out_len);`
- `bool chronicle_span_begin(const char *trace_id, const char *parent_span_id, const char *span_type, const char *name, const char *payload_json, char *span_id_out);`
- `bool chronicle_span_end(const char *span_id, const char *status, const char *payload_json);`
- `bool chronicle_event(const char *event_type, const char *trace_id, const char *span_id, const char *parent_span_id, const char *actor_type, const char *actor_id, const char *payload_json, const char *sensitivity);`
- `bool chronicle_blob_put(const void *data, size_t len, const char *logical_type, const char *content_type, const char *sensitivity, char *sha_out, size_t sha_out_len);`
- `bool chronicle_blob_put_text(const char *text, const char *logical_type, const char *sensitivity, char *sha_out, size_t sha_out_len);`
- `bool chronicle_edge(const char *from_id, const char *to_id, const char *relation, double confidence, const char *metadata_json);`
- `bool chronicle_user_message(const char *trace_id, const char *span_id, const char *text);`
- `bool chronicle_context_materialized(const char *trace_id, const char *span_id, const char *request_json, int estimated_tokens);`
- `bool chronicle_llm_request(const char *trace_id, const char *span_id, const char *provider, const char *model, const char *request_json, int estimated_tokens);`
- `bool chronicle_llm_delta(const char *trace_id, const char *span_id, const char *kind, const char *text);`
- `bool chronicle_llm_response(const char *trace_id, const char *span_id, const char *provider, const char *model, const char *output_text, const char *raw_response_json, int input_tokens, int output_tokens, int cache_read_tokens, int cache_write_tokens, int reasoning_tokens, double cost_usd, double latency_ms, const char *finish_reason, const char *generation_id);`
- `bool chronicle_tool_call_start(const char *trace_id, const char *parent_span_id, const char *tool_name, const char *tool_id, const char *args_json, char *tool_span_id_out);`
- `bool chronicle_tool_call_end(const char *trace_id, const char *tool_span_id, const char *tool_name, const char *result_text, bool ok, bool timeout, double latency_ms);`
- `char *chronicle_build_activity_json(int limit, const char *session_filter);`
- `char *chronicle_build_activity_html(int limit, const char *session_filter);`
- `char *chronicle_build_activity_html_ex(int limit, const char *session_filter, const char *type_filter, const char *search_filter);`
- `char *chronicle_read_blob_hex(const char *sha256, size_t max_bytes, const char **content_type_out);`

## `cloud_runtime.h`

Function-like declarations: 6

### Declarations

- `bool dsco_cloud_runtime_requested(int argc, char **argv);`
- `bool dsco_cloud_runtime_init(int argc, char **argv, char *err, size_t err_len);`
- `bool dsco_cloud_runtime_active(void);`
- `const char *dsco_cloud_lease_id(void);`
- `bool dsco_cloud_runtime_apply_compiled_ceilings(char *err, size_t err_len);`
- `bool dsco_cloud_destination_allowed(const char *url_or_host);`

## `cluster.h`

Function-like declarations: 2

### Declarations

- `int dsco_cluster_cli(int argc, char **argv);`
- `int dsco_cluster_rpc_endpoints(const char *peers_csv, char *out, size_t outlen, int ensure);`

## `codex_app_directory.h`

Function-like declarations: 6

### Declarations

- `void codex_app_directory_init(codex_app_directory_t *dir);`
- `void codex_app_directory_free(codex_app_directory_t *dir);`
- `bool codex_app_directory_load_file(codex_app_directory_t *dir, const char *path, char *err, size_t err_len);`
- `const codex_app_directory_entry_t *codex_app_directory_find(const codex_app_directory_t *dir, const char *id_or_name);`
- `unsigned codex_app_directory_actions_for_labels(bool retrievable, bool sync, bool consequential, bool interactive);`
- `const char *codex_app_directory_default_path(char *buf, size_t buf_len);`

## `codex_cache.h`

Function-like declarations: 10

### Declarations

- `typedef void (*codex_model_cb)(const codex_model_view_t *m, void *ud);`
- `void codex_cache_init(void);`
- `int codex_cache_count(void);`
- `int codex_cache_load_sync(void);`
- `int codex_cache_wait_ready(int timeout_ms);`
- `int codex_cache_foreach(codex_model_cb cb, void *ud);`
- `const model_info_t *codex_cache_lookup(const char *name);`
- `const char *codex_cache_default_model(void);`
- `const char *codex_cache_default_effort(const char *model);`
- `bool codex_cache_model_supported(const char *model);`

## `codex_usage.h`

Function-like declarations: 2

### Declarations

- `bool codex_usage_parse_jsonl(const char *jsonl, codex_usage_snapshot_t *out, char *error, size_t error_len);`
- `bool codex_usage_fetch(codex_usage_snapshot_t *out, char *error, size_t error_len);`

## `command_plane.h`

Function-like declarations: 9

### Declarations

- `int command_plane_open(command_plane_t *cp, const char *path);`
- `void command_plane_close(command_plane_t *cp);`
- `command_plane_begin_t command_plane_begin(command_plane_t *cp, command_record_t *out, const char *kind, const char *idempotency_key, const char *request_hash);`
- `int command_plane_get_by_id(command_plane_t *cp, const char *command_id, command_record_t *out);`
- `int command_plane_get_by_idempotency_key(command_plane_t *cp, const char *idempotency_key, command_record_t *out);`
- `int command_plane_complete(command_plane_t *cp, const char *command_id, const char *result_json);`
- `int command_plane_fail(command_plane_t *cp, const char *command_id, const char *error);`
- `const char *command_plane_status_name(command_plane_status_t status);`
- `command_plane_status_t command_plane_status_from_name(const char *status);`

## `compositor_parity.h`

Function-like declarations: 1

### Declarations

- `int compositor_parity_write(const char *directory, FILE *summary);`

## `compositor_stream_bench.h`

Function-like declarations: 3

### Declarations

- `void compositor_stream_bench_config_default(compositor_stream_bench_config_t *config);`
- `int compositor_stream_bench_run(FILE *summary, const compositor_stream_bench_config_t *config);`
- `bool compositor_stream_bench_write_json( FILE *out, const compositor_stream_bench_config_t *config, size_t source_bytes, double elapsed_ms, const pixel_tui_perf_snapshot_t *perf);`

## `compute.h`

Function-like declarations: 2

### Declarations

- `void compute_register_backends(void);`
- `int dsco_compute_cli(int argc, char **argv);`

## `config.h`

Function-like declarations: 27

### Declarations

- `const char *v = getenv("DSCO_SWARM_DEFAULT_MINI");`
- `} const char *dsco_secret(const char *key);`
- `static inline int dsco_max_tokens(void) { return dsco_env_int("DSCO_MAX_TOKENS", MAX_TOKENS, 1, 100000);`
- `} static inline int dsco_max_agent_turns(void) { return dsco_env_int("DSCO_MAX_AGENT_TURNS", MAX_AGENT_TURNS, 1, 999999);`
- `} static inline int dsco_hard_turn_ceiling(void) { return dsco_env_int("DSCO_HARD_TURN_CEILING", HARD_TURN_CEILING, 1, 100000000);`
- `const model_info_t *openrouter_cache_lookup(const char *slug);`
- `const model_info_t *codex_cache_lookup(const char *name);`
- `const char *slash = strchr(src, '/');`
- `if (slash && slash < p) slash = strchr(p, '/');`
- `dst[out++] = (char)tolower(c);`
- `model_normalize_key(name, want_norm, sizeof(want_norm));`
- `model_normalize_key(MODEL_REGISTRY[i].alias, alias_norm, sizeof(alias_norm));`
- `model_normalize_key(MODEL_REGISTRY[i].model_id, model_norm, sizeof(model_norm));`
- `} { size_t nl = strlen(name);`
- `memcpy(base, name, blen);`
- `model_normalize_key(base, base_norm, sizeof(base_norm));`
- `model_normalize_key(MODEL_REGISTRY[i].alias, alias_norm, sizeof(alias_norm));`
- `model_normalize_key(MODEL_REGISTRY[i].model_id, model_norm, sizeof(model_norm));`
- `} } } } const model_info_t *codex_model = codex_cache_lookup(name);`
- `return openrouter_cache_lookup(name);`
- `} static inline const model_info_t *model_lookup_priced(const char *name, model_info_t *storage) { const model_info_t *base = model_lookup(name);`
- `} const model_info_t *m = model_lookup(name);`
- `} static inline int model_context_window(const char *name) { const model_info_t *m = model_lookup(name);`
- `} static inline bool dsco_effort_is_wire_value(const char *effort) { return effort && (strcmp(effort, EFFORT_NONE) == 0 || strcmp(effort, EFFORT_MINIMAL) == 0 || strcmp(effort, EFFORT_LOW) == 0 || strcmp(effort, EFFORT_MEDIUM) == 0 || strcmp(effort, EFFORT_HIGH) == 0 || strcmp(effort, EFFORT_XHIGH) == 0);`
- `} static inline bool dsco_effort_is_valid(const char *effort) { return dsco_effort_is_auto(effort) || dsco_effort_is_wire_value(effort) || (effort && strcmp(effort, EFFORT_MAX) == 0);`
- `} else { strncpy(dst, effort, dst_len - 1);`
- `static inline void tui_features_init(tui_features_t *f) { memset(f, 0, sizeof(*f));`

## `connector.h`

Function-like declarations: 22

### Declarations

- `void conn_result_free(conn_result_t *r);`
- `typedef bool (*conn_chunk_cb)(const char *chunk, void *ctx);`
- `void *(*open)(const char *config_json, char *err, size_t errlen);`
- `void (*invoke)(void *self, const char *method, const char *params_json, conn_result_t *out);`
- `void (*stream)(void *self, const char *method, const char *params_json, conn_chunk_cb on_chunk, void *ctx, conn_result_t *out);`
- `char *(*describe)(void *self);`
- `char *(*schema)(void *self, const char *method);`
- `void (*close)(void *self);`
- `int connector_register(const connector_vtable_t *vt);`
- `const connector_vtable_t *connector_find(const char *kind);`
- `int connector_list(const connector_vtable_t **out, int max);`
- `void connector_register_builtins(void);`
- `connector_t *connector_open(const char *kind, const char *config_json, char *err, size_t errlen);`
- `void connector_invoke(connector_t *c, const char *method, const char *params_json, conn_result_t *out);`
- `void connector_stream(connector_t *c, const char *method, const char *params_json, conn_chunk_cb on_chunk, void *ctx, conn_result_t *out);`
- `char *connector_describe(connector_t *c);`
- `unsigned connector_capabilities(const connector_t *c);`
- `void connector_close(connector_t *c);`
- `char *connector_schema(connector_t *c, const char *method);`
- `int connector_validate(connector_t *c, const char *method, const char *params_json, char *err, size_t errlen);`
- `void connector_set_validate(int on);`
- `int connector_cli(int argc, char **argv);`

## `construct.h`

Function-like declarations: 6

### Declarations

- `void construct_start(void);`
- `void construct_stop(void);`
- `int construct_tick(void);`
- `void construct_protect(const char *tool_name, construct_priority_t prio, int renew_quantum_s, int low_water_s, int max_lifetime_s);`
- `void construct_unprotect(const char *tool_name);`
- `void construct_register_tool(void);`

## `context_fabric.h`

Function-like declarations: 17

### Declarations

- `int ctxkey_format(const ctxkey_t *key, char *out, size_t cap);`
- `bool ctxkey_parse(const char *s, ctxkey_t *out);`
- `const char *ctx_kind_name(ctx_kind_t kind);`
- `ctx_broker_t *ctx_broker_open(const char *db_path);`
- `void ctx_broker_close(ctx_broker_t *b);`
- `ctx_broker_t *ctx_broker_default(void);`
- `ctx_broker_t *ctx_broker_open_vfs(struct vfs_db *vfs);`
- `void ctx_broker_set_shared_vfs(struct vfs_db *vfs);`
- `bool ctx_put(ctx_broker_t *b, const void *bytes, size_t n, const ctx_put_opts_t *opts, ctxkey_t *out_key, bool *out_deduped);`
- `char *ctx_get(ctx_broker_t *b, const ctxkey_t *key, size_t *out_len);`
- `int ctx_search(ctx_broker_t *b, const char *query, int kind_filter, int max, ctx_search_hit_t *out);`
- `bool ctx_pin(ctx_broker_t *b, const ctxkey_t *key);`
- `bool ctx_unpin(ctx_broker_t *b, const ctxkey_t *key);`
- `int ctx_pinned_list(ctx_broker_t *b, ctxkey_t *out, int max);`
- `bool ctx_scope_create(ctx_broker_t *b, const ctxkey_t *keys, int n, ctxkey_t *out_scope);`
- `int ctx_scope_resolve(ctx_broker_t *b, const ctxkey_t *scope, ctxkey_t *out, int max);`
- `ctx_stat_t ctx_broker_stat(ctx_broker_t *b);`

## `control_flow.h`

Function-like declarations: 7

### Declarations

- `bool condition_parse(const char *expr_str, condition_t *cond_out);`
- `bool condition_evaluate(const condition_t *cond, const char *ctx_json);`
- `plan_status_t step_execute_atoms(int step_id);`
- `plan_status_t execute_step_with_control( int step_id, const control_flow_t *cf, const char *state_json);`
- `bool step_set_control_flow(int step_id, const control_flow_t *cf);`
- `const control_flow_t *step_get_control_flow(int step_id);`
- `const char *control_type_name(control_type_t t);`

## `coroutine.h`

Function-like declarations: 2

### Declarations

- `typedef int (*coro_func_t)(void *ctx);`
- `int ret = slot->func(slot->ctx);`

## `cost_budget.h`

Function-like declarations: 3

### Declarations

- `const model_info_t *mi = model_lookup(session->model);`
- `s.session_spent_usd = cost_budget_session_spent_usd(session);`
- `s.exhausted = (s.session_limited && s.session_remaining_usd <= 0.0) || (s.daily_limited && s.daily_remaining_usd <= 0.0);`

## `cost_model.h`

Function-like declarations: 7

### Declarations

- `void cost_model_init(void);`
- `void cost_model_learn(const char *topology_name, int total_tokens, double actual_cost_usd, double actual_latency_s);`
- `double cost_model_predict(const char *topology_name, int input_tokens, int output_tokens);`
- `double cost_model_predict_latency(const char *topology_name);`
- `int cost_model_stats_json(char *buf, size_t buflen);`
- `void cost_model_flush(void);`
- `bool cost_model_predict_full(const char *topology_name, int input_tokens, int output_tokens, cost_prediction_t *out);`

## `crypto.h`

Function-like declarations: 25

### Declarations

- `void sha256_init(sha256_ctx_t *ctx);`
- `void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);`
- `void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]);`
- `void sha256_hex(const uint8_t *data, size_t len, char hex[65]);`
- `void md5_init(md5_ctx_t *ctx);`
- `void md5_update(md5_ctx_t *ctx, const uint8_t *data, size_t len);`
- `void md5_final(md5_ctx_t *ctx, uint8_t hash[16]);`
- `void md5_hex(const uint8_t *data, size_t len, char hex[33]);`
- `void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[32]);`
- `void hmac_sha256_hex(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, char hex[65]);`
- `bool hmac_sha256_verify(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, const uint8_t expected_mac[32]);`
- `size_t base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len);`
- `size_t base64_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len);`
- `size_t base64url_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len);`
- `size_t base64url_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len);`
- `void hex_encode(const uint8_t *src, size_t len, char *dst);`
- `size_t hex_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len);`
- `void uuid_v4(char uuid[37]);`
- `bool crypto_random_bytes(uint8_t *buf, size_t len);`
- `void crypto_random_hex(size_t nbytes, char *hex);`
- `void hkdf_sha256(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len, const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len);`
- `bool jwt_decode(const char *token, char *header, size_t h_len, char *payload, size_t p_len);`
- `bool crypto_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);`
- `t = (t ^ (t >> 15)) * (t | 1u);`
- `t ^= t + (t ^ (t >> 7)) * (t | 61u);`

## `cstring_key.gen.h`

Function-like declarations: 0

### Declarations


## `dcr.h`

Function-like declarations: 18

### Declarations

- `void dcr_init(void);`
- `void dcr_reload(void);`
- `bool dcr_is_loaded(void);`
- `size_t dcr_provider_count(void);`
- `const dcr_provider_t *dcr_provider_at(size_t idx);`
- `const dcr_provider_t *dcr_provider_find(const char *name_or_alias);`
- `const provider_profile_t *dcr_provider_profile_find(const char *name_or_alias);`
- `const char *dcr_provider_primary_env_var(const dcr_provider_t *p);`
- `bool dcr_provider_has_env_var(const dcr_provider_t *p, const char *env_var);`
- `const char *dcr_provider_wire_api(const char *provider);`
- `long dcr_provider_stream_idle_timeout_ms(const char *provider, long fallback_ms);`
- `int dcr_provider_stream_max_retries(const char *provider, int fallback);`
- `int dcr_provider_request_max_retries(const char *provider, int fallback);`
- `size_t dcr_model_count(void);`
- `const dcr_model_t *dcr_model_at(size_t idx);`
- `const dcr_model_t *dcr_model_find(const char *id_or_alias);`
- `const char *dcr_reasoning_effort_normalize(const char *provider, const char *model, const char *effort, char *out, size_t out_len);`
- `int dcr_cli(int argc, char **argv);`

## `dist_logo.h`

Function-like declarations: 0

### Declarations


## `dsco_accel.h`

Function-like declarations: 14

### Declarations

- `int dsco_accel_init(void);`
- `void dsco_accel_shutdown(void);`
- `const dsco_accel_info_t *dsco_accel_info(void);`
- `float dsco_accel_dot(const float *a, const float *b, int dim);`
- `float dsco_accel_l2norm(const float *a, int dim);`
- `float dsco_accel_cosine(const float *a, const float *b, int dim);`
- `int dsco_accel_cosine_batch(const float *q, int dim, const float *cands, int n, float *out);`
- `int dsco_accel_gemv(const float *A, int m, int n, const float *x, float *y);`
- `float dsco_accel_softmax(float *x, int n);`
- `void dsco_accel_log_softmax(float *x, int n);`
- `int dsco_accel_topk(const float *x, int n, int k, int *out_idx, float *out_val);`
- `int dsco_accel_argmax(const float *x, int n);`
- `int dsco_accel_dequant_int8(const int8_t *src, const float *scales, int n, int dim, float *out);`
- `int dsco_accel_quant_int8(const float *src, int n, int dim, float *scales_out, int8_t *dst);`

## `dsco_crypto.h`

Function-like declarations: 22

### Declarations

- `extern "C" { int dsco_crypto_init(void);`
- `bool dsco_crypto_has_pq(void);`
- `bool dsco_crypto_aead_is_aegis(void);`
- `const char *dsco_crypto_suite_id(void);`
- `void dsco_random_bytes(void *buf, size_t len);`
- `void dsco_memzero(void *p, size_t n);`
- `int dsco_memcmp_ct(const void *a, const void *b, size_t n);`
- `int dsco_mlock(void *p, size_t n);`
- `int dsco_munlock(void *p, size_t n);`
- `int dsco_hkdf_sha512(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len, const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len);`
- `int dsco_kdf_subkey(const uint8_t root[32], const char *label, uint8_t out[32]);`
- `int dsco_pwhash_argon2id(const uint8_t *password, size_t pw_len, const uint8_t salt[16], uint8_t *out, size_t out_len);`
- `void dsco_hash(const uint8_t *msg, size_t msg_len, uint8_t out[DSCO_HASH_BYTES]);`
- `void dsco_keyed_mac(const uint8_t key[32], const uint8_t *msg, size_t msg_len, uint8_t out[DSCO_HASH_BYTES]);`
- `long dsco_aead_seal(const uint8_t key[DSCO_AEAD_KEY_BYTES], const uint8_t nonce[DSCO_AEAD_NONCE_BYTES], const uint8_t *aad, size_t aad_len, const uint8_t *plaintext, size_t pt_len, uint8_t *ciphertext, size_t ct_cap);`
- `long dsco_aead_open(const uint8_t key[DSCO_AEAD_KEY_BYTES], const uint8_t nonce[DSCO_AEAD_NONCE_BYTES], const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext, size_t ct_len, uint8_t *plaintext, size_t pt_cap);`
- `int dsco_identity_keygen(uint8_t pk_hybrid[DSCO_HYBRID_PK_BYTES], uint8_t sk_ed25519[DSCO_ED25519_SK_BYTES], uint8_t sk_mldsa[DSCO_MLDSA_SK_BYTES]);`
- `long dsco_identity_sign(const uint8_t sk_ed25519[DSCO_ED25519_SK_BYTES], const uint8_t sk_mldsa[DSCO_MLDSA_SK_BYTES], const uint8_t *msg, size_t msg_len, uint8_t sig_out[DSCO_HYBRID_SIG_BYTES]);`
- `int dsco_identity_verify(const uint8_t pk_hybrid[DSCO_HYBRID_PK_BYTES], const uint8_t *msg, size_t msg_len, const uint8_t *sig, size_t sig_len);`
- `int dsco_kem_keygen(uint8_t pk_hybrid[DSCO_HYBRID_KEM_PK_BYTES], uint8_t sk_x25519[DSCO_X25519_SK_BYTES], uint8_t sk_mlkem[DSCO_MLKEM_SK_BYTES]);`
- `int dsco_kem_encap(const uint8_t pk_hybrid[DSCO_HYBRID_KEM_PK_BYTES], uint8_t ct_hybrid[DSCO_HYBRID_KEM_CT_BYTES], uint8_t ss_out[DSCO_HYBRID_KEM_SS_BYTES]);`
- `int dsco_kem_decap(const uint8_t sk_x25519[DSCO_X25519_SK_BYTES], const uint8_t sk_mlkem[DSCO_MLKEM_SK_BYTES], const uint8_t ct_hybrid[DSCO_HYBRID_KEM_CT_BYTES], uint8_t ss_out[DSCO_HYBRID_KEM_SS_BYTES]);`

## `dsco_dht.h`

Function-like declarations: 11

### Declarations

- `dsco_dht_t *dsco_dht_start(const dsco_dht_config_t *cfg);`
- `bool dsco_dht_bootstrap(dsco_dht_t *d, const char *host, uint16_t port);`
- `void dsco_dht_find_peers(dsco_dht_t *d);`
- `void dsco_dht_get_stats(dsco_dht_t *d, dsco_dht_stats_t *out);`
- `void dsco_dht_stop(dsco_dht_t *d);`
- `dsco_dht_t *dsco_dht_global(void);`
- `dsco_dht_kbuckets_t *dsco_dht_kbuckets_create(const uint8_t self_id[20], int k);`
- `void dsco_dht_kbuckets_destroy(dsco_dht_kbuckets_t *kb);`
- `bool dsco_dht_kbuckets_touch(dsco_dht_kbuckets_t *kb, const uint8_t node_id[20], const char *addr, uint64_t now_tick);`
- `int dsco_dht_kbuckets_bucket_size(dsco_dht_kbuckets_t *kb, const uint8_t node_id[20]);`
- `int dsco_dht_kbuckets_closest(dsco_dht_kbuckets_t *kb, const uint8_t target[20], uint8_t out_ids[][20], int max);`

## `dsco_mlx.h`

Function-like declarations: 10

### Declarations

- `int dsco_mlx_init(void);`
- `void dsco_mlx_shutdown(void);`
- `bool dsco_mlx_is_available(void);`
- `const char *dsco_mlx_library_path(void);`
- `const char *dsco_mlx_version(void);`
- `int dsco_mlx_cosine_batch(const float *q, int dim, const float *cands, int n, float *out);`
- `int dsco_mlx_matmul(const float *A, int m, int n, const float *B, int k, float *C);`
- `int dsco_mlx_softmax(float *x, int batch, int n);`
- `int dsco_mlx_argmax(const float *x, int batch, int n, int *idx);`
- `int dsco_mlx_l2norm_rows(float *x, int n, int dim);`

## `dsco_pool.h`

Function-like declarations: 8

### Declarations

- `typedef void (*dsco_pool_iter_fn)(size_t i, void *ctx);`
- `typedef void (*dsco_pool_work_fn)(void *ctx);`
- `int dsco_pool_global_init(int nthreads);`
- `void dsco_pool_global_shutdown(void);`
- `void dsco_pool_apply(size_t n, void *ctx, dsco_pool_iter_fn fn);`
- `dsco_task_t *dsco_pool_submit(dsco_pool_work_fn fn, void *ctx);`
- `void dsco_pool_join(dsco_task_t *t);`
- `int dsco_pool_worker_count(void);`

## `dsco_swim.h`

Function-like declarations: 14

### Declarations

- `typedef uint64_t (*dsco_swim_now_fn)(void *ctx);`
- `dsco_swim_t *dsco_swim_create(const dsco_swim_config_t *cfg);`
- `void dsco_swim_destroy(dsco_swim_t *swim);`
- `bool dsco_swim_upsert(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN], dsco_swim_state_t state, uint64_t incarnation);`
- `bool dsco_swim_mark_suspect(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN], uint64_t incarnation);`
- `bool dsco_swim_refute(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN], uint64_t higher_incarnation);`
- `int dsco_swim_tick(dsco_swim_t *swim);`
- `dsco_swim_state_t dsco_swim_state(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN]);`
- `uint64_t dsco_swim_incarnation(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN]);`
- `int dsco_swim_count(dsco_swim_t *swim);`
- `bool dsco_swim_ping(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]);`
- `bool dsco_swim_ping_req(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]);`
- `bool dsco_swim_ack(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]);`
- `bool dsco_swim_handle_message(dsco_swim_t *swim, const uint8_t from[MESH_PUBKEY_LEN], const void *data, size_t len);`

## `durable_agents.h`

Function-like declarations: 2

### Declarations

- `void durable_agents_default_db_path(char *out, size_t len);`
- `int durable_agents_cli(int argc, char **argv);`

## `embedded_data_registry.h`

Function-like declarations: 1

### Declarations

- `const unsigned char *embedded_data_get(const char *name, size_t *out_len);`

## `embedded_key.gen.h`

Function-like declarations: 0

### Declarations


## `env_config.h`

Function-like declarations: 8

### Declarations

- `bool dsco_env_truthy_str(const char *v);`
- `bool dsco_env_falsy_str(const char *v);`
- `bool dsco_env_bool(const char *name, bool def);`
- `int dsco_env_int(const char *name, int def, int min_v, int max_v);`
- `long dsco_env_long(const char *name, long def, long min_v, long max_v);`
- `size_t dsco_env_size(const char *name, size_t def, size_t min_v, size_t max_v);`
- `double dsco_env_double(const char *name, double def, double min_v, double max_v);`
- `const char *dsco_env_str(const char *name, const char *def);`

## `env_guard.h`

Function-like declarations: 2

### Declarations

- `uint32_t env_guard_init(void);`
- `bool env_guard_check_dylibs(void);`

## `error.h`

Function-like declarations: 7

### Declarations

- `const dsco_error_t *dsco_err_last(void);`
- `dsco_err_code_t dsco_err_code(void);`
- `const char *dsco_err_msg(void);`
- `void dsco_err_clear(void);`
- `void dsco_err_set(dsco_err_code_t code, const char *file, int line, const char *fmt, ...);`
- `void dsco_err_wrap(dsco_err_code_t code, const char *file, int line, const char *fmt, ...);`
- `const char *dsco_err_code_str(dsco_err_code_t code);`

## `eval.h`

Function-like declarations: 12

### Declarations

- `void eval_init(eval_ctx_t *ctx);`
- `double eval_expr(eval_ctx_t *ctx, const char *expr);`
- `void eval_format(eval_ctx_t *ctx, const char *expr, char *out, size_t out_len);`
- `void eval_multi(eval_ctx_t *ctx, const char *exprs, char *out, size_t out_len);`
- `void eval_set_var(eval_ctx_t *ctx, const char *name, double value);`
- `double eval_get_var(eval_ctx_t *ctx, const char *name);`
- `void bigint_from_str(bigint_t *b, const char *s);`
- `void bigint_to_str(const bigint_t *b, char *s, size_t len);`
- `void bigint_add(const bigint_t *a, const bigint_t *b, bigint_t *result);`
- `void bigint_mul(const bigint_t *a, const bigint_t *b, bigint_t *result);`
- `void bigint_factorial(int n, bigint_t *result);`
- `bool bigint_is_prime(const bigint_t *n);`

## `event_loop.h`

Function-like declarations: 15

### Declarations

- `typedef void (*ev_fd_cb)(int fd, ev_mask_t events, void *ctx);`
- `typedef void (*ev_timer_cb)(int timer_id, void *ctx);`
- `typedef void (*ev_defer_cb)(void *ctx);`
- `ev_loop_t *ev_loop_new(void);`
- `void ev_loop_free(ev_loop_t *loop);`
- `int ev_watch_fd(ev_loop_t *loop, int fd, ev_mask_t mask, ev_fd_cb cb, void *ctx);`
- `int ev_unwatch_fd(ev_loop_t *loop, int fd);`
- `int ev_timer_once(ev_loop_t *loop, int ms, ev_timer_cb cb, void *ctx);`
- `int ev_timer_repeat(ev_loop_t *loop, int interval_ms, ev_timer_cb cb, void *ctx);`
- `void ev_timer_cancel(ev_loop_t *loop, int timer_id);`
- `int ev_defer(ev_loop_t *loop, ev_defer_cb cb, void *ctx);`
- `int ev_loop_poll(ev_loop_t *loop, int timeout_ms);`
- `void ev_loop_run(ev_loop_t *loop);`
- `void ev_loop_stop(ev_loop_t *loop);`
- `ev_stats_t ev_loop_stats(ev_loop_t *loop);`

## `execution_layer.h`

Function-like declarations: 5

### Declarations

- `typedef bool (*execution_leaf_fn)(const char *name, const char *input_json, const char *tier, char *result, size_t result_len);`
- `const char *execution_effect_name(execution_effect_t effect);`
- `const char *execution_status_name(execution_status_t status);`
- `bool execution_submit(const execution_intent_t *intent, execution_receipt_t *receipt, char *result, size_t result_len);`
- `bool execution_last_receipt_json(char *out, size_t out_len);`

## `executive.h`

Function-like declarations: 14

### Declarations

- `typedef struct { void (*request_exit)(const char *reason);`
- `double (*get_session_budget)(void);`
- `void (*set_session_budget)(double usd);`
- `double (*get_session_spent)(void);`
- `void (*force_phase_red)(bool on);`
- `void (*request_model_downshift)(const char *reason);`
- `void (*escalate)(const char *question);`
- `bool (*frontier_snapshot)(double *score, bool *on_frontier, char *summary, size_t summary_len);`
- `void (*audit)(const char *category, const char *title, const char *detail);`
- `void executive_set_hooks(const executive_hooks_t *hooks);`
- `bool executive_decide(const char *input_json, char *result, size_t rlen);`
- `bool tool_executive_decision(const char *input, char *result, size_t rlen);`
- `exec_decision_t executive_parse_decision(const char *name, bool *ok);`
- `const char *executive_decision_name(exec_decision_t d);`
- `bool executive_spending_paused(void);`

## `extension/backend.h`

Function-like declarations: 7

### Declarations

- `int (*init)(void *impl);`
- `void (*shutdown)(void *impl);`
- `const char *(*version)(void *impl);`
- `int backend_register(backend_interface_t *backend);`
- `backend_interface_t *backend_get(const char *name, backend_category_t cat);`
- `int backend_load(const char *name, backend_category_t cat);`
- `int backend_unload(const char *name, backend_category_t cat);`

## `extension/eigen_backend.h`

Function-like declarations: 1

### Declarations

- `int eigen_backend_init(void);`

## `extension/fftw_backend.h`

Function-like declarations: 1

### Declarations

- `int fftw_backend_init(void);`

## `extension/numerical_backend.h`

Function-like declarations: 15

### Declarations

- `int (*matrix_alloc)(size_t rows, size_t cols, gsl_matrix **out);`
- `int (*matrix_free)(gsl_matrix *m);`
- `int (*gemm)(const gsl_matrix *A, const gsl_matrix *B, gsl_matrix *C, double alpha, double beta);`
- `int (*fft_complex_forward)(double *data, size_t n);`
- `double (*mean)(const double *data, size_t n);`
- `double (*variance)(const double *data, size_t n);`
- `double (*correlation)(const double *x, const double *y, size_t n);`
- `double (*gaussian_sample)(double mu, double sigma);`
- `double (*gaussian_pdf)(double x, double mu, double sigma);`
- `double (*gaussian_cdf)(double x, double mu, double sigma);`
- `int (*optimize)(double (*f)(const double *x, size_t n), double *x, size_t n, int max_iter);`
- `double (*bessel_J0)(double x);`
- `double (*gamma)(double x);`
- `double (*erf)(double x);`
- `int numerical_backend_register(numerical_backend_t *impl);`

## `extension/skill_requirements.h`

Function-like declarations: 3

### Declarations

- `int skill_requirements_init(skill_requirements_t *req);`
- `int skill_requirements_add(skill_requirements_t *req, backend_category_t cat, const char *name, int mandatory);`
- `int skill_requirements_satisfy(const skill_requirements_t *req);`

## `face_sdf.h`

Function-like declarations: 6

### Declarations

- `void face_params_default(face_params_t *p);`
- `int face_preset_count(void);`
- `void face_preset(int idx, face_params_t *p, const char **name_out);`
- `double face_sdf_de(double x, double y, double z, const face_params_t *p);`
- `int face_sdf_material(double x, double y, double z, const face_params_t *p);`
- `void face_sdf_albedo(int material, double *r, double *g, double *b);`

## `fingerprint.h`

Function-like declarations: 7

### Declarations

- `const dsco_fingerprint_t *dsco_fingerprint_get(void);`
- `int dsco_fingerprint_refresh(void);`
- `size_t dsco_fingerprint_to_json(const dsco_fingerprint_t *fp, bool include_pii, bool include_all, char *out, size_t out_cap);`
- `size_t dsco_fingerprint_summary(const dsco_fingerprint_t *fp, char *out, size_t out_cap);`
- `size_t dsco_fingerprint_stability_report(const dsco_fingerprint_t *fp, char *out, size_t out_cap);`
- `const char *dsco_hv_name(dsco_hv_type_t hv);`
- `static inline size_t dsco_fingerprint_to_json_compat( const dsco_fingerprint_t *fp, bool include_pii, char *out, size_t out_cap) { return dsco_fingerprint_to_json(fp, include_pii, false, out, out_cap);`

## `font_compat.h`

Function-like declarations: 6

### Declarations

- `int font_compat_draw_rgb(uint8_t *rgb, int width, int height, int stride, int x, int y, int max_width, const char *utf8, float point_size, bool bold, uint8_t red, uint8_t green, uint8_t blue, float opacity);`
- `int font_compat_draw_rgb_styled(uint8_t *rgb, int width, int height, int stride, int x, int y, int max_width, const char *utf8, float point_size, bool bold, bool italic, uint8_t red, uint8_t green, uint8_t blue, float opacity);`
- `int font_compat_measure_utf8(const char *utf8, float point_size, bool bold);`
- `int font_compat_measure_utf8_styled(const char *utf8, float point_size, bool bold, bool italic);`
- `int font_compat_line_height(float point_size, bool bold);`
- `bool font_compat_available(void);`

## `fractal.h`

Function-like declarations: 3

### Declarations

- `bool fractal_is_kind(const char *kind);`
- `int fractal_plot(char *out, size_t cap, const char *json);`
- `int fractal_anim(const char *json);`

## `frontier.h`

Function-like declarations: 5

### Declarations

- `void frontier_init(frontier_ledger_t *l);`
- `frontier_decomp_t frontier_record(frontier_ledger_t *l, const frontier_turn_t *t, const frontier_prices_t *p);`
- `frontier_verdict_t frontier_verdict(const frontier_ledger_t *l);`
- `double frontier_waste_ratio(const frontier_ledger_t *l);`
- `const char *frontier_report(const frontier_ledger_t *l, char *buf, int buf_len);`

## `generated/runtime_spec_contract.h`

Function-like declarations: 0

### Declarations


## `gov_experiment.h`

Function-like declarations: 13

### Declarations

- `gov_model_t gov_experiment_model(void);`
- `const char *gov_model_name(gov_model_t m);`
- `const char *gov_stage_name(gov_stage_t s);`
- `bool gov_experiment_bypass_all(void);`
- `bool gov_stage_active(gov_model_t m, gov_stage_t s);`
- `bool gov_stage_enforces(gov_model_t m, gov_stage_t s);`
- `bool gov_shadow_all_tools(gov_model_t m);`
- `void gov_stage_record(gov_stage_t s, double ms, bool would_deny, bool enforced);`
- `void gov_experiment_note_gate_run(void);`
- `void gov_experiment_note_bypass(void);`
- `void gov_experiment_reset(void);`
- `size_t gov_experiment_report_json(char *buf, size_t len);`
- `void gov_experiment_totals(unsigned long *gate_calls, unsigned long *bypassed, double *gate_ms_total);`

## `governance.h`

Function-like declarations: 30

### Declarations

- `void governance_init(governance_engine_t *g);`
- `void governance_destroy(governance_engine_t *g);`
- `bool governance_register_agent(governance_engine_t *g, const char *agent_id, principal_tier_t tier, double gsu_budget);`
- `bool governance_deregister_agent(governance_engine_t *g, const char *agent_id);`
- `const agent_envelope_t *governance_get_agent(const governance_engine_t *g, const char *agent_id);`
- `capability_tier_t governance_record_outcome(governance_engine_t *g, const char *agent_id, bool success);`
- `capability_tier_t governance_derive_capability(int successes, int failures, double *out_success_lb);`
- `bool governance_can_claim_capability(const governance_engine_t *g, const char *agent_id, capability_tier_t want);`
- `double governance_wilson_lower_bound(int successes, int failures);`
- `bool governance_authorize(governance_engine_t *g, const char *agent_id, const char *action, double gsu_cost);`
- `bool governance_can_do(const governance_engine_t *g, const char *agent_id, const char *action, double gsu_cost);`
- `bool governance_charge_gsu(governance_engine_t *g, const char *agent_id, double amount);`
- `double governance_remaining_gsu(const governance_engine_t *g, const char *agent_id);`
- `bool governance_reset_budget(governance_engine_t *g, const char *agent_id, double new_amount, int requester_tier);`
- `bool governance_check_hardcoded(const governance_engine_t *g, const char *action);`
- `double governance_get_param(const governance_engine_t *g, const char *name);`
- `bool governance_set_param(governance_engine_t *g, const char *name, double value, int requester_tier);`
- `bool governance_checkpoint(governance_engine_t *g, const char *agent_id, const char *action, double gsu_cost, const char *context);`
- `void governance_tick(governance_engine_t *g);`
- `int governance_to_json(const governance_engine_t *g, char *buf, size_t len);`
- `int governance_status_json(const governance_engine_t *g, char *buf, size_t len);`
- `int governance_audit_json(const governance_engine_t *g, char *buf, size_t len, int last_n);`
- `const char *governance_tier_name(principal_tier_t t);`
- `void governance_set_vfs(vfs_db_t *vfs);`
- `bool governance_check_breakers(governance_engine_t *g, const char *agent_id);`
- `bool governance_trip_breaker(governance_engine_t *g, circuit_breaker_type_t type, const char *reason);`
- `bool governance_reset_breaker(governance_engine_t *g, circuit_breaker_type_t type);`
- `void governance_breaker_update(governance_engine_t *g, circuit_breaker_type_t type, double value);`
- `int governance_breakers_json(const governance_engine_t *g, char *buf, size_t len);`
- `bool governance_shadow_check(governance_engine_t *g, const char *agent_id, const char *proposed_action, shadow_check_result_t *result);`

## `graphsub_client.h`

Function-like declarations: 18

### Declarations

- `const char *graphsub_host(void);`
- `uint16_t graphsub_port(void);`
- `const char *graphsub_tenant_id(void);`
- `bool graphsub_is_available(void);`
- `char *graphsub_agent_register(const char *agent_id, const char *model, const char *capabilities_json, const char *tools_json, const char *hostname, const char *version, const char *bridge_peer);`
- `char *graphsub_agent_heartbeat(const char *agent_id, const char *status, double load, int memory_used_mb, int active_agents, int uptime_seconds);`
- `char *graphsub_agent_deregister(const char *agent_id);`
- `char *graphsub_pheromone_deposit(const char *agent_id, const char *target_type, const char *target_id, const char *signal_type, double intensity, int ttl_seconds, const char *metadata_json);`
- `char *graphsub_pheromone_query(const char *target_id, const char *signal_type, double min_intensity);`
- `char *graphsub_pheromone_sweep(const char *target_type, const char *signal_type);`
- `char *graphsub_graph_traverse(const char *start_node, int depth, const char *edge_filter_json, int limit);`
- `char *graphsub_memory_sync(const char *agent_id, const char *episodes_json, const char *type);`
- `char *graphsub_memory_consolidate(const char *agent_id, const char *from_ts, const char *to_ts);`
- `char *graphsub_swarm_register(const char *swarm_id, const char *topology, const char *agents_json, const char *task_desc);`
- `char *graphsub_tool_result(const char *agent_id, const char *tool_name, const char *tool_params_json, const char *result_json, int duration_ms, bool success, const char *executed_on, const char *swarm_id);`
- `char *graphsub_bridge_fleet(const char *peers_json);`
- `char *graphsub_post(const char *path, const char *json_body);`
- `char *graphsub_get(const char *path);`

## `harden.h`

Function-like declarations: 3

### Declarations

- `void dsco_harden_init(void);`
- `bool dsco_harden_checkpoint(void);`
- `bool dsco_harden_enabled(void);`

## `heartbeat.h`

Function-like declarations: 7

### Declarations

- `void heartbeat_start(void);`
- `void heartbeat_set_context(int argc, char **argv);`
- `void heartbeat_set_phase(const char *phase);`
- `void heartbeat_stop(void);`
- `bool heartbeat_running(void);`
- `void heartbeat_poke(void);`
- `void heartbeat_note_event(const char *event, const char *detail);`

## `hlc.h`

Function-like declarations: 10

### Declarations

- `int64_t hlc_phys_now_ms(void);`
- `hlc_t hlc_now(void);`
- `hlc_t hlc_update(hlc_t remote, bool *did_clamp);`
- `int hlc_compare(hlc_t a, hlc_t b);`
- `void hlc_encode(hlc_t ts, char *out, size_t out_len);`
- `bool hlc_decode(const char *s, hlc_t *out);`
- `uint64_t hlc_pack(hlc_t ts);`
- `hlc_t hlc_unpack(uint64_t packed);`
- `hlc_t hlc_peek(void);`
- `void hlc_reset(void);`

## `hotplug.h`

Function-like declarations: 3

### Declarations

- `bool hotplug_scan(void);`
- `int hotplug_rebuild_all(void);`
- `const char *hotplug_src_dir(void);`

## `http_pool.h`

Function-like declarations: 4

### Declarations

- `void dsco_http_global_init(void);`
- `void dsco_http_global_cleanup(void);`
- `void dsco_http_pool_apply(CURL *easy);`
- `void dsco_http_pool_cleanup(void);`

## `img_util.h`

Function-like declarations: 1

### Declarations

- `bool dsco_image_downscale_jpeg(const char *in_path, int max_dim, const char *out_path);`

## `integration_fabric.h`

Function-like declarations: 8

### Declarations

- `const dsco_integration_profile_t *dsco_integration_profiles(size_t *count);`
- `const dsco_integration_profile_t *dsco_integration_profile_for_server(const char *server_name);`
- `const dsco_integration_profile_t *dsco_integration_profile_for_tool(const char *tool_name);`
- `unsigned dsco_integration_actions_for_catalog_labels(bool retrievable, bool sync, bool consequential, bool interactive);`
- `unsigned dsco_integration_actions_for_tool(const char *tool_name);`
- `bool dsco_integration_requires_confirmation(const char *tool_name);`
- `bool dsco_integration_action_has(unsigned actions, dsco_integration_action_t action);`
- `const char *dsco_integration_scope_name(dsco_integration_scope_t scope);`

## `integrations.h`

Function-like declarations: 256

### Declarations

- `bool tool_tavily_search(const char *input, char *result, size_t rlen);`
- `bool tool_brave_search(const char *input, char *result, size_t rlen);`
- `bool tool_serpapi(const char *input, char *result, size_t rlen);`
- `bool tool_jina_read(const char *input, char *result, size_t rlen);`
- `bool tool_jina_search(const char *input, char *result, size_t rlen);`
- `bool tool_jina_embed(const char *input, char *result, size_t rlen);`
- `int jina_embed_batch(char **texts, int n, const char *task, const char *model, int want_dim, float *vecs, int stride, int *out_dim);`
- `bool tool_jina_ai_reader(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_search(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_embed(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_batch_embed_submit(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_batch_status(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_batch_output(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_batch_errors(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_batch_cancel(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_batches_list(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_rerank(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_research(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_live_kb(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_constellation(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_classify(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_train(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_classifiers_list(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_classifier_delete(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_match(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_models_list(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_model_get(const char *input, char *result, size_t rlen);`
- `bool tool_jina_ai_chat(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_search(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_search(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_extract(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_create(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_status(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_result(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_events(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_input(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_group_create(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_group_get(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_group_events(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_group_add_runs(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_group_runs(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_task_group_run_get(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_entity_search(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_ingest(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_create(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_status(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_result(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_cancel(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_events(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_extend(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_enrich(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_findall_schema(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_chat(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_monitor_create(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_monitor_list(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_monitor_get(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_monitor_events(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_monitor_update(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_monitor_trigger(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_monitor_cancel(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_jobs(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_wait(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_research(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_live_kb(const char *input, char *result, size_t rlen);`
- `bool tool_parallel_ai_constellation(const char *input, char *result, size_t rlen);`
- `bool tool_github_search(const char *input, char *result, size_t rlen);`
- `bool tool_github_issue(const char *input, char *result, size_t rlen);`
- `bool tool_github_pr(const char *input, char *result, size_t rlen);`
- `bool tool_github_repo(const char *input, char *result, size_t rlen);`
- `bool tool_github_create_issue(const char *input, char *result, size_t rlen);`
- `bool tool_github_actions(const char *input, char *result, size_t rlen);`
- `bool tool_alpha_vantage(const char *input, char *result, size_t rlen);`
- `bool tool_av_time_series_intraday(const char *input, char *result, size_t rlen);`
- `bool tool_av_time_series_daily(const char *input, char *result, size_t rlen);`
- `bool tool_av_time_series_daily_adj(const char *input, char *result, size_t rlen);`
- `bool tool_av_time_series_weekly(const char *input, char *result, size_t rlen);`
- `bool tool_av_time_series_weekly_adj(const char *input, char *result, size_t rlen);`
- `bool tool_av_time_series_monthly(const char *input, char *result, size_t rlen);`
- `bool tool_av_time_series_monthly_adj(const char *input, char *result, size_t rlen);`
- `bool tool_av_quote(const char *input, char *result, size_t rlen);`
- `bool tool_av_bulk_quotes(const char *input, char *result, size_t rlen);`
- `bool tool_av_search(const char *input, char *result, size_t rlen);`
- `bool tool_av_market_status(const char *input, char *result, size_t rlen);`
- `bool tool_av_realtime_options(const char *input, char *result, size_t rlen);`
- `bool tool_av_historical_options(const char *input, char *result, size_t rlen);`
- `bool tool_av_news(const char *input, char *result, size_t rlen);`
- `bool tool_av_overview(const char *input, char *result, size_t rlen);`
- `bool tool_av_etf(const char *input, char *result, size_t rlen);`
- `bool tool_av_income(const char *input, char *result, size_t rlen);`
- `bool tool_av_balance(const char *input, char *result, size_t rlen);`
- `bool tool_av_cashflow(const char *input, char *result, size_t rlen);`
- `bool tool_av_earnings(const char *input, char *result, size_t rlen);`
- `bool tool_av_earnings_estimates(const char *input, char *result, size_t rlen);`
- `bool tool_av_dividends(const char *input, char *result, size_t rlen);`
- `bool tool_av_splits(const char *input, char *result, size_t rlen);`
- `bool tool_av_insider(const char *input, char *result, size_t rlen);`
- `bool tool_av_institutional(const char *input, char *result, size_t rlen);`
- `bool tool_av_transcript(const char *input, char *result, size_t rlen);`
- `bool tool_av_movers(const char *input, char *result, size_t rlen);`
- `bool tool_av_listing_status(const char *input, char *result, size_t rlen);`
- `bool tool_av_earnings_calendar(const char *input, char *result, size_t rlen);`
- `bool tool_av_ipo_calendar(const char *input, char *result, size_t rlen);`
- `bool tool_av_analytics_fixed(const char *input, char *result, size_t rlen);`
- `bool tool_av_analytics_sliding(const char *input, char *result, size_t rlen);`
- `bool tool_av_forex(const char *input, char *result, size_t rlen);`
- `bool tool_av_fx_intraday(const char *input, char *result, size_t rlen);`
- `bool tool_av_fx_daily(const char *input, char *result, size_t rlen);`
- `bool tool_av_fx_weekly(const char *input, char *result, size_t rlen);`
- `bool tool_av_fx_monthly(const char *input, char *result, size_t rlen);`
- `bool tool_av_crypto(const char *input, char *result, size_t rlen);`
- `bool tool_av_crypto_intraday(const char *input, char *result, size_t rlen);`
- `bool tool_av_crypto_weekly(const char *input, char *result, size_t rlen);`
- `bool tool_av_crypto_monthly(const char *input, char *result, size_t rlen);`
- `bool tool_av_wti(const char *input, char *result, size_t rlen);`
- `bool tool_av_brent(const char *input, char *result, size_t rlen);`
- `bool tool_av_natural_gas(const char *input, char *result, size_t rlen);`
- `bool tool_av_copper(const char *input, char *result, size_t rlen);`
- `bool tool_av_aluminum(const char *input, char *result, size_t rlen);`
- `bool tool_av_wheat(const char *input, char *result, size_t rlen);`
- `bool tool_av_corn(const char *input, char *result, size_t rlen);`
- `bool tool_av_cotton(const char *input, char *result, size_t rlen);`
- `bool tool_av_sugar(const char *input, char *result, size_t rlen);`
- `bool tool_av_coffee(const char *input, char *result, size_t rlen);`
- `bool tool_av_all_commodities(const char *input, char *result, size_t rlen);`
- `bool tool_av_gold_spot(const char *input, char *result, size_t rlen);`
- `bool tool_av_gold_history(const char *input, char *result, size_t rlen);`
- `bool tool_av_real_gdp(const char *input, char *result, size_t rlen);`
- `bool tool_av_real_gdp_per_capita(const char *input, char *result, size_t rlen);`
- `bool tool_av_treasury_yield(const char *input, char *result, size_t rlen);`
- `bool tool_av_federal_funds_rate(const char *input, char *result, size_t rlen);`
- `bool tool_av_cpi(const char *input, char *result, size_t rlen);`
- `bool tool_av_inflation(const char *input, char *result, size_t rlen);`
- `bool tool_av_retail_sales(const char *input, char *result, size_t rlen);`
- `bool tool_av_durables(const char *input, char *result, size_t rlen);`
- `bool tool_av_unemployment(const char *input, char *result, size_t rlen);`
- `bool tool_av_nonfarm_payroll(const char *input, char *result, size_t rlen);`
- `bool tool_av_sma(const char *input, char *result, size_t rlen);`
- `bool tool_av_ema(const char *input, char *result, size_t rlen);`
- `bool tool_av_wma(const char *input, char *result, size_t rlen);`
- `bool tool_av_dema(const char *input, char *result, size_t rlen);`
- `bool tool_av_tema(const char *input, char *result, size_t rlen);`
- `bool tool_av_trima(const char *input, char *result, size_t rlen);`
- `bool tool_av_kama(const char *input, char *result, size_t rlen);`
- `bool tool_av_mama(const char *input, char *result, size_t rlen);`
- `bool tool_av_vwap(const char *input, char *result, size_t rlen);`
- `bool tool_av_t3(const char *input, char *result, size_t rlen);`
- `bool tool_av_macd(const char *input, char *result, size_t rlen);`
- `bool tool_av_macdext(const char *input, char *result, size_t rlen);`
- `bool tool_av_stoch(const char *input, char *result, size_t rlen);`
- `bool tool_av_stochf(const char *input, char *result, size_t rlen);`
- `bool tool_av_rsi(const char *input, char *result, size_t rlen);`
- `bool tool_av_stochrsi(const char *input, char *result, size_t rlen);`
- `bool tool_av_willr(const char *input, char *result, size_t rlen);`
- `bool tool_av_adx(const char *input, char *result, size_t rlen);`
- `bool tool_av_adxr(const char *input, char *result, size_t rlen);`
- `bool tool_av_apo(const char *input, char *result, size_t rlen);`
- `bool tool_av_ppo(const char *input, char *result, size_t rlen);`
- `bool tool_av_mom(const char *input, char *result, size_t rlen);`
- `bool tool_av_bop(const char *input, char *result, size_t rlen);`
- `bool tool_av_cci(const char *input, char *result, size_t rlen);`
- `bool tool_av_cmo(const char *input, char *result, size_t rlen);`
- `bool tool_av_roc(const char *input, char *result, size_t rlen);`
- `bool tool_av_rocr(const char *input, char *result, size_t rlen);`
- `bool tool_av_aroon(const char *input, char *result, size_t rlen);`
- `bool tool_av_aroonosc(const char *input, char *result, size_t rlen);`
- `bool tool_av_mfi(const char *input, char *result, size_t rlen);`
- `bool tool_av_trix_ind(const char *input, char *result, size_t rlen);`
- `bool tool_av_ultosc(const char *input, char *result, size_t rlen);`
- `bool tool_av_dx(const char *input, char *result, size_t rlen);`
- `bool tool_av_minus_di(const char *input, char *result, size_t rlen);`
- `bool tool_av_plus_di(const char *input, char *result, size_t rlen);`
- `bool tool_av_minus_dm(const char *input, char *result, size_t rlen);`
- `bool tool_av_plus_dm(const char *input, char *result, size_t rlen);`
- `bool tool_av_bbands(const char *input, char *result, size_t rlen);`
- `bool tool_av_midpoint(const char *input, char *result, size_t rlen);`
- `bool tool_av_midprice(const char *input, char *result, size_t rlen);`
- `bool tool_av_sar(const char *input, char *result, size_t rlen);`
- `bool tool_av_trange(const char *input, char *result, size_t rlen);`
- `bool tool_av_atr(const char *input, char *result, size_t rlen);`
- `bool tool_av_natr(const char *input, char *result, size_t rlen);`
- `bool tool_av_ad_line(const char *input, char *result, size_t rlen);`
- `bool tool_av_adosc(const char *input, char *result, size_t rlen);`
- `bool tool_av_obv(const char *input, char *result, size_t rlen);`
- `bool tool_av_ht_trendline(const char *input, char *result, size_t rlen);`
- `bool tool_av_ht_sine(const char *input, char *result, size_t rlen);`
- `bool tool_av_ht_trendmode(const char *input, char *result, size_t rlen);`
- `bool tool_av_ht_dcperiod(const char *input, char *result, size_t rlen);`
- `bool tool_av_ht_dcphase(const char *input, char *result, size_t rlen);`
- `bool tool_av_ht_phasor(const char *input, char *result, size_t rlen);`
- `bool tool_fred_series(const char *input, char *result, size_t rlen);`
- `bool tool_slack_post(const char *input, char *result, size_t rlen);`
- `bool tool_discord_post(const char *input, char *result, size_t rlen);`
- `bool tool_twilio_sms(const char *input, char *result, size_t rlen);`
- `bool tool_notion_search(const char *input, char *result, size_t rlen);`
- `bool tool_notion_page(const char *input, char *result, size_t rlen);`
- `bool tool_weather(const char *input, char *result, size_t rlen);`
- `bool tool_mapbox_geocode(const char *input, char *result, size_t rlen);`
- `bool tool_firecrawl(const char *input, char *result, size_t rlen);`
- `bool tool_elevenlabs_tts(const char *input, char *result, size_t rlen);`
- `bool tool_pinecone_query(const char *input, char *result, size_t rlen);`
- `bool tool_stripe(const char *input, char *result, size_t rlen);`
- `bool tool_supabase_query(const char *input, char *result, size_t rlen);`
- `bool tool_huggingface(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_markets(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_events(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_categories(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_prices(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_book(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_trades(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_search(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_events(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_markets(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_orderbook(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_trades(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_series(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_search(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_candlesticks(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_weather(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_market_snapshot(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_event_detail(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_daily_markets(const char *input, char *result, size_t rlen);`
- `bool tool_cross_platform_delta(const char *input, char *result, size_t rlen);`
- `bool tool_market_movers(const char *input, char *result, size_t rlen);`
- `bool tool_market_cache_refresh(const char *input, char *result, size_t rlen);`
- `bool tool_market_cache_query(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_historical_markets(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_historical_trades(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_historical_cutoff(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_resolved(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_resolved_events(const char *input, char *result, size_t rlen);`
- `bool tool_historical_cross_platform(const char *input, char *result, size_t rlen);`
- `bool tool_systematic_ingest_polymarket(const char *input, char *result, size_t rlen);`
- `bool tool_systematic_ingest_kalshi(const char *input, char *result, size_t rlen);`
- `bool tool_systematic_analytics(const char *input, char *result, size_t rlen);`
- `bool tool_systematic_signals(const char *input, char *result, size_t rlen);`
- `bool tool_strat_completeness(const char *input, char *result, size_t rlen);`
- `bool tool_strat_binary_fade(const char *input, char *result, size_t rlen);`
- `bool tool_strat_stale_snipe(const char *input, char *result, size_t rlen);`
- `bool tool_strat_kelly(const char *input, char *result, size_t rlen);`
- `bool tool_strat_spread_scan(const char *input, char *result, size_t rlen);`
- `bool tool_kb_ingest(const char *input, char *result, size_t rlen);`
- `bool tool_kb_search(const char *input, char *result, size_t rlen);`
- `bool tool_kb_list(const char *input, char *result, size_t rlen);`
- `bool tool_kb_get(const char *input, char *result, size_t rlen);`
- `bool tool_kb_delete(const char *input, char *result, size_t rlen);`
- `bool tool_arxiv_search(const char *input, char *result, size_t rlen);`
- `bool tool_arxiv_ingest(const char *input, char *result, size_t rlen);`
- `bool tool_kb_deep_search(const char *input, char *result, size_t rlen);`
- `bool tool_prediction_scan(const char *input, char *result, size_t rlen);`
- `bool tool_prediction_weather(const char *input, char *result, size_t rlen);`
- `bool tool_prediction_snapshot(const char *input, char *result, size_t rlen);`
- `bool tool_prediction_arb(const char *input, char *result, size_t rlen);`
- `bool tool_prediction_semantic_match(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_whale_trades(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_leaderboard(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_history(const char *input, char *result, size_t rlen);`

## `introspect.h`

Function-like declarations: 2

### Declarations

- `int introspect_print_codebase_stats(FILE *out);`
- `int introspect_run_selves(FILE *out, int n, const char *prompt);`

## `ipc.h`

Function-like declarations: 42

### Declarations

- `static inline int dsco_ipc_heartbeat_sec(void) { return dsco_env_int("DSCO_IPC_HEARTBEAT_SEC", IPC_HEARTBEAT_SEC, 1, 3600);`
- `} static inline int dsco_ipc_stale_sec(void) { return dsco_env_int("DSCO_IPC_STALE_SEC", IPC_STALE_SEC, 2, 86400);`
- `} static inline size_t dsco_ipc_max_body(void) { return dsco_env_size("DSCO_IPC_MAX_BODY", IPC_MAX_BODY, 1024, IPC_MAX_BODY);`
- `bool ipc_init(const char *db_path, const char *agent_id);`
- `void ipc_shutdown(void);`
- `const char *ipc_self_id(void);`
- `const char *ipc_db_path(void);`
- `void ipc_set_event_loop(ev_loop_t *loop);`
- `bool ipc_register(const char *parent_id, int depth, const char *role, const char *toolkit);`
- `bool ipc_agent_define(const char *agent_id, const char *parent_id, int depth, const char *role, const char *model, const char *toolkit);`
- `bool ipc_agent_define_bound(const char *agent_id, const char *parent_id, int depth, const char *role, const char *model, const char *toolkit, const ipc_agent_binding_t *binding);`
- `bool ipc_set_status(ipc_agent_status_t status, const char *current_task);`
- `bool ipc_heartbeat(void);`
- `int ipc_list_agents(ipc_agent_info_t *out, int max);`
- `bool ipc_get_agent(const char *agent_id, ipc_agent_info_t *out);`
- `bool ipc_agent_alive(const char *agent_id);`
- `int ipc_reap_dead_agents(double stale_s);`
- `bool ipc_send(const char *to_agent, const char *topic, const char *body);`
- `int ipc_recv(ipc_message_t *out, int max);`
- `int ipc_recv_topic(const char *topic, ipc_message_t *out, int max);`
- `int ipc_list_inbox(const char *agent_id, bool unread_only, bool mark_read, ipc_message_t *out, int max);`
- `int ipc_list_sent(const char *agent_id, ipc_message_t *out, int max);`
- `int ipc_list_bus(ipc_message_t *out, int max);`
- `int ipc_unread_count(void);`
- `int ipc_task_submit(const char *description, int priority, int parent_task_id);`
- `bool ipc_task_claim(ipc_task_t *out);`
- `bool ipc_task_start(int task_id);`
- `bool ipc_task_complete(int task_id, const char *result);`
- `bool ipc_task_fail(int task_id, const char *error);`
- `int ipc_task_list(const char *assigned_to, ipc_task_t *out, int max);`
- `int ipc_task_pending_count(void);`
- `int ipc_task_requeue_stale(double timeout_s);`
- `bool ipc_scratch_put(const char *key, const char *value);`
- `char *ipc_scratch_get(const char *key);`
- `bool ipc_scratch_del(const char *key);`
- `int ipc_scratch_keys(const char *prefix, char keys[][IPC_MAX_KEY], int max);`
- `int ipc_poll(void);`
- `int ipc_status_json(char *buf, size_t len);`
- `bool ipc_checkpoint_save(const char *agent_id, int generation, const char *memory_json, const char *conv_json, const char *plan_json, const char *task_json);`
- `bool ipc_checkpoint_restore(const char *agent_id, int generation, char **memory_json_out, char **conv_json_out, char **plan_json_out, char **task_json_out);`
- `int ipc_checkpoint_list(const char *agent_id, int *generations_out, double *timestamps_out, int max);`
- `int ipc_checkpoint_prune(const char *agent_id, int keep_generations);`

## `json_util.h`

Function-like declarations: 39

### Declarations

- `void arena_init(arena_t *a);`
- `void *arena_alloc(arena_t *a, size_t size);`
- `char *arena_strdup(arena_t *a, const char *s);`
- `void arena_reset(arena_t *a);`
- `void arena_free(arena_t *a);`
- `json_validation_t json_validate_schema(const char *json, const char *schema_json);`
- `void *safe_malloc(size_t size);`
- `void *safe_realloc(void *ptr, size_t size);`
- `void *safe_reallocarray(void *ptr, size_t count, size_t elem_size);`
- `char *safe_strdup(const char *s);`
- `((ptr) = safe_reallocarray((ptr), (count), sizeof *(ptr))) void jbuf_init(jbuf_t *b, size_t initial_cap);`
- `void jbuf_free(jbuf_t *b);`
- `void jbuf_reset(jbuf_t *b);`
- `void jbuf_append(jbuf_t *b, const char *s);`
- `void jbuf_append_len(jbuf_t *b, const char *s, size_t n);`
- `void jbuf_appendf(jbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));`
- `void jbuf_append_char(jbuf_t *b, char c);`
- `void jbuf_append_json_str(jbuf_t *b, const char *s);`
- `void jbuf_append_int(jbuf_t *b, int v);`
- `bool json_parse_response(const char *json, parsed_response_t *out);`
- `bool json_parse_response_arena(const char *json, parsed_response_t *out, arena_t *arena);`
- `void json_free_response(parsed_response_t *r);`
- `char *json_get_str(const char *json, const char *key);`
- `char *json_get_raw(const char *json, const char *key);`
- `int json_get_int(const char *json, const char *key, int def);`
- `long long json_get_i64(const char *json, const char *key, long long def);`
- `bool json_get_bool(const char *json, const char *key, bool def);`
- `double json_get_double(const char *json, const char *key, double def);`
- `json_view_t *json_view_open(const char *json);`
- `void json_view_close(json_view_t *v);`
- `const char *json_view_str(json_view_t *v, const char *key);`
- `long long json_view_i64(json_view_t *v, const char *key, long long def);`
- `double json_view_f64(json_view_t *v, const char *key, double def);`
- `bool json_view_bool(json_view_t *v, const char *key, bool def);`
- `bool json_view_ok(const json_view_t *v);`
- `bool json_is_valid_container(const char *json);`
- `typedef void (*json_array_cb)(const char *element_start, void *ctx);`
- `int json_array_foreach(const char *json, const char *key, json_array_cb cb, void *ctx);`
- `char *json_schema_ensure_property_types(const char *schema_json);`

## `killswitch.h`

Function-like declarations: 16

### Declarations

- `void killswitch_init(killswitch_registry_t *r);`
- `int killswitch_trigger(killswitch_registry_t *r, kill_level_t level, const char *target, const char *reason, kill_trigger_t trigger, int principal_tier, double timeout, bool cascade);`
- `bool killswitch_resolve(killswitch_registry_t *r, int kill_id, int principal_tier);`
- `bool killswitch_is_killed(const killswitch_registry_t *r, const char *target);`
- `bool killswitch_system_halted(const killswitch_registry_t *r);`
- `bool killswitch_level_active(const killswitch_registry_t *r, kill_level_t level, const char *target);`
- `int killswitch_get_active(const killswitch_registry_t *r, const char *target, killswitch_entry_t *out, int max);`
- `int killswitch_list_active(const killswitch_registry_t *r, killswitch_entry_t *out, int max);`
- `int killswitch_tick(killswitch_registry_t *r);`
- `int killswitch_to_json(const killswitch_registry_t *r, char *buf, size_t len);`
- `int killswitch_status_json(const killswitch_registry_t *r, char *buf, size_t len);`
- `const char *killswitch_level_name(kill_level_t l);`
- `const char *killswitch_state_name(kill_state_t s);`
- `const char *killswitch_trigger_name(kill_trigger_t t);`
- `void killswitch_set_vfs(vfs_db_t *vfs);`
- `int killswitch_restore_from_vfs(killswitch_registry_t *r);`

## `kimi_oauth.h`

Function-like declarations: 6

### Declarations

- `const char *kimi_oauth_access_token(bool allow_refresh);`
- `const char *kimi_oauth_refresh_after_unauthorized(const char *rejected_token);`
- `bool kimi_oauth_available(void);`
- `const char *kimi_oauth_source_name(void);`
- `struct curl_slist *kimi_oauth_append_identity_headers(struct curl_slist *headers, const char *product, const char *version);`
- `time_t kimi_code_quota_reset_at(void);`

## `kitty_agent_windows.h`

Function-like declarations: 4

### Declarations

- `void kitty_agent_window_spawn(int child_id, pid_t child_pid, const char *task, const char *model);`
- `void kitty_agent_window_append(int child_id, const char *data, size_t len);`
- `void kitty_agent_window_complete(int child_id, const char *status, int exit_code);`
- `void kitty_agent_windows_shutdown(void);`

## `kitty_banner.h`

Function-like declarations: 12

### Declarations

- `bool kitty_banner_available(FILE *out);`
- `void kitty_banner_invalidate_geometry(void);`
- `void kitty_banner_clear(FILE *out);`
- `void kitty_banner_settle(FILE *out);`
- `int kitty_banner_dissolve(FILE *out);`
- `int kitty_banner_render(FILE *out);`
- `int kitty_banner_render_layers(FILE *out, int loops);`
- `int kitty_banner_render_auto(FILE *out);`
- `bool kitty_banner_write_layers_ppm(const char *dir);`
- `bool kitty_banner_write_ppm(const char *path, int frame, int frames);`
- `bool kitty_banner_render_rgba(unsigned char *rgba, int width, int height, size_t stride, int frame, int frames);`
- `int dsco_banner_render_cells(FILE *out, int loops);`

## `kitty_banner_mask.h`

Function-like declarations: 0

### Declarations


## `kitty_graphics.h`

Function-like declarations: 11

### Declarations

- `void kitty_graphics_send_options_default(kitty_graphics_send_options_t *options);`
- `bool kitty_graphics_send_pixels(FILE *out, const char *control, const void *pixels, size_t pixel_bytes, const kitty_graphics_send_options_t *options);`
- `bool kitty_graphics_send_pixels_ex(FILE *out, const char *control, const void *pixels, size_t pixel_bytes, const kitty_graphics_send_options_t *options, kitty_graphics_send_stats_t *stats);`
- `bool kitty_graphics_send_rgb_patch(FILE *out, uint32_t image_id, uint32_t frame, int x, int y, int width, int height, const void *pixels, size_t pixel_bytes, kitty_graphics_send_stats_t *stats);`
- `bool kitty_graphics_select_frame(FILE *out, uint32_t image_id, uint32_t frame, kitty_graphics_send_stats_t *stats);`
- `bool kitty_graphics_environment_hint(void);`
- `bool kitty_graphics_probe(FILE *out);`
- `bool kitty_graphics_available(FILE *out);`
- `bool kitty_graphics_known_unsupported(void);`
- `bool kitty_graphics_emit_query(FILE *out, uint32_t image_id);`
- `size_t kitty_graphics_base64_encoded_length(size_t input_bytes);`

## `kitty_lab.h`

Function-like declarations: 4

### Declarations

- `bool kitty_lab_write_ppm(const char *path, int width, int height, int frame, int frames);`
- `bool kitty_lab_render(FILE *out, int width, int height, int frames, bool animate);`
- `bool kitty_lab_write_ppm_view(const char *path, int width, int height, int frame, int frames, kitty_lab_view_t view);`
- `bool kitty_lab_render_view(FILE *out, int width, int height, int frames, bool animate, kitty_lab_view_t view);`

## `kitty_taste_grid.h`

Function-like declarations: 3

### Declarations

- `bool kitty_taste_grid_write_ppm(const char *path, int width, int height);`
- `bool kitty_taste_grid_render(FILE *out, int width, int height);`
- `void kitty_taste_grid_clear(FILE *out);`

## `kitty_tools.h`

Function-like declarations: 2

### Declarations

- `bool tool_kitty_remote(const char *input_json, char *result, size_t result_len);`
- `bool tool_kitten(const char *input_json, char *result, size_t result_len);`

## `learned_cost.h`

Function-like declarations: 6

### Declarations

- `void cost_db_init(cost_db_t *db);`
- `bool cost_db_load(cost_db_t *db);`
- `bool cost_db_save(cost_db_t *db);`
- `void learn_from_execution(cost_db_t *db, const char *task, const char *topology, int actual_tokens, double actual_cost);`
- `bool predict_cost(const cost_db_t *db, const char *task, const char *topology, cost_prediction_result_t *out);`
- `int find_similar_executions(const cost_db_t *db, const char *task, const char *topology, int k, cost_record_t *out_records);`

## `legion.h`

Function-like declarations: 12

### Declarations

- `void legion_init(void);`
- `const legion_variant_t *legion_get(int id);`
- `const legion_variant_t *legion_find(const char *name);`
- `const legion_variant_t *legion_registry(int *count);`
- `int legion_find_by_role(legion_role_t role, agent_class_t cls, const legion_variant_t **out, int max_out);`
- `const legion_variant_t *legion_auto_select(const char *task, agent_class_t cls);`
- `int legion_spawn(int variant_id, const char *task);`
- `int legion_spawn_squad(const int *variant_ids, int count, const char *task, int *out_agent_ids);`
- `int legion_variant_json(const legion_variant_t *v, char *buf, size_t len);`
- `int legion_angel_count(void);`
- `int legion_demon_count(void);`
- `int legion_count_by_role(legion_role_t role);`

## `llm.h`

Function-like declarations: 66

### Declarations

- `typedef void (*stream_text_cb)(const char *text, void *ctx);`
- `typedef void (*stream_tool_start_cb)(const char *name, const char *id, void *ctx);`
- `typedef void (*stream_tool_arg_delta_cb)(const char *name, const char *id, const char *delta, void *ctx);`
- `typedef void (*stream_thinking_cb)(const char *text, void *ctx);`
- `void session_state_init(session_state_t *s, const char *model);`
- `const char *session_trust_tier_to_string(dsco_trust_tier_t tier);`
- `dsco_trust_tier_t session_trust_tier_from_string(const char *s, bool *ok);`
- `const char *session_goal_status_to_string(dsco_goal_status_t status);`
- `dsco_goal_status_t session_goal_status_from_string(const char *s, bool *ok);`
- `void conv_init(conversation_t *c);`
- `void conv_free(conversation_t *c);`
- `void conv_add_user_text(conversation_t *c, const char *text);`
- `void conv_add_assistant_text(conversation_t *c, const char *text);`
- `void conv_add_assistant_tool_use(conversation_t *c, const char *tool_id, const char *tool_name, const char *tool_input);`
- `void conv_add_tool_result_named(conversation_t *c, const char *tool_id, const char *tool_name, const char *result, bool is_error);`
- `void conv_add_tool_result(conversation_t *c, const char *tool_id, const char *result, bool is_error);`
- `void conv_add_assistant_raw(conversation_t *c, parsed_response_t *resp);`
- `void conv_add_user_image_base64(conversation_t *c, const char *media_type, const char *base64_data, const char *text);`
- `void conv_add_user_image_url(conversation_t *c, const char *url, const char *text);`
- `void conv_add_user_document(conversation_t *c, const char *media_type, const char *base64_data, const char *title, const char *text);`
- `void conv_pop_last(conversation_t *c);`
- `bool conv_pop_last_turn(conversation_t *c);`
- `void conv_ensure_tool_results(conversation_t *c);`
- `tool_integrity_result_t conv_validate_tool_call_integrity(const conversation_t *c, bool allow_pending_last);`
- `void conv_trim_old_results(conversation_t *c, int keep_recent, int max_chars);`
- `bool conv_compact_recent_tool_turn(conversation_t *c, int max_chars, int protect_tail);`
- `void compact_config_init(compact_config_t *cfg);`
- `int conv_build_rounds(conversation_t *c, api_round_t *rounds, int max_rounds);`
- `void conv_drop_rounds(conversation_t *c, api_round_t *rounds, int n_drop, int total_rounds);`
- `} int conv_rough_estimate(conversation_t *c);`
- `int conv_token_estimate(conversation_t *c, session_state_t *s);`
- `int effective_context_window(session_state_t *s);`
- `int auto_compact_threshold(session_state_t *s);`
- `void post_compact_restore_init(post_compact_restore_t *r);`
- `void post_compact_restore_free(post_compact_restore_t *r);`
- `void post_compact_restore_track(post_compact_restore_t *r, const char *path, const char *content);`
- `void post_compact_restore_inject(post_compact_restore_t *r, conversation_t *c);`
- `void conv_strip_binaries(conversation_t *c, int keep_recent);`
- `compact_result_t conv_auto_compact(conversation_t *c, session_state_t *s, compact_config_t *cfg);`
- `char *tools_build_deferred_catalog(const char **paged_names, int paged_count, int *out_deferred_count);`
- `bool conv_save(conversation_t *c, const char *path);`
- `bool conv_load(conversation_t *c, const char *path);`
- `bool conv_save_ex(conversation_t *c, const session_state_t *session, const char *path);`
- `bool conv_load_ex(conversation_t *c, session_state_t *session, const char *path);`
- `char *llm_build_request(conversation_t *c, const char *model, int max_tokens);`
- `char *llm_build_request_ex(conversation_t *c, session_state_t *session, int max_tokens);`
- `char *llm_build_request_for_credential(conversation_t *c, const char *model, int max_tokens, const char *credential);`
- `char *llm_build_request_ex_for_credential(conversation_t *c, session_state_t *session, int max_tokens, const char *credential);`
- `int llm_count_tokens(const char *api_key, const char *request_json);`
- `const char *llm_get_custom_system_prompt(void);`
- `void llm_debug_save_request(const char *request_json, int http_status);`
- `bool llm_anthropic_uses_claude_code_auth(const char *credential);`
- `stream_result_t llm_stream(const char *api_key, const char *request_json, stream_text_cb text_cb, stream_tool_start_cb tool_cb, stream_tool_arg_delta_cb tool_delta_cb, stream_thinking_cb thinking_cb, void *cb_ctx);`
- `stream_result_t llm_stream_reuse(CURL *curl, const char *api_key, const char *request_json, stream_text_cb text_cb, stream_tool_start_cb tool_cb, stream_tool_arg_delta_cb tool_delta_cb, stream_thinking_cb thinking_cb, void *cb_ctx);`
- `void dsco_strip_terminal_controls_inplace(char *s);`
- `void tool_metrics_init(tool_metrics_t *m);`
- `void tool_metrics_record(tool_metrics_t *m, const char *name, bool success, double latency_ms);`
- `const tool_metric_t *tool_metrics_get(tool_metrics_t *m, const char *name);`
- `void tool_cache_init(tool_cache_t *c);`
- `void tool_cache_free(tool_cache_t *c);`
- `bool tool_cache_get(tool_cache_t *c, const char *tool, const char *input, char *result, size_t rlen, bool *success);`
- `void tool_cache_put(tool_cache_t *c, const char *tool, const char *input, const char *result, bool success, double ttl);`
- `void stream_checkpoint_init(stream_checkpoint_t *cp);`
- `void stream_checkpoint_save(stream_checkpoint_t *cp, const content_block_t *blocks, int block_count, const char *partial_text, const char *partial_input, const usage_t *usage, const stream_telemetry_t *telemetry);`
- `void stream_checkpoint_free(stream_checkpoint_t *cp);`
- `injection_level_t detect_prompt_injection(const char *text);`

## `local_llm.h`

Function-like declarations: 6

### Declarations

- `int local_llm_probe_servers(local_server_t *out, int max);`
- `int local_llm_list_models(local_model_t *out, int max);`
- `bool local_llm_server_up(const char *server);`
- `bool local_llm_base_url(const char *server, char *out, size_t out_len);`
- `int local_llm_context_window(const char *server, const char *model);`
- `bool local_llm_is_local_ref(const char *name);`

## `math_fastpath.h`

Function-like declarations: 6

### Declarations

- `bool mfp_is_func(const char *name, size_t len);`
- `bool mfp_is_const(const char *name, size_t len);`
- `bool mfp_ident_known(const char *name, size_t len);`
- `bool mfp_is_pure_math(const char *expr, char *reason);`
- `bool mfp_rewrite_func_shorthand(const char *expr, char *out, size_t out_len);`
- `bool mfp_eval(const char *expr, char *out, size_t out_len);`

## `mcp.h`

Function-like declarations: 7

### Declarations

- `int mcp_init(mcp_registry_t *reg);`
- `void mcp_set_silent(bool silent);`
- `void mcp_cancel(void);`
- `void mcp_cancel_reset(void);`
- `void mcp_shutdown(mcp_registry_t *reg);`
- `const mcp_tool_t *mcp_get_tools(mcp_registry_t *reg, int *count);`
- `char *mcp_call_tool(mcp_registry_t *reg, const char *tool_name, const char *arguments_json);`

## `mcp_names.h`

Function-like declarations: 4

### Declarations

- `void dsco_mcp_normalize_name(const char *in, char *out, size_t out_len);`
- `void dsco_mcp_build_tool_name(const char *server_name, const char *tool_name, char *out, size_t out_len);`
- `bool dsco_mcp_is_canonical_tool_name(const char *name);`
- `void dsco_mcp_legacy_alias_from_canonical(const char *name, char *out, size_t out_len);`

## `mcp_server.h`

Function-like declarations: 1

### Declarations

- `int mcp_server_run(const char *toolsets_csv, const char *tier);`

## `md.h`

Function-like declarations: 8

### Declarations

- `void md_init(md_renderer_t *r, FILE *out);`
- `void md_feed(md_renderer_t *r, const char *text, size_t len);`
- `void md_feed_str(md_renderer_t *r, const char *text);`
- `void md_flush(md_renderer_t *r);`
- `void md_reset(md_renderer_t *r);`
- `char *md_latex_to_unicode(const char *tex, size_t len);`
- `void md_render_code_line(FILE *out, const char *line, const char *lang);`
- `const char *md_lang_from_path(const char *path);`

## `memory_tier.h`

Function-like declarations: 35

### Declarations

- `void memory_store_init(memory_store_t *m);`
- `void memory_store_destroy(memory_store_t *m);`
- `int memory_store(memory_store_t *m, memory_tier_t tier, const char *key, const char *value, double importance);`
- `int memory_store_tagged(memory_store_t *m, memory_tier_t tier, const char *key, const char *value, double importance, const char **tags, int tag_count);`
- `const memory_entry_t *memory_recall(memory_store_t *m, const char *key);`
- `int memory_recall_by_tag(memory_store_t *m, const char *tag, const memory_entry_t **out, int max);`
- `int memory_recall_tier(memory_store_t *m, memory_tier_t tier, const memory_entry_t **out, int max);`
- `int memory_search(memory_store_t *m, const char *query, const memory_entry_t **out, int max);`
- `bool memory_update(memory_store_t *m, const char *key, const char *value);`
- `bool memory_forget(memory_store_t *m, const char *key);`
- `bool memory_pin(memory_store_t *m, const char *key);`
- `bool memory_unpin(memory_store_t *m, const char *key);`
- `bool memory_promote(memory_store_t *m, const char *key);`
- `bool memory_classify(memory_store_t *m, const char *key, memory_class_t level);`
- `bool memory_classify_review(memory_store_t *m, const char *key);`
- `int memory_purge_umbral(memory_store_t *m);`
- `bool memory_class_promotable(const memory_entry_t *e);`
- `int memory_decay_tick(memory_store_t *m, double threshold);`
- `int memory_consolidate(memory_store_t *m);`
- `double memory_keep_score(const memory_entry_t *e, memory_keep_mode_t mode, double relevance, double now);`
- `int memory_tick(memory_store_t *m);`
- `int memory_to_json(const memory_store_t *m, char *buf, size_t len);`
- `int memory_tier_to_json(const memory_store_t *m, memory_tier_t tier, char *buf, size_t len);`
- `int memory_status_json(const memory_store_t *m, char *buf, size_t len);`
- `const char *memory_tier_name(memory_tier_t t);`
- `double memory_tier_halflife(memory_tier_t t);`
- `const char *memory_class_name(memory_class_t c);`
- `bool memory_class_from_name(const char *s, memory_class_t *out);`
- `double memory_calc_strength(memory_tier_t tier, double created_at, double now);`
- `void memory_store_set_vfs(vfs_db_t *vfs);`
- `void memory_persist_semantic(memory_store_t *m);`
- `int memory_restore_semantic(memory_store_t *m);`
- `void memory_store_set_vecstore(struct vecstore *vs);`
- `int memory_search_semantic(memory_store_t *m, const char *query, const memory_entry_t **out, int max);`
- `bool memory_entry_set_embedding(memory_store_t *m, const char *key, const float *vec, int dim);`

## `mesh.h`

Function-like declarations: 16

### Declarations

- `typedef void (*mesh_on_message_fn)(const uint8_t *from_pk, const void *data, size_t len, void *ctx);`
- `typedef void (*mesh_on_peer_fn)(const mesh_peer_info_t *peer, void *ctx);`
- `mesh_node_t *mesh_node_create(uint16_t port);`
- `void mesh_node_destroy(mesh_node_t *n);`
- `bool mesh_node_start(mesh_node_t *n);`
- `void mesh_node_stop(mesh_node_t *n);`
- `const uint8_t *mesh_node_pubkey(mesh_node_t *n);`
- `bool mesh_node_connect(mesh_node_t *n, const char *host, uint16_t port);`
- `bool mesh_node_send_to(mesh_node_t *n, const uint8_t *peer_pk, const void *data, size_t len);`
- `int mesh_node_broadcast(mesh_node_t *n, const void *data, size_t len);`
- `int mesh_node_peers(mesh_node_t *n, mesh_peer_info_t *out, int max);`
- `void mesh_node_set_on_message(mesh_node_t *n, mesh_on_message_fn cb, void *ctx);`
- `void mesh_node_set_on_connect(mesh_node_t *n, mesh_on_peer_fn cb, void *ctx);`
- `void mesh_node_set_on_disconnect(mesh_node_t *n, mesh_on_peer_fn cb, void *ctx);`
- `void mesh_pubkey_to_hex(const uint8_t *pk, char out[65]);`
- `bool mesh_pubkey_from_hex(const char *hex, uint8_t pk[MESH_PUBKEY_LEN]);`

## `model_pricing.h`

Function-like declarations: 4

### Declarations

- `void model_pricing_init(void);`
- `void model_pricing_shutdown(void);`
- `int model_pricing_lookup(const char *provider, const char *model_id, model_price_t *out);`
- `int model_pricing_load_openai_markdown(const char *markdown, size_t len);`

## `native_composer.h`

Function-like declarations: 1

### Declarations

- `bool native_composer_build(native_ui_scene_t *scene, int width, int height, const native_composer_model_t *model);`

## `native_masthead.h`

Function-like declarations: 1

### Declarations

- `bool native_masthead_build(native_ui_scene_t *scene, int width, int height, const native_masthead_model_t *model);`

## `native_ui.h`

Function-like declarations: 27

### Declarations

- `typedef bool (*native_ui_event_handler_t)(struct native_ui_scene *scene, struct native_ui_node *node, const native_ui_event_t *event, void *context);`
- `typedef struct { void (*begin_frame)(void *context, native_ui_rect_t viewport, const native_ui_damage_t *damage);`
- `void (*push_clip)(void *context, native_ui_rect_t rect);`
- `void (*pop_clip)(void *context);`
- `void (*fill_rect)(void *context, native_ui_rect_t rect, native_ui_color_token_t color, uint8_t opacity, uint8_t radius);`
- `void (*stroke_rect)(void *context, native_ui_rect_t rect, native_ui_color_token_t color, uint8_t width, uint8_t radius);`
- `void (*draw_text)(void *context, native_ui_rect_t rect, const char *text, native_ui_type_token_t type, native_ui_color_token_t color, uint8_t opacity);`
- `void (*draw_icon)(void *context, native_ui_rect_t rect, const char *name, native_ui_color_token_t color, uint8_t opacity);`
- `void (*draw_custom)(void *context, const native_ui_node_t *node);`
- `void (*end_frame)(void *context);`
- `void native_ui_scene_init(native_ui_scene_t *scene, int width, int height);`
- `int native_ui_scene_add(native_ui_scene_t *scene, int parent, uint64_t key, native_ui_element_t element, native_ui_role_t role);`
- `native_ui_node_t *native_ui_scene_node(native_ui_scene_t *scene, int index);`
- `void native_ui_node_set_text(native_ui_node_t *node, const char *text);`
- `void native_ui_node_set_accessibility_label(native_ui_node_t *node, const char *label);`
- `void native_ui_layout(native_ui_scene_t *scene);`
- `void native_ui_diff(const native_ui_scene_t *previous, const native_ui_scene_t *current, native_ui_damage_t *damage);`
- `int native_ui_hit_test(const native_ui_scene_t *scene, int x, int y);`
- `int native_ui_focus_move(native_ui_scene_t *scene, bool backwards);`
- `bool native_ui_dispatch(native_ui_scene_t *scene, const native_ui_event_t *event);`
- `void native_ui_render(const native_ui_scene_t *scene, const native_ui_scene_t *previous, const native_ui_backend_t *backend, void *context);`
- `int native_ui_scene_from_json(native_ui_scene_t *scene, const char *json, int width, int height);`
- `native_ui_density_t native_ui_density_for_size(int width, int height);`
- `native_ui_viewport_metrics_t native_ui_terminal_viewport(int columns, int rows, int physical_width, int physical_height, int requested_scale);`
- `native_ui_agent_shell_layout_t native_ui_agent_shell_layout(int width, int height);`
- `native_ui_composer_layout_t native_ui_composer_layout(const char *text, size_t cursor_byte, int columns, int max_rows);`
- `const char *native_ui_role_name(native_ui_role_t role);`
- `const char *native_ui_agent_state_name(native_ui_agent_state_t state);`

## `net_server.h`

Function-like declarations: 13

### Declarations

- `typedef netsrv_response_t (*netsrv_handler_fn)(const netsrv_request_t *req, void *ctx);`
- `int netsrv_stream_send(netsrv_stream_t *s, const char *buf, size_t len);`
- `typedef void (*netsrv_stream_fn)(const netsrv_request_t *req, netsrv_stream_t *s, void *ctx);`
- `dsco_net_server_t *netsrv_create(uint16_t port, bool use_tls, const char *cert_pem_path, const char *key_pem_path);`
- `void netsrv_destroy(dsco_net_server_t *s);`
- `bool netsrv_route(dsco_net_server_t *s, const char *method, const char *path, netsrv_handler_fn fn, void *ctx);`
- `bool netsrv_route_stream(dsco_net_server_t *s, const char *method, const char *path, netsrv_stream_fn fn, void *ctx);`
- `void netsrv_set_auth_key(dsco_net_server_t *s, const uint8_t *key, size_t key_len);`
- `bool netsrv_start(dsco_net_server_t *s);`
- `void netsrv_stop(dsco_net_server_t *s);`
- `uint16_t netsrv_port(dsco_net_server_t *s);`
- `bool netsrv_gen_tls_cert(const char *cert_path, const char *key_path, const char *cn);`
- `char *netsrv_client_post(const char *host, uint16_t port, const char *path, const char *json_body, const uint8_t *auth_key, size_t auth_key_len, bool use_tls);`

## `ooda.h`

Function-like declarations: 22

### Declarations

- `void ooda_engine_init(ooda_engine_t *e);`
- `int ooda_begin(ooda_engine_t *e);`
- `bool ooda_observe(ooda_engine_t *e, const char *content, const char *source, double confidence);`
- `bool ooda_orient_add(ooda_engine_t *e, const char *factor, double weight, bool is_constraint);`
- `bool ooda_orient_context(ooda_engine_t *e, double budget_remaining, int principal_tier, bool safety_critical);`
- `ooda_action_t ooda_decide(ooda_engine_t *e);`
- `bool ooda_act_result(ooda_engine_t *e, bool success, const char *result);`
- `bool ooda_complete(ooda_engine_t *e);`
- `void ooda_abort(ooda_engine_t *e);`
- `ooda_phase_t ooda_current_phase(const ooda_engine_t *e);`
- `const ooda_decision_t *ooda_current_decision(const ooda_engine_t *e);`
- `int ooda_recent_history(const ooda_engine_t *e, ooda_history_entry_t *out, int max);`
- `double ooda_success_rate(const ooda_engine_t *e, int last_n);`
- `double ooda_brier_score(const ooda_engine_t *e, int last_n);`
- `double ooda_calibration_gap(const ooda_engine_t *e, int last_n);`
- `int ooda_to_json(const ooda_engine_t *e, char *buf, size_t len);`
- `int ooda_cycle_to_json(const ooda_cycle_t *c, char *buf, size_t len);`
- `const char *ooda_phase_name(ooda_phase_t p);`
- `const char *ooda_action_name(ooda_action_t a);`
- `const char *ooda_capability_name(capability_tier_t t);`
- `void ooda_set_scheduler(struct scheduler_t *sched);`
- `int ooda_run_scheduled(ooda_cycle_t *cycle, struct scheduler_t *sched);`

## `openai_images.h`

Function-like declarations: 1

### Declarations

- `bool tool_openai_image_generate(const char *input_json, char *result, size_t result_len);`

## `openai_oauth.h`

Function-like declarations: 8

### Declarations

- `bool openai_oauth_load(openai_oauth_bundle_t *out);`
- `bool openai_oauth_refresh(openai_oauth_bundle_t *bundle);`
- `const char *openai_oauth_access_token(bool allow_refresh);`
- `bool openai_oauth_account_id(char *out, size_t out_len);`
- `bool openai_oauth_available(void);`
- `const char *openai_oauth_source_name(void);`
- `int openai_oauth_login(void);`
- `int openai_oauth_logout(void);`

## `openrouter_cache.h`

Function-like declarations: 12

### Declarations

- `void openrouter_cache_init(void);`
- `int openrouter_cache_count(void);`
- `int openrouter_cache_wait_ready(int timeout_ms);`
- `int openrouter_cache_load_sync(void);`
- `typedef void (*or_model_cb)(const or_model_view_t *m, void *ud);`
- `int openrouter_cache_foreach(or_model_cb cb, void *ud);`
- `const char *dsco_route_by_task(dsco_task_type_t task);`
- `const char *dsco_route_optimal(double budget_per_1m, int min_ctx);`
- `int dsco_route_failover_dynamic(const char *failed_model, char out_models[][128], int max_models);`
- `const char *dsco_route_cheapest_tool(void);`
- `const char *dsco_route_free_tool(void);`
- `const char *dsco_route_largest_ctx_tool(void);`

## `openrouter_lanes.h`

Function-like declarations: 2

### Declarations

- `int openrouter_lanes_build(const openrouter_lane_query_t *query, openrouter_lane_t *out, int max);`
- `bool tool_openrouter_lanes(const char *input, char *result, size_t rlen);`

## `orchestrator.h`

Function-like declarations: 1

### Declarations

- `bool agent_run_orchestrated(const char *api_key, const char *chat_model, const char *worker_model, const char *provider_override);`

## `output_guard.h`

Function-like declarations: 3

### Declarations

- `bool output_guard_init(void);`
- `void output_guard_reset(void);`
- `void output_guard_shutdown(void);`

## `overmind.h`

Function-like declarations: 5

### Declarations

- `void overmind_task_init(overmind_task_t *task);`
- `bool overmind_plan(const overmind_task_t *task, overmind_plan_t *plan);`
- `bool overmind_plan_valid(const overmind_plan_t *plan);`
- `const char *overmind_disposition_name(overmind_disposition_t disposition);`
- `int overmind_plan_summary(const overmind_plan_t *plan, char *buf, size_t len);`

## `peer_bootstrap.h`

Function-like declarations: 3

### Declarations

- `void peer_bootstrap_init(void *mesh_node, uint16_t port);`
- `void peer_bootstrap_stop(void);`
- `void peer_bootstrap_reseed(void);`

## `pets.h`

Function-like declarations: 25

### Declarations

- `void pet_roll(const char *seed, pet_bones_t *out);`
- `int pet_render_sprite(const pet_bones_t *b, int frame, char lines[6][96]);`
- `int pet_sprite_frame_count(pet_species_t s);`
- `void pet_render_face(const pet_bones_t *b, char *buf, size_t n);`
- `const char *pet_species_name(pet_species_t s);`
- `const char *pet_rarity_name(pet_rarity_t r);`
- `const char *pet_rarity_stars(pet_rarity_t r);`
- `const char *pet_rarity_color(pet_rarity_t r);`
- `const char *pet_stat_name(int i);`
- `const char *pet_status_glyph(pet_status_t st);`
- `const char *pet_status_color(pet_status_t st);`
- `const char *pet_status_word(pet_status_t st);`
- `void pet_card_print(FILE *out, const pet_t *p, int frame);`
- `void pet_gallery_print(FILE *out, int frame);`
- `void pet_roster_init(pet_roster_t *r);`
- `void pet_roster_free(pet_roster_t *r);`
- `int pet_roster_upsert(pet_roster_t *r, int id, int project_id, const char *label, const char *seed, pet_status_t status);`
- `void pet_roster_set_status(pet_roster_t *r, int id, pet_status_t status, double cost_usd);`
- `void pet_roster_activity(pet_roster_t *r, int id, double bytes);`
- `void pet_roster_remove(pet_roster_t *r, int id);`
- `void pet_roster_counts(pet_roster_t *r, int *pending, int *working, int *done, int *error, int *total);`
- `void pet_roster_render(FILE *out, pet_roster_t *r, int width, int max_rows);`
- `void pet_roster_tick(pet_roster_t *r);`
- `int pet_roster_next_unnotified(pet_roster_t *r, pet_status_t *out_status, char *out_label, size_t label_n, pet_bones_t *out_bones);`
- `pet_roster_t *pet_roster_global(void);`

## `pheromone.h`

Function-like declarations: 18

### Declarations

- `void pheromone_field_init(pheromone_field_t *f);`
- `void pheromone_field_destroy(pheromone_field_t *f);`
- `int pheromone_deposit(pheromone_field_t *f, pheromone_type_t type, double concentration, const char *region, const char *source, const char *meta);`
- `int pheromone_deposit_ex(pheromone_field_t *f, pheromone_type_t type, double concentration, const char *region, const char *source, const char *meta, pheromone_decay_t decay_fn, double lambda, double ttl);`
- `double pheromone_sense(pheromone_field_t *f, pheromone_type_t type, const char *region, pheromone_aggregation_t agg);`
- `bool pheromone_gradient(pheromone_field_t *f, pheromone_type_t type, const char *region, pheromone_aggregation_t agg, pheromone_gradient_t *out);`
- `int pheromone_sense_all(pheromone_field_t *f, const char *region, pheromone_aggregation_t agg, pheromone_gradient_t *out, int max);`
- `int pheromone_tick(pheromone_field_t *f);`
- `bool pheromone_reinforce(pheromone_field_t *f, int signal_id, double amount);`
- `int pheromone_evaporate_region(pheromone_field_t *f, const char *region);`
- `int pheromone_to_json(const pheromone_field_t *f, char *buf, size_t len);`
- `bool pheromone_from_json(pheromone_field_t *f, const char *json);`
- `const char *pheromone_type_name(pheromone_type_t t);`
- `const char *pheromone_decay_name(pheromone_decay_t d);`
- `const char *pheromone_agg_name(pheromone_aggregation_t a);`
- `int pheromone_status_json(const pheromone_field_t *f, char *buf, size_t len);`
- `void pheromone_attach_event_loop(pheromone_field_t *f, ev_loop_t *loop, int interval_ms);`
- `void pheromone_detach_event_loop(void);`

## `pipeline.h`

Function-like declarations: 7

### Declarations

- `pipeline_t *pipeline_create(const char *input);`
- `void pipeline_free(pipeline_t *p);`
- `void pipeline_add_stage(pipeline_t *p, pipe_stage_type_t type, const char *arg);`
- `void pipeline_add_stage_n(pipeline_t *p, pipe_stage_type_t type, int n);`
- `char *pipeline_execute(pipeline_t *p);`
- `pipeline_t *pipeline_parse(const char *input, const char *spec);`
- `char *pipeline_run(const char *input, const char *spec);`

## `pixel_fx.h`

Function-like declarations: 10

### Declarations

- `void pixel_fx_surface_init(pixel_fx_surface_t *s, uint8_t *rgb, int width, int height);`
- `bool pixel_fx_clip_push(pixel_fx_surface_t *s, int x, int y, int w, int h);`
- `void pixel_fx_clip_pop(pixel_fx_surface_t *s);`
- `void pixel_fx_fill_rounded(pixel_fx_surface_t *s, int x, int y, int w, int h, int radius, pixel_fx_rgb_t color, double alpha);`
- `void pixel_fx_stroke_rounded(pixel_fx_surface_t *s, int x, int y, int w, int h, int radius, int thickness, pixel_fx_rgb_t color, double alpha);`
- `void pixel_fx_shadow(pixel_fx_surface_t *s, int x, int y, int w, int h, int radius, int blur, int offset_y, pixel_fx_rgb_t color, double alpha);`
- `void pixel_fx_gradient(pixel_fx_surface_t *s, int x, int y, int w, int h, const pixel_fx_stop_t *stops, int stop_count, bool horizontal, double alpha);`
- `void pixel_fx_gradient_radial(pixel_fx_surface_t *s, int cx, int cy, int radius, const pixel_fx_stop_t *stops, int stop_count, double alpha);`
- `bool pixel_fx_blur(pixel_fx_surface_t *s, int x, int y, int w, int h, int radius);`
- `void pixel_fx_glass(pixel_fx_surface_t *s, int x, int y, int w, int h, int corner_radius, int blur_radius, pixel_fx_rgb_t tint, double tint_alpha);`

## `pixel_tui.h`

Function-like declarations: 53

### Declarations

- `bool pixel_tui_available(FILE *out);`
- `bool pixel_tui_session_begin(FILE *out, const char *model);`
- `bool pixel_tui_session_active(void);`
- `void pixel_tui_session_set_state(FILE *out, pixel_tui_state_t state);`
- `void pixel_tui_session_begin_message(FILE *out, const char *role, const char *detail);`
- `void pixel_tui_session_append_text(FILE *out, const char *text);`
- `void pixel_tui_session_end_message(FILE *out);`
- `void pixel_tui_session_add_message(FILE *out, const char *role, const char *text);`
- `void pixel_tui_session_show_commands(FILE *out, const pixel_tui_command_t *commands, int count);`
- `void pixel_tui_session_set_input(FILE *out, const char *text, size_t cursor, bool active);`
- `void pixel_tui_session_set_composer(FILE *out, const char *text, size_t cursor, bool active, pixel_tui_menu_kind_t menu_kind, const pixel_tui_menu_item_t *items, int item_count, int selected);`
- `void pixel_tui_session_set_model(FILE *out, const char *model, const char *slot_name);`
- `void pixel_tui_session_set_usage(FILE *out, int input_tokens, int output_tokens, double cost_usd, int turn, int tools_used);`
- `void pixel_tui_session_set_budget(FILE *out, double limit_usd, double burn_rate, double percent, double runway_seconds);`
- `void pixel_tui_session_set_clock(FILE *out, bool show_clock);`
- `const char *pixel_tui_theme_name(void);`
- `bool pixel_tui_session_set_theme(FILE *out, const char *name);`
- `const char *pixel_tui_session_cycle_theme(FILE *out, int direction);`
- `void pixel_tui_session_notify(FILE *out, pixel_tui_notice_level_t level, const char *text);`
- `void pixel_tui_session_clear_notifications(FILE *out);`
- `void pixel_tui_session_show_modal(FILE *out, pixel_tui_modal_kind_t kind, const char *title, const char *subtitle, const pixel_tui_menu_item_t *items, int item_count, int selected, const char *footer);`
- `void pixel_tui_session_clear_modal(FILE *out);`
- `void pixel_tui_session_scroll(FILE *out, int lines);`
- `void pixel_tui_session_set_turn(FILE *out, int turn);`
- `void pixel_tui_session_set_runtime_metrics(FILE *out, double cost_usd, double context_percent);`
- `void pixel_tui_session_set_queue_depth(FILE *out, int depth, int capacity);`
- `pixel_tui_tool_view_t pixel_tui_tool_view_parse(const char *value);`
- `const char *pixel_tui_tool_view_name(pixel_tui_tool_view_t view);`
- `void pixel_tui_tool_result_preview(const char *result, pixel_tui_tool_view_t view, char *dst, size_t cap, uint32_t *tail_lines, uint32_t *total_bytes);`
- `uint64_t pixel_tui_session_tool_begin(FILE *out, const char *name, const char *input_json);`
- `void pixel_tui_session_tool_end(FILE *out, uint64_t operation_id, const char *name, bool ok, double elapsed_ms, const char *result);`
- `void pixel_tui_session_swarm_update(FILE *out, int child_id, const char *status, const char *task, const char *model, size_t output_bytes, double cost_usd);`
- `void pixel_tui_session_note_thinking(FILE *out, const char *delta);`
- `void pixel_tui_session_terminal_control(FILE *out, const char *sequence);`
- `void pixel_tui_session_suspend_terminal(FILE *out);`
- `void pixel_tui_session_resume_terminal(FILE *out);`
- `bool pixel_tui_session_terminal_suspended(void);`
- `void pixel_tui_session_refresh(FILE *out);`
- `void pixel_tui_session_end(FILE *out);`
- `int pixel_tui_render_plan(FILE *out, int plan_id);`
- `int pixel_tui_render_plan_view(FILE *out, int plan_id, pixel_plan_view_t view);`
- `bool pixel_tui_write_plan_ppm(const char *path, int plan_id, int width);`
- `bool pixel_tui_write_plan_view_ppm(const char *path, int plan_id, int width, pixel_plan_view_t view);`
- `bool pixel_tui_write_session_ppm(const char *path, int width, int height, const char *model, pixel_tui_state_t state);`
- `bool pixel_tui_write_fixture_ppm(const char *path, int width, int height, const pixel_tui_fixture_t *fixture, pixel_tui_density_metrics_t *metrics);`
- `void pixel_tui_tool_preview_extract(const char *name, const char *input_json, char *dst, size_t cap);`
- `int pixel_tui_render_scene_json(FILE *out, const char *scene_json);`
- `bool pixel_tui_write_scene_ppm(const char *path, const char *scene_json, int width, int height);`
- `bool tool_ui_render(const char *input_json, char *result, size_t result_len);`
- `typedef bool (*pixel_capture_publish_fn)(const char *line, size_t len, void *ctx);`
- `void pixel_capture_parser_init(pixel_capture_parser_t *p);`
- `bool pixel_capture_parser_feed(pixel_capture_parser_t *p, const char *bytes, size_t n, pixel_capture_publish_fn publish, void *ctx);`
- `bool pixel_capture_parser_finish(pixel_capture_parser_t *p, pixel_capture_publish_fn publish, void *ctx);`

## `pixel_tui_perf.h`

Function-like declarations: 11

### Declarations

- `bool pixel_tui_perf_enabled(void);`
- `void pixel_tui_perf_set_capture(bool enabled);`
- `void pixel_tui_perf_reset(void);`
- `void pixel_tui_perf_note_stream_request(bool coalesced);`
- `void pixel_tui_perf_note_throttled(void);`
- `void pixel_tui_perf_note_scheduler_wake(double lateness_ms, double frame_gap_ms, double frame_interval_ms, bool rendered);`
- `void pixel_tui_perf_note_producer_wait(double wait_ms);`
- `void pixel_tui_perf_record(const pixel_tui_frame_sample_t *sample);`
- `pixel_tui_perf_snapshot_t pixel_tui_perf_snapshot(void);`
- `bool pixel_tui_perf_write_json(FILE *out);`
- `bool pixel_tui_perf_report_from_env(FILE *fallback);`

## `plan.h`

Function-like declarations: 53

### Declarations

- `void plan_engine_init(void);`
- `int plan_create(const char *title, const char *goal, plan_mode_t mode);`
- `bool plan_delete(int plan_id);`
- `plan_t *plan_get(int plan_id);`
- `int plan_list(int *ids_out, int max_out);`
- `bool plan_set_description(int plan_id, const char *goal);`
- `int plan_add_step(int plan_id, int parent_step_id, const char *title, step_type_t type);`
- `bool plan_remove_step(int plan_id, int step_id);`
- `step_t *step_get(int step_id);`
- `bool step_set_title(int step_id, const char *title);`
- `bool step_set_description(int step_id, const char *desc);`
- `bool step_set_priority(int step_id, int priority);`
- `bool step_set_status(int step_id, plan_status_t status);`
- `bool step_add_tag(int step_id, const char *tag);`
- `bool step_add_note(int step_id, const char *note);`
- `bool step_add_dep(int step_id, int dep_step_id);`
- `bool step_remove_dep(int step_id, int dep_step_id);`
- `bool step_deps_satisfied(int step_id);`
- `bool step_can_run(int step_id);`
- `int step_add_atom(int step_id, const char *title, atom_type_t type);`
- `bool atom_remove(int step_id, int atom_id);`
- `atom_t *atom_get(int atom_id);`
- `bool atom_set_tool(int atom_id, const char *tool_name, const char *input_json);`
- `bool atom_set_shell(int atom_id, const char *command);`
- `bool atom_set_dialog_prompt(int atom_id, const char *prompt);`
- `bool atom_set_assert(int atom_id, const char *condition);`
- `bool atom_set_result(int atom_id, const char *result);`
- `bool atom_run(int atom_id, char *result_buf, size_t rlen);`
- `bool atom_wire(int src_atom_id, int dst_atom_id, const char *key);`
- `bool atom_unwire(int src_atom_id, int dst_atom_id);`
- `char *atom_resolve_inputs(int atom_id);`
- `int plan_ready_atoms(int plan_id, int *atom_ids_out, int max_out);`
- `int plan_run_next(int plan_id);`
- `bool atom_inputs_ready(int atom_id);`
- `void plan_rollup_status(int plan_id);`
- `int plan_run_all(int plan_id, int max_atoms);`
- `int plan_decompose(int plan_id, int focus_step_id, const char **subtitles, int count);`
- `int plan_aggregate_atoms(int plan_id, int parent_step_id, const char *step_title, const int *atom_ids, int count);`
- `int plan_dialog_ask(int plan_id, int step_id, const char *prompt, const char **choices, int choice_count);`
- `bool plan_dialog_answer(int dialog_id, const char *response, int chosen);`
- `dialog_t *dialog_get(int dialog_id);`
- `int plan_pending_dialogs(int plan_id, int *dialog_ids_out, int max_out);`
- `int plan_to_json(int plan_id, char *buf, size_t len);`
- `int plan_summary(int plan_id, char *buf, size_t len);`
- `int plan_tree_render(int plan_id, char *buf, size_t len);`
- `const char *plan_status_name(plan_status_t s);`
- `const char *plan_mode_name(plan_mode_t m);`
- `const char *step_type_name(step_type_t t);`
- `const char *atom_type_name(atom_type_t t);`
- `plan_mode_t plan_mode_parse(const char *s);`
- `step_type_t step_type_parse(const char *s);`
- `atom_type_t atom_type_parse(const char *s);`
- `plan_status_t plan_status_parse(const char *s);`

## `plan_cache.h`

Function-like declarations: 13

### Declarations

- `void plan_cache_init(void);`
- `void plan_cache_free(void);`
- `void plan_cache_load(void);`
- `void plan_cache_save(void);`
- `void plan_cache_flush(void);`
- `bool plan_cache_lookup(const char *task, plan_cache_result_t *result);`
- `void plan_cache_store(const char *task, const char *topology_name, const char *rationale, float fit_score);`
- `void plan_cache_store_json(const char *task, const char *plan_json);`
- `void plan_cache_feedback(const char *task, bool success);`
- `const plan_cache_entry_t *plan_cache_find_entry(const char *task);`
- `int plan_cache_stats_json(char *buf, size_t buflen);`
- `float plan_similarity_score(const char *task_a, const char *task_b);`
- `char *plan_cache_adapt(const plan_cache_entry_t *entry, uint64_t expected_task_hash, const char *new_task);`

## `plan_dag.h`

Function-like declarations: 4

### Declarations

- `const char *plan_graph_error_name(plan_graph_error_t code);`
- `bool plan_graph_validate(int plan_id, plan_graph_diagnostic_t *diag);`
- `int plan_step_ready_wave(int plan_id, int *step_ids, int max_ids);`
- `int plan_atom_ready_wave(int plan_id, int *atom_ids, int max_ids);`

## `plan_optimizer.h`

Function-like declarations: 6

### Declarations

- `plan_options_t *plan_analyze(const char *task, int budget_cents);`
- `void plan_options_free(plan_options_t *opts);`
- `int plan_options_json(const plan_options_t *opts, char *buf, size_t buflen);`
- `const plan_option_t *plan_options_best(const plan_options_t *opts);`
- `double plan_estimate_cost(const plan_option_t *opt);`
- `double topology_cost_multiplier(const char *topology_name);`

## `plot.h`

Function-like declarations: 26

### Declarations

- `plot_opts_t plot_opts_default(void);`
- `int plot_line(char *out, size_t cap, const double *y, int n, const plot_opts_t *o);`
- `int plot_line_xy(char *out, size_t cap, const double *x, const double *y, int n, const plot_opts_t *o);`
- `int plot_scatter(char *out, size_t cap, const double *x, const double *y, int n, const plot_opts_t *o);`
- `int plot_area(char *out, size_t cap, const double *y, int n, const plot_opts_t *o);`
- `int plot_bar(char *out, size_t cap, const char *const *labels, const double *v, int n, const plot_opts_t *o);`
- `int plot_column(char *out, size_t cap, const char *const *labels, const double *v, int n, const plot_opts_t *o);`
- `int plot_hist(char *out, size_t cap, const double *samples, int n, int bins, const plot_opts_t *o);`
- `int plot_heatmap(char *out, size_t cap, const double *grid, int rows, int cols, const plot_opts_t *o);`
- `int plot_boxplot(char *out, size_t cap, const double *samples, int n, const plot_opts_t *o);`
- `int plot_candlestick(char *out, size_t cap, const double *open, const double *high, const double *low, const double *close, int n, const plot_opts_t *o);`
- `int plot_gauge(char *out, size_t cap, double value, double lo, double hi, const plot_opts_t *o);`
- `int plot_sparkline(char *out, size_t cap, const double *y, int n, const plot_opts_t *o);`
- `int plot_pie(char *out, size_t cap, const char *const *labels, const double *v, int n, const plot_opts_t *o);`
- `int plot_waterfall(char *out, size_t cap, const char *const *labels, const double *deltas, int n, const plot_opts_t *o);`
- `int plot_bullet(char *out, size_t cap, const char *label, double value, double target, const double *ranges, int nr, const plot_opts_t *o);`
- `int plot_lollipop(char *out, size_t cap, const char *const *labels, const double *v, int n, const plot_opts_t *o);`
- `int plot_slope(char *out, size_t cap, const char *const *labels, const double *left, const double *right, int n, const plot_opts_t *o);`
- `int plot_ecdf(char *out, size_t cap, const double *samples, int n, const plot_opts_t *o);`
- `int plot_calendar(char *out, size_t cap, const double *v, int n, const plot_opts_t *o);`
- `int plot_ridgeline(char *out, size_t cap, const char *const *names, const double *const *series, const int *lens, int n_series, const plot_opts_t *o);`
- `int plot_violin(char *out, size_t cap, const double *samples, int n, const plot_opts_t *o);`
- `int plot_bignum(char *out, size_t cap, double value, const char *caption, const plot_opts_t *o);`
- `int plot_attractor(char *out, size_t cap, const char *kind, double a, double b, double c, double d, long iters, const plot_opts_t *o);`
- `int plot_fractal(char *out, size_t cap, const char *set, double cx, double cy, double scale, double jx, double jy, int iters, const plot_opts_t *o);`
- `int plot_dispatch(const char *input_json, char *out, size_t cap);`

## `plugin.h`

Function-like declarations: 10

### Declarations

- `void plugin_init(plugin_registry_t *reg);`
- `void plugin_reload(plugin_registry_t *reg);`
- `void plugin_cleanup(plugin_registry_t *reg);`
- `const tool_def_t *plugin_get_tools(const plugin_registry_t *reg, int *count);`
- `void plugin_list(const plugin_registry_t *reg, char *out, size_t out_len);`
- `bool plugin_load(plugin_registry_t *reg, const char *path);`
- `bool plugin_unload(plugin_registry_t *reg, const char *name);`
- `bool plugin_validate_manifest_file(const char *path, char *out, size_t out_len);`
- `bool plugin_validate_lockfile_file(const char *path, char *out, size_t out_len);`
- `bool plugin_validate_manifest_and_lock(const char *manifest_path, const char *lock_path, char *out, size_t out_len);`

## `presence.h`

Function-like declarations: 8

### Declarations

- `typedef void (*presence_lock_fn)(void *ctx);`
- `void presence_init(int idle_threshold_s, presence_lock_fn cb, void *ctx);`
- `void presence_start(void);`
- `void presence_stop(void);`
- `double presence_idle_seconds(void);`
- `bool presence_is_locked(void);`
- `void presence_mark_unlocked(void);`
- `void presence_poke(void);`

## `project.h`

Function-like declarations: 24

### Declarations

- `void dsco_ring_init(dsco_ring_t *r, size_t cap);`
- `void dsco_ring_free(dsco_ring_t *r);`
- `size_t dsco_ring_write(dsco_ring_t *r, const char *data, size_t len);`
- `size_t dsco_ring_snapshot(dsco_ring_t *r, char *out, size_t out_cap);`
- `const char *dsco_project_registry_root(void);`
- `int dsco_project_registry_ensure(void);`
- `int dsco_project_create(const char *root, const char *name, dsco_project_t **out);`
- `int dsco_project_open(const char *id_or_name_or_path, dsco_project_t **out);`
- `int dsco_project_save(const dsco_project_t *p);`
- `int dsco_project_close(dsco_project_t *p);`
- `void dsco_project_free(dsco_project_t *p);`
- `int dsco_project_archive(const char *id);`
- `int dsco_project_list(dsco_project_summary_t *out, int max, bool include_archived);`
- `int dsco_project_start(dsco_project_t *p, const char *api_key);`
- `int dsco_project_pause(dsco_project_t *p);`
- `int dsco_project_resume(dsco_project_t *p);`
- `int dsco_project_kill(dsco_project_t *p);`
- `int dsco_project_send_input(dsco_project_t *p, const char *line);`
- `int dsco_project_drain_output(dsco_project_t *p);`
- `int dsco_project_poll_fd(const dsco_project_t *p);`
- `size_t dsco_project_snapshot(dsco_project_t *p, char *out, size_t out_cap);`
- `double dsco_project_activity_bps(dsco_project_t *p);`
- `void dsco_project_activity_ring(dsco_project_t *p, double *out16);`
- `const char *dsco_project_state_name(dsco_project_state_t s);`

## `project_grid.h`

Function-like declarations: 21

### Declarations

- `void dsco_grid_init(dsco_grid_t *g);`
- `void dsco_grid_set_preset(dsco_grid_t *g, int rows, int cols);`
- `void dsco_grid_cycle_preset(dsco_grid_t *g);`
- `void dsco_grid_assign_projects(dsco_grid_t *g, int project_count);`
- `int dsco_grid_split(dsco_grid_t *g, int tile_id, dsco_tile_kind_t dir);`
- `void dsco_grid_zoom_toggle(dsco_grid_t *g);`
- `void dsco_grid_focus(dsco_grid_t *g, int tile_id);`
- `void dsco_grid_focus_dir(dsco_grid_t *g, int dx, int dy);`
- `void dsco_grid_focus_project(dsco_grid_t *g, int project_idx);`
- `int dsco_grid_find_leaf(const dsco_grid_t *g, int project_idx);`
- `typedef void (*dsco_grid_leaf_cb)(const dsco_tile_t *t, void *ctx);`
- `void dsco_grid_foreach_leaf(const dsco_grid_t *g, dsco_grid_leaf_cb cb, void *ctx);`
- `void dsco_grid_cycle_color_mode(dsco_grid_t *g);`
- `void dsco_grid_cycle_border(dsco_grid_t *g);`
- `void dsco_grid_cycle_theme(dsco_grid_t *g);`
- `dsco_rgb_t dsco_grid_project_color(const char *project_id);`
- `dsco_rgb_t dsco_grid_state_color(int state);`
- `dsco_rgb_t dsco_grid_hsl_to_rgb(float h_deg, float s, float l);`
- `dsco_rgb_t dsco_grid_lerp(dsco_rgb_t a, dsco_rgb_t b, float t);`
- `void dsco_grid_layout(dsco_grid_t *g, int row0, int col0, int rows, int cols);`
- `dsco_border_glyphs_t dsco_grid_border_glyphs(dsco_border_t b);`

## `project_mux.h`

Function-like declarations: 3

### Declarations

- `int dsco_mux_run(const char *api_key, const char *initial_root);`
- `int dsco_mux_spawn_worker(dsco_project_t *p, const char *api_key);`
- `int dsco_mux_kill_worker(dsco_project_t *p);`

## `prompt_pool.h`

Function-like declarations: 8

### Declarations

- `void prompt_pool_init(void);`
- `void prompt_pool_shutdown(void);`
- `int prompt_pool_count(void);`
- `bool prompt_pool_suggestion(char *out, size_t out_sz);`
- `unsigned prompt_pool_bucket(void);`
- `bool prompt_pool_add(const char *prompt, const char *src);`
- `int prompt_pool_refresh_now(void);`
- `void prompt_pool_register_tool(void);`

## `provider.h`

Function-like declarations: 55

### Declarations

- `char *(*build_request)(provider_t *p, conversation_t *conv, session_state_t *session, int max_tokens, const char *credential);`
- `struct curl_slist *(*build_headers)(provider_t *p, const char *api_key);`
- `stream_result_t (*stream)(provider_t *p, const char *api_key, const char *request_json, stream_text_cb text_cb, stream_tool_start_cb tool_cb, stream_tool_arg_delta_cb tool_delta_cb, stream_thinking_cb thinking_cb, void *cb_ctx);`
- `provider_t *provider_create(const char *name);`
- `void provider_free(provider_t *p);`
- `bool provider_prepare(provider_t *p);`
- `stream_result_t provider_stream_reuse(provider_t *p, const char *api_key, const char *request_json, stream_text_cb text_cb, stream_tool_start_cb tool_cb, stream_tool_arg_delta_cb tool_delta_cb, stream_thinking_cb thinking_cb, void *cb_ctx);`
- `void provider_reset_connection(provider_t *p);`
- `const char *provider_detect(const char *model, const char *api_key);`
- `const char *provider_model_family(const char *model);`
- `bool provider_model_supports_cache_control(const char *model);`
- `bool provider_model_supports_automatic_prompt_cache(const char *model);`
- `bool provider_model_supports_prompt_cache_key(const char *model);`
- `bool provider_model_supports_prompt_cache_retention(const char *model);`
- `const char *provider_resolve_api_key(const char *provider_name);`
- `bool provider_has_custom_api_base(const char *provider_name);`
- `bool provider_has_usable_key(const char *provider_name, const char *fallback_api_key);`
- `bool provider_is_local_endpoint(const char *provider_name);`
- `const char *provider_route_for_model(const char *model, const char *fallback_api_key, const char *provider_override);`
- `const char *provider_resolve_request_api_key(const char *provider_name, const char *fallback_api_key);`
- `void provider_export_child_process_credentials_for_provider(const char *provider_name, const char *resolved_key);`
- `void provider_export_child_process_credentials(const char *model, const char *resolved_key);`
- `bool provider_debug_auth_enabled(void);`
- `const char *provider_auth_mode(const char *provider_name, const char *resolved_key);`
- `bool provider_usage_is_included(const char *provider_name, const char *resolved_key);`
- `long provider_last_subscription_queue_ms(void);`
- `void provider_debug_log_request(const char *provider_name, const char *model, const char *resolved_key);`
- `const char *provider_claude_code_oauth_source(void);`
- `bool provider_claude_code_import_credentials(void);`
- `const char *provider_claude_code_metadata_user_id(void);`
- `const char *provider_claude_code_session_id(void);`
- `bool provider_sakana_current_key_is_subscription(void);`
- `const char *provider_sakana_subscription_request_key(void);`
- `bool provider_sakana_has_payg_key(void);`
- `const char *provider_sakana_payg_request_key(void);`
- `bool provider_claude_code_get_account_info(char *subscription_type_out, size_t st_len, char *rate_limit_tier_out, size_t rl_len);`
- `bool provider_msg_is_credit_too_low(const char *msg);`
- `bool provider_chatgpt_429_is_quota(const char *error_json_or_message);`
- `bool provider_stream_result_is_credit_exhausted(const char *provider_name, const stream_result_t *result);`
- `bool provider_msg_is_gated(const char *msg);`
- `time_t provider_credit_reset_at_from_value(const char *value, time_t now);`
- `time_t provider_credit_reset_at_from_text(const char *text, time_t now);`
- `bool provider_credit_reset_at_from_header_line(const char *line, time_t now, time_t *reset_at);`
- `bool provider_msg_is_context_overflow(const char *msg);`
- `bool provider_model_supports_cache_control(const char *model);`
- `bool provider_model_is_routable(const char *model, const char *fallback_api_key, const char *provider_override, const char **out_provider_name);`
- `int provider_build_default_fallback_models(const char *model, char out_models[][128], int max_models);`
- `const char *provider_select_default_primary_model(bool prefer_code);`
- `const char *provider_provider_for_api_key(const char *api_key);`
- `const char *provider_publish_api_key_env(const char *api_key);`
- `const char *provider_primary_model_for(const char *family, bool prefer_code);`
- `bool provider_test_parse_openai_sse(const char *bytes, size_t len, provider_test_openai_sse_result_t *out);`
- `bool provider_test_parse_openai_sse_for_model(const char *bytes, size_t len, const char *source_provider, const char *request_model, provider_test_openai_sse_result_t *out);`
- `void provider_test_free_openai_sse_result(provider_test_openai_sse_result_t *result);`
- `long provider_test_chatgpt_retry_after_ms(const char *text);`

## `provider_pool.h`

Function-like declarations: 10

### Declarations

- `provider_pool_t *provider_pool(void);`
- `void provider_pool_init(const char *session_key);`
- `provider_t *provider_pool_acquire(const char *name);`
- `provider_slot_t *provider_pool_slot(const char *name);`
- `void provider_pool_report(const char *name, bool ok, double latency_ms);`
- `void provider_pool_mark_subscription_exhausted(const char *name, time_t exhausted_until);`
- `time_t provider_pool_subscription_exhausted_until(const char *name);`
- `bool provider_pool_healthy(const char *name);`
- `void provider_pool_render(char *out, size_t out_len);`
- `void provider_pool_shutdown(void);`

## `provider_profiles.h`

Function-like declarations: 11

### Declarations

- `size_t provider_profile_count(void);`
- `const provider_profile_t *provider_profile_at(size_t index);`
- `const provider_profile_t *provider_profile_find(const char *name_or_alias);`
- `const char *provider_profile_canonical_name(const char *name_or_alias);`
- `const char *provider_api_mode_name(provider_api_mode_t mode);`
- `const char *provider_auth_type_name(provider_auth_type_t auth_type);`
- `const char *provider_transport_kind_name(provider_transport_kind_t transport);`
- `const char *provider_profile_primary_env_var(const provider_profile_t *profile);`
- `bool provider_profile_has_env_var(const provider_profile_t *profile, const char *env_var);`
- `bool provider_profile_has_alias(const provider_profile_t *profile, const char *alias);`
- `bool provider_profile_transport_supported(const provider_profile_t *profile);`

## `px_backend.h`

Function-like declarations: 14

### Declarations

- `typedef struct { void (*begin_frame)(void *surface, native_ui_rect_t viewport, const native_ui_damage_t *damage);`
- `void (*push_clip)(void *surface, native_ui_rect_t rect);`
- `void (*pop_clip)(void *surface);`
- `void (*fill_rect)(void *surface, native_ui_rect_t rect, px_backend_color_t color, uint8_t opacity, uint8_t radius, bool raised);`
- `void (*stroke_rect)(void *surface, native_ui_rect_t rect, px_backend_color_t color, uint8_t opacity, uint8_t width, uint8_t radius);`
- `void (*draw_text)(void *surface, native_ui_rect_t rect, const char *text, native_ui_type_token_t type, px_backend_color_t color, uint8_t opacity);`
- `void (*draw_icon)(void *surface, native_ui_rect_t rect, const char *name, px_backend_color_t color, uint8_t opacity);`
- `void (*draw_line)(void *surface, int x0, int y0, int x1, int y1, px_backend_color_t color, uint8_t opacity);`
- `void (*fill_circle)(void *surface, int cx, int cy, int radius, px_backend_color_t color, uint8_t opacity);`
- `void (*draw_custom)(void *surface, const native_ui_node_t *node, const px_backend_palette_t *palette);`
- `void (*end_frame)(void *surface);`
- `void px_backend_init(px_backend_t *backend, void *surface, const px_backend_palette_t *palette, const px_backend_ops_t *ops);`
- `const native_ui_backend_t *px_backend_native_vtable(void);`
- `px_backend_color_t px_backend_resolve_color(const px_backend_t *backend, native_ui_color_token_t token);`
- `const native_ui_damage_t *px_backend_last_damage(const px_backend_t *backend);`

## `px_theme.h`

Function-like declarations: 7

### Declarations

- `int px_theme_count(void);`
- `const px_theme_t *px_theme_get(int index);`
- `const px_theme_t *px_theme_find(const char *name);`
- `const px_theme_t *px_theme_active(void);`
- `const px_theme_t *px_theme_set_active(const char *name);`
- `const px_theme_t *px_theme_cycle(int direction);`
- `px_backend_palette_t px_theme_palette(const px_theme_t *theme);`

## `px_widgets.h`

Function-like declarations: 14

### Declarations

- `native_ui_color_token_t px_widget_tone_token(px_widget_tone_t tone);`
- `static inline uint64_t px_widget_subkey(uint64_t key, unsigned child) { return key ^ ((uint64_t)(child + 1) << 56) ^ UINT64_C(0x5057494447);`
- `} int px_widget_card(native_ui_scene_t *scene, int parent, uint64_t key, const char *title);`
- `int px_widget_kv_row(native_ui_scene_t *scene, int parent, uint64_t key, const char *label, const char *value);`
- `int px_widget_badge(native_ui_scene_t *scene, int parent, uint64_t key, const char *text, px_widget_tone_t tone);`
- `int px_widget_meter(native_ui_scene_t *scene, int parent, uint64_t key, const char *label, double value, px_widget_tone_t tone);`
- `int px_widget_sparkline(native_ui_scene_t *scene, int parent, uint64_t key, const double *values, int count, px_widget_tone_t tone);`
- `int px_widget_progress(native_ui_scene_t *scene, int parent, uint64_t key, double fraction, double phase, px_widget_tone_t tone);`
- `int px_widget_gauge(native_ui_scene_t *scene, int parent, uint64_t key, const char *label, double value, px_widget_tone_t tone);`
- `int px_widget_bar_chart(native_ui_scene_t *scene, int parent, uint64_t key, const double *values, int count, px_widget_tone_t tone);`
- `int px_widget_spinner(native_ui_scene_t *scene, int parent, uint64_t key, double phase, px_widget_tone_t tone);`
- `int px_widget_activity_dots(native_ui_scene_t *scene, int parent, uint64_t key, double phase, px_widget_tone_t tone);`
- `int px_widget_agent_card(native_ui_scene_t *scene, int parent, uint64_t key, const px_widget_agent_t *agent);`
- `int px_widget_toast(native_ui_scene_t *scene, int parent, uint64_t key, const char *text, px_widget_tone_t tone);`

## `realtime.h`

Function-like declarations: 5

### Declarations

- `int realtime_voice_run(const realtime_opts_t *opts);`
- `const char *realtime_default_reasoning_effort(void);`
- `int realtime_voice_default_max_tools(void);`
- `int realtime_voice_select_tool_names_for_context( const char *context, char names[][DSCO_REALTIME_TOOL_NAME_MAX], int max_tools);`
- `int realtime_voice_cli(int argc, char **argv);`

## `recovery.h`

Function-like declarations: 10

### Declarations

- `typedef bool (*recovery_fn_t)(void *arg, char *errbuf, size_t errlen);`
- `bool execute_with_retry(recovery_fn_t fn, void *arg, const retry_config_t *cfg, char *errbuf, size_t errlen);`
- `bool execute_with_fallback(recovery_fn_t primary_fn, recovery_fn_t *fallback_fns, int n_fallbacks, void *arg, char *errbuf, size_t errlen);`
- `int backtrack_and_replay(int plan_id, int distance);`
- `void recovery_log_init(recovery_log_t *log);`
- `void recovery_log_record(recovery_log_t *log, const recovery_log_entry_t *entry);`
- `int recovery_log_count(const recovery_log_t *log);`
- `const recovery_log_entry_t *recovery_log_get(const recovery_log_t *log, int idx);`
- `int recovery_log_dump(const recovery_log_t *log, const char *path);`
- `const char *recovery_action_name(recovery_action_t a);`

## `remote_cli.h`

Function-like declarations: 3

### Declarations

- `int dsco_remote_cli(int argc, char **argv);`
- `int dsco_fleet_cli(int argc, char **argv);`
- `bool dsco_fleet_resolve(const char *peer, char *user, size_t ul, char *addr, size_t al);`

## `rich_text.h`

Function-like declarations: 2

### Declarations

- `size_t rich_text_parse(const char *markdown, rich_token_t *tokens, size_t capacity);`
- `size_t rich_text_latex_to_unicode(const char *latex, char *out, size_t out_size);`

## `ring_buffer.h`

Function-like declarations: 4

### Declarations

- `void ring_init(metric_ring_t *r);`
- `void ring_push(metric_ring_t *r, const metric_sample_t *s);`
- `size_t ring_count(const metric_ring_t *r);`
- `const metric_sample_t *ring_get(const metric_ring_t *r, size_t idx);`

## `rl_hooks.h`

Function-like declarations: 3

### Declarations

- `bool rl_hooks_record_event(const rl_hook_event_t *event);`
- `bool rl_hooks_format_event(const rl_hook_event_t *event, char *out, size_t out_len);`
- `bool rl_hooks_inspect_run(const char *run_id, char *out, size_t out_len);`

## `rollout.h`

Function-like declarations: 17

### Declarations

- `int (*actions_fn)(rollout_state_t s, rollout_action_t *out, int max, void *ud);`
- `rollout_step_result_t (*execute_fn)(rollout_state_t s, rollout_action_t a, void *ud);`
- `rollout_step_result_t (*model_fn)(rollout_state_t s, rollout_action_t a, void *ud);`
- `rollout_regime_t (*regime_fn)(rollout_action_t a, void *ud);`
- `bool (*is_terminal_fn)(rollout_state_t s, void *ud);`
- `void (*state_hash_fn)(rollout_state_t s, char *out, size_t out_len, void *ud);`
- `void (*action_key_fn)(rollout_action_t a, char *out, size_t out_len, void *ud);`
- `double (*action_cost_fn)(rollout_action_t a, void *ud);`
- `rollout_state_t (*clone_state_fn)(rollout_state_t s, void *ud);`
- `void (*free_state_fn)(rollout_state_t s, void *ud);`
- `rollout_action_t (*clone_action_fn)(rollout_action_t a, void *ud);`
- `void (*free_action_fn)(rollout_action_t a, void *ud);`
- `void rollout_config_defaults(rollout_config_t *cfg);`
- `rollout_result_t *rollout_search(const rollout_env_t *env, const rollout_config_t *cfg);`
- `void rollout_result_free(rollout_result_t *r);`
- `char *rollout_result_to_plan_json(const rollout_result_t *r, const char *task);`
- `rollout_step_result_t rollout_realize_step(const rollout_env_t *env, rollout_state_t s, rollout_action_t a);`

## `router.h`

Function-like declarations: 17

### Declarations

- `void router_init(router_t *r, router_policy_t policy);`
- `void router_destroy(router_t *r);`
- `task_complexity_t router_classify_task(const char *user_msg, int conversation_turns, int tool_calls_last_turn, int context_token_pct);`
- `router_decision_t router_decide(router_t *r, const char *current_model, task_complexity_t complexity, double session_cost_so_far, double last_latency_ms, int consecutive_failures);`
- `void router_record_turn(router_t *r, const char *model_id, int input_tokens, int output_tokens, double latency_ms, double cost_usd, double tokens_per_sec, bool success);`
- `const char *router_cheaper_model(const char *current_model);`
- `const char *router_stronger_model(const char *current_model);`
- `const char *router_fastest_model(task_complexity_t min_complexity);`
- `router_model_stat_t *router_get_stats(router_t *r, const char *model_id);`
- `int router_to_json(router_t *r, char *buf, size_t len);`
- `int router_history_to_json(router_t *r, char *buf, size_t len);`
- `const char *router_policy_name(router_policy_t p);`
- `router_policy_t router_policy_parse(const char *s);`
- `const char *task_complexity_name(task_complexity_t c);`
- `task_complexity_t task_complexity_parse(const char *s);`
- `const char *switch_reason_name(switch_reason_t r);`
- `int model_tier(const char *model_id);`

## `rsi_curriculum.h`

Function-like declarations: 7

### Declarations

- `const rsi_skill_t *rsi_curriculum_skills(int *count);`
- `const rsi_category_t *rsi_curriculum_categories(int *count);`
- `const rsi_skill_t *rsi_curriculum_find_skill(const char *id);`
- `bool rsi_curriculum_validate_promotion(const rsi_eval_summary_t *candidate, char *reason, size_t reason_len);`
- `int rsi_curriculum_summary_json(char *buf, size_t len);`
- `int rsi_curriculum_skill_json(const char *skill_id, char *buf, size_t len);`
- `int rsi_curriculum_gate_json(const rsi_eval_summary_t *candidate, char *buf, size_t len);`

## `rtf.h`

Function-like declarations: 2

### Declarations

- `int rtf_render_markdown(FILE *out, const char *markdown);`
- `int rtf_render_file(const char *in_path, const char *out_path);`

## `scheduler.h`

Function-like declarations: 16

### Declarations

- `typedef int (*task_func_t)(void *ctx);`
- `void sched_init(scheduler_t *s);`
- `void sched_destroy(scheduler_t *s);`
- `task_id_t sched_spawn(scheduler_t *s, task_func_t func, void *ctx, const char *label, sched_priority_t prio);`
- `bool sched_cancel(scheduler_t *s, task_id_t id);`
- `sched_task_t *sched_task_get(scheduler_t *s, task_id_t id);`
- `void sched_wait_fd(scheduler_t *s, int fd);`
- `void sched_sleep(scheduler_t *s, int ms);`
- `void sched_wait_task(scheduler_t *s, task_id_t other);`
- `int sched_tick(scheduler_t *s);`
- `int sched_run(scheduler_t *s, int poll_ms);`
- `void sched_stop(scheduler_t *s);`
- `int sched_run_sync(scheduler_t *s, task_func_t func, void *ctx, const char *label);`
- `int sched_count_by_state(scheduler_t *s, task_state_t state);`
- `sched_stats_t sched_get_stats(scheduler_t *s);`
- `void sched_dump(scheduler_t *s);`

## `se_store.h`

Function-like declarations: 5

### Declarations

- `bool se_store_init(uint8_t out_key[32]);`
- `void se_store_wipe(void);`
- `bool se_store_available(void);`
- `int se_store_sign(const uint8_t *data, size_t data_len, uint8_t *sig_buf, size_t sig_buf_len);`
- `bool se_store_pubkey(uint8_t out[65]);`

## `sealed_store.h`

Function-like declarations: 9

### Declarations

- `void sealed_store_init(void);`
- `void sealed_store_set_master_key(const uint8_t key[32]);`
- `bool sealed_store_master_key_copy(uint8_t out[32]);`
- `int sealed_store_put(const char *key, const char *val, size_t val_len);`
- `int sealed_store_get(const char *key, char *buf, size_t buf_len);`
- `void sealed_store_wipe(const char *key);`
- `void sealed_store_wipe_all(void);`
- `const char *sealed_getenv(const char *key);`
- `const char *sealed_store_peek(const char *key);`

## `self_improve.h`

Function-like declarations: 21

### Declarations

- `void self_improve_init(self_improve_t *si);`
- `void self_improve_record_tool(self_improve_t *si, const char *tool_name, bool success, double latency_ms, int estimated_tokens);`
- `void self_improve_record_turn(self_improve_t *si, int turn_number, double turn_cost, int input_tokens, int output_tokens, int context_usage_pct, double budget_used_pct);`
- `void self_improve_record_turn_usage(self_improve_t *si, int turn_number, double turn_cost, int input_tokens, int output_tokens, int cache_read_tokens, int cache_write_tokens, int context_usage_pct, double budget_used_pct);`
- `int self_improve_consolidate(self_improve_t *si);`
- `bool self_improve_load_history(self_improve_t *si);`
- `bool self_improve_save_history(const self_improve_t *si);`
- `const char *self_improve_summary(const self_improve_t *si, char *buf, size_t buf_len);`
- `const si_strategy_weights_t *self_improve_weights(const self_improve_t *si);`
- `void self_improve_acknowledge(self_improve_t *si, int suggestion_idx);`
- `double self_improve_tool_score(const self_improve_t *si, const char *tool_name);`
- `bool self_improve_tool_failing(const self_improve_t *si, const char *tool_name);`
- `void self_improve_record_redundant(self_improve_t *si, const char *tool_name);`
- `bool self_improve_suggest_alternative(const self_improve_t *si, const char *tool_name, char *alt_name, size_t alt_len);`
- `void self_improve_turn_reset(self_improve_t *si);`
- `bool tool_self_improve(const char *input_json, char *result, size_t result_len);`
- `bool tool_self_assess(const char *input_json, char *result, size_t result_len);`
- `self_improve_record_tool(&g_self_improve, (name), (ok), (ms), (tokens)) self_improve_record_turn(&g_self_improve, (n), (cost), (in_tok), \ (out_tok), (ctx_pct), (budget_pct)) self_improve_record_turn_usage(&g_self_improve, (n), (cost), (in_tok), (out_tok), \ (cr_tok), (cw_tok), (ctx_pct), (budget_pct)) self_improve_consolidate(&g_self_improve) self_improve_turn_reset(&g_self_improve) void self_improve_record_goal_state(self_improve_t *si, const char *goal_id, int state, int grip_strength, double elapsed_s);`
- `void self_improve_record_swarm_outcome(self_improve_t *si, const char *topology, int agents, bool success, double quality, double elapsed_s);`
- `void self_improve_record_strategy_result(self_improve_t *si, const char *strategy, const char *goal_type, bool success, int grip_escalations, double elapsed_s);`
- `void self_improve_record_tournament_result(self_improve_t *si, const char *winner_strategy, int competitors, double margin, double elapsed_s);`

## `semantic.h`

Function-like declarations: 13

### Declarations

- `void sem_tokenize(const char *text, token_list_t *out);`
- `void sem_tfidf_init(tfidf_index_t *idx);`
- `int sem_tfidf_add_doc(tfidf_index_t *idx, const char *text);`
- `void sem_tfidf_finalize(tfidf_index_t *idx);`
- `void sem_tfidf_vectorize(tfidf_index_t *idx, const char *text, tfidf_vec_t *out);`
- `double sem_cosine_sim(const tfidf_vec_t *a, const tfidf_vec_t *b);`
- `int sem_bm25_rank(tfidf_index_t *idx, const char *query, bm25_result_t *results, int max_results);`
- `void sem_tools_index_build(tfidf_index_t *idx, const char **names, const char **descriptions, int tool_count);`
- `int sem_tools_rank(tfidf_index_t *idx, const char *query, tool_score_t *results, int max_results, int tool_count);`
- `int sem_classify(const char *query, classification_t *results, int max_results);`
- `const char *sem_category_name(query_category_t cat);`
- `void semantic_set_vfs(vfs_db_t *vfs);`
- `int sem_score_messages(tfidf_index_t *idx, const char *query, const char **msg_texts, int msg_count, msg_score_t *results, int max_results);`

## `sequence_state.h`

Function-like declarations: 5

### Declarations

- `void dsco_sequence_state_init(dsco_sequence_state_t *seq, const dsco_sampling_params_t *sampling, dsco_kvblock_ref_t blocks);`
- `void dsco_sequence_state_free(dsco_sequence_state_t *seq);`
- `bool dsco_sequence_state_append(dsco_sequence_state_t *seq, int token);`
- `bool dsco_sequence_state_next(dsco_sequence_state_t *seq, int *out_token);`
- `void dsco_sequence_state_rewind(dsco_sequence_state_t *seq);`

## `session_memory.h`

Function-like declarations: 11

### Declarations

- `int session_init(session_db_t *db, const char *task_text);`
- `void session_free(session_db_t *db);`
- `int session_flush(session_db_t *db);`
- `int session_remember(session_db_t *db, const char *key, const char *value, int ttl_seconds);`
- `const char *session_recall(session_db_t *db, const char *key);`
- `int session_lookup_similar(session_db_t *db, const char *task, const session_record_t **out, int k);`
- `int session_promote(session_db_t *db);`
- `int session_persist(session_db_t *db);`
- `int session_end(session_db_t *db);`
- `int session_evict_expired(session_db_t *db);`
- `int session_status_json(const session_db_t *db, char *buf, size_t len);`

## `setup.h`

Function-like declarations: 7

### Declarations

- `const char *dsco_setup_profile_name(void);`
- `int dsco_setup_load_saved_env(void);`
- `int dsco_setup_autopopulate(bool overwrite, bool include_generic, char *summary, size_t summary_len);`
- `int dsco_setup_bootstrap_from_env(char *summary, size_t summary_len);`
- `int dsco_setup_report(char *out, size_t out_len);`
- `const char *dsco_setup_env_path(void);`
- `bool dsco_setup_set_key(const char *key, const char *value);`

## `shadeexpr.h`

Function-like declarations: 4

### Declarations

- `shadeexpr_t *shadeexpr_compile(const char *src, char *errbuf, size_t errcap);`
- `double shadeexpr_eval(const shadeexpr_t *e, const double *slots);`
- `const char *shadeexpr_source(const shadeexpr_t *e);`
- `void shadeexpr_free(shadeexpr_t *e);`

## `simd.h`

Function-like declarations: 91

### Declarations

- `for (size_t i = 0; i < tail; i++) { if (p[len - 1 - i] == n) return (ssize_t)(len - 1 - i);`
- `uint8x16_t needle_v = vdupq_n_u8(n);`
- `uint8x16_t v = vld1q_u8(p + off);`
- `uint8x16_t eq = vceqq_u8(v, needle_v);`
- `uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 0);`
- `uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 1);`
- `if (hi) { int bit = 63 - __builtin_clzll(hi);`
- `return (ssize_t)(off + 8 + (bit >> 3));`
- `} int bit = 63 - __builtin_clzll(lo);`
- `return (ssize_t)(off + (bit >> 3));`
- `for (size_t i = 0; i < tail; i++) { if (p[len - 1 - i] == n) return (ssize_t)(len - 1 - i);`
- `__m128i needle_v = _mm_set1_epi8((char)n);`
- `__m128i v = _mm_loadu_si128((const __m128i *)(p + off));`
- `__m128i eq = _mm_cmpeq_epi8(v, needle_v);`
- `unsigned m = (unsigned)_mm_movemask_epi8(eq);`
- `int bit = 31 - __builtin_clz(m);`
- `return (ssize_t)(off + bit);`
- `uint8x16_t needle_v = vdupq_n_u8(n);`
- `for (; i + 16 <= len; i += 16) { uint8x16_t v = vld1q_u8(p + i);`
- `uint8x16_t eq = vceqq_u8(v, needle_v);`
- `uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 0);`
- `uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 1);`
- `if (lo) { int bit = __builtin_ctzll(lo);`
- `return (ssize_t)(i + (bit >> 3));`
- `} int bit = __builtin_ctzll(hi);`
- `return (ssize_t)(i + 8 + (bit >> 3));`
- `__m128i needle_v = _mm_set1_epi8((char)n);`
- `for (; i + 16 <= len; i += 16) { __m128i v = _mm_loadu_si128((const __m128i *)(p + i));`
- `__m128i eq = _mm_cmpeq_epi8(v, needle_v);`
- `unsigned m = (unsigned)_mm_movemask_epi8(eq);`
- `int bit = __builtin_ctz(m);`
- `return (ssize_t)(i + bit);`
- `uint8x16_t needle_v = vdupq_n_u8(n);`
- `for (; i + 16 <= len; ) { uint8x16_t acc = vdupq_n_u8(0);`
- `for (size_t b = 0; b < blocks; b++) { uint8x16_t v = vld1q_u8(p + i + (b << 4));`
- `uint8x16_t eq = vceqq_u8(v, needle_v);`
- `acc = vsubq_u8(acc, eq);`
- `} count += (size_t)vaddlvq_u8(acc);`
- `__m128i needle_v = _mm_set1_epi8((char)n);`
- `for (; i + 16 <= len; i += 16) { __m128i v = _mm_loadu_si128((const __m128i *)(p + i));`
- `__m128i eq = _mm_cmpeq_epi8(v, needle_v);`
- `count += (size_t)__builtin_popcount((unsigned)_mm_movemask_epi8(eq));`
- `const uint8x16_t q = vdupq_n_u8('"');`
- `const uint8x16_t bs = vdupq_n_u8('\\');`
- `const uint8x16_t z = vdupq_n_u8(0);`
- `const uint8_t *base = (const uint8_t *)(addr & ~(uintptr_t)15);`
- `size_t off = (size_t)(addr - (uintptr_t)base);`
- `uint8x16_t v = vld1q_u8(base);`
- `uint8x16_t m = vorrq_u8(vorrq_u8(vceqq_u8(v, q), vceqq_u8(v, bs)), vceqq_u8(v, z));`
- `uint8x8_t nb = vshrn_n_u16(vreinterpretq_u16_u8(m), 4);`
- `uint64_t bits = vget_lane_u64(vreinterpret_u64_u8(nb), 0);`
- `bits &= (~0ULL << (off * 4));`
- `for (const uint8_t *cur = base + 16;; cur += 16) { v = vld1q_u8(cur);`
- `m = vorrq_u8(vorrq_u8(vceqq_u8(v, q), vceqq_u8(v, bs)), vceqq_u8(v, z));`
- `nb = vshrn_n_u16(vreinterpretq_u16_u8(m), 4);`
- `bits = vget_lane_u64(vreinterpret_u64_u8(nb), 0);`
- `return (size_t)(cur - s) + ((size_t)__builtin_ctzll(bits) >> 2);`
- `} const __m128i q = _mm_set1_epi8('"');`
- `const __m128i bs = _mm_set1_epi8('\\');`
- `const __m128i z = _mm_setzero_si128();`
- `const uint8_t *base = (const uint8_t *)(addr & ~(uintptr_t)15);`
- `size_t off = (size_t)(addr - (uintptr_t)base);`
- `__m128i v = _mm_load_si128((const __m128i *)base);`
- `__m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, bs)), _mm_cmpeq_epi8(v, z));`
- `unsigned bits = (unsigned)_mm_movemask_epi8(m) & (~0u << off);`
- `for (const uint8_t *cur = base + 16;; cur += 16) { v = _mm_load_si128((const __m128i *)cur);`
- `m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, bs)), _mm_cmpeq_epi8(v, z));`
- `unsigned b2 = (unsigned)_mm_movemask_epi8(m);`
- `return (size_t)(cur - s) + (size_t)__builtin_ctz(b2);`
- `const uint8x16_t lo = vdupq_n_u8(0x20);`
- `const uint8x16_t hi = vdupq_n_u8(0x7e);`
- `const uint8x16_t q = vdupq_n_u8('"');`
- `const uint8x16_t bs = vdupq_n_u8('\\');`
- `for (; i + 16 <= len; i += 16) { uint8x16_t v = vld1q_u8(p + i);`
- `uint8x16_t bad = vcltq_u8(v, lo);`
- `bad = vorrq_u8(bad, vcgtq_u8(v, hi));`
- `bad = vorrq_u8(bad, vceqq_u8(v, q));`
- `bad = vorrq_u8(bad, vceqq_u8(v, bs));`
- `uint8x8_t nb = vshrn_n_u16(vreinterpretq_u16_u8(bad), 4);`
- `uint64_t m = vget_lane_u64(vreinterpret_u64_u8(nb), 0);`
- `return i + ((size_t)__builtin_ctzll(m) >> 2);`
- `} const __m128i lo = _mm_set1_epi8(0x20);`
- `const __m128i q = _mm_set1_epi8('"');`
- `const __m128i bs = _mm_set1_epi8('\\');`
- `for (; i + 16 <= len; i += 16) { __m128i v = _mm_loadu_si128((const __m128i *)(p + i));`
- `__m128i bad = _mm_cmplt_epi8(v, lo);`
- `bad = _mm_or_si128(bad, _mm_cmpeq_epi8(v, q));`
- `bad = _mm_or_si128(bad, _mm_cmpeq_epi8(v, bs));`
- `unsigned m = (unsigned)_mm_movemask_epi8(bad);`
- `return i + (size_t)__builtin_ctz(m);`
- `while (pos > 0) { ssize_t nl = dsco_simd_rfind_byte(base, pos, '\n');`

## `skill_index.h`

Function-like declarations: 6

### Declarations

- `skill_index_t *skill_index_new(void);`
- `void skill_index_free(skill_index_t *ix);`
- `int skill_index_dim(void);`
- `int skill_index_add(skill_index_t *ix, const char *name, const float *vec, int dim);`
- `size_t skill_index_count(const skill_index_t *ix);`
- `int skill_index_query(const skill_index_t *ix, const float *query, int dim, skill_hit_t *out, int k);`

## `spend_governor.h`

Function-like declarations: 6

### Declarations

- `spend_plan_t spend_governor_plan(const spend_signals_t *sig);`
- `void spend_plan_apply_learned(spend_plan_t *plan, const spend_learned_t *lw, const spend_signals_t *sig);`
- `int spend_effort_rank(const char *effort);`
- `bool spend_effort_is_downshift(const char *current, const char *candidate);`
- `void spend_governor_default_budgets(bool had_explicit, double *session_usd, double *daily_usd);`
- `const char *spend_phase_label(spend_phase_t phase);`

## `startup.h`

Function-like declarations: 4

### Declarations

- `DSCO_CAP_TUI | DSCO_CAP_VOS | DSCO_CAP_IPC | \ DSCO_CAP_MCP | DSCO_CAP_MEMORY | DSCO_CAP_ACCEL | \ DSCO_CAP_TRUST | DSCO_CAP_TRACE) bool dsco_profile_parse(const char *name, dsco_profile_t *out);`
- `const char *dsco_profile_name(dsco_profile_t profile);`
- `const char *dsco_caps_to_string(dsco_caps_t caps, char *buf, size_t buflen);`
- `void dsco_startup_init(dsco_profile_t profile, dsco_caps_t caps);`

## `stateful_atoms.h`

Function-like declarations: 8

### Declarations

- `plan_state_t *plan_state_init(int plan_id);`
- `void plan_state_free(plan_state_t *state);`
- `const char *plan_state_get_output(const plan_state_t *state, int atom_id);`
- `bool plan_state_set_output(plan_state_t *state, int atom_id, const char *output_json);`
- `bool plan_state_checkpoint(plan_state_t *state);`
- `int plan_state_rollback(plan_state_t *state, int steps);`
- `bool execute_atom_with_input(plan_state_t *state, int atom_id, char *result_buf, size_t rlen);`
- `int plan_state_run_all(plan_state_t *state);`

## `strategy.h`

Function-like declarations: 8

### Declarations

- `typedef struct { bool (*frontier_snapshot)(double *score, bool *on_frontier, char *summary, size_t summary_len);`
- `void (*audit)(const char *category, const char *title, const char *detail);`
- `void strategy_set_hooks(const strategy_hooks_t *hooks);`
- `void strategy_reset(void);`
- `bool strategy_assess(const char *input_json, char *result, size_t rlen);`
- `int strategy_objective_count(void);`
- `const strat_objective_t *strategy_objective_at(int idx);`
- `double strategy_kelly_fraction(double p, double b);`
- `void strategy_register_tool(void);`

## `structured_process.h`

Function-like declarations: 5

### Declarations

- `const char *structured_process_schema_json(void);`
- `int structured_process_schema_response_format_json(char *buf, size_t len);`
- `bool structured_process_classify(const char *input, sp_classification_t *out);`
- `int structured_process_synthesize_json(const char *input, char *buf, size_t len);`
- `int structured_process_create_plan_from_json(const char *process_json);`

## `subscription_bench.h`

Function-like declarations: 5

### Declarations

- `size_t dsco_subscription_lane_count(void);`
- `const dsco_subscription_lane_spec_t *dsco_subscription_lane_at(size_t index);`
- `bool dsco_subscription_lane_native_ready(const dsco_subscription_lane_spec_t *lane, const char *fallback_api_key, char *auth_mode, size_t auth_mode_len, char *endpoint, size_t endpoint_len, char *reason, size_t reason_len);`
- `int dsco_subscription_lanes_print_json(FILE *out, const char *fallback_api_key);`
- `int dsco_subscription_bench_run(FILE *out, const dsco_subscription_bench_options_t *options);`

## `subscription_gate.h`

Function-like declarations: 2

### Declarations

- `bool subscription_gate_acquire(subscription_gate_t *gate, const char *scope, const volatile int *interrupted, long *waited_ms);`
- `void subscription_gate_release(subscription_gate_t *gate, long cooldown_ms);`

## `supervisor.h`

Function-like declarations: 6

### Declarations

- `int supervisor_run(int child_argc, char **child_argv);`
- `bool supervisor_resolve_hotswap_exec(const char *current_path, const char *explicit_path, char *out, size_t out_len);`
- `void dsco_maybe_exec_shell_to_keep_terminal(void);`
- `void main_install_crash_handlers(void);`
- `int supervisor_metrics_dump(metrics_format_t format, const char *output_path);`
- `size_t supervisor_metrics_count(void);`

## `swarm.h`

Function-like declarations: 52

### Declarations

- `static inline int dsco_swarm_max_children(void) { return dsco_env_int("DSCO_SWARM_MAX_CHILDREN", SWARM_MAX_CHILDREN, 1, SWARM_MAX_CHILDREN);`
- `} static inline int dsco_swarm_max_groups(void) { return dsco_env_int("DSCO_SWARM_MAX_GROUPS", SWARM_MAX_GROUPS, 1, SWARM_MAX_GROUPS);`
- `} static inline int dsco_swarm_max_depth(void) { return dsco_env_int("DSCO_SWARM_MAX_DEPTH", SWARM_MAX_DEPTH, 0, SWARM_MAX_DEPTH);`
- `} static inline size_t dsco_swarm_max_output(void) { return dsco_env_size("DSCO_SWARM_MAX_OUTPUT", SWARM_MAX_OUTPUT, 4096, SWARM_MAX_OUTPUT);`
- `} static inline size_t dsco_swarm_read_buf(void) { return dsco_env_size("DSCO_SWARM_READ_BUF", SWARM_READ_BUF, 1024, SWARM_READ_BUF);`
- `typedef void (*swarm_stream_cb)(int child_id, const char *data, size_t len, void *ctx);`
- `void swarm_init(swarm_t *s, const char *api_key, const char *model);`
- `void swarm_destroy(swarm_t *s);`
- `int swarm_spawn(swarm_t *s, const char *task, const char *model);`
- `int swarm_spawn_in_group(swarm_t *s, int group_id, const char *task, const char *model);`
- `void swarm_set_next_instance(const char *effort, double temperature, double top_p, int top_k, int thinking_budget, const char *tool_choice, const char *system_prompt);`
- `int swarm_spawn_provider(swarm_t *s, int group_id, const char *task, const char *model, const char *provider);`
- `int swarm_spawn_provider_auth_lane(swarm_t *s, int group_id, const char *task, const char *model, const char *provider, const char *auth_class);`
- `int swarm_spawn_openrouter_lane(swarm_t *s, int group_id, const char *task, const char *model, const char *upstream, const char *quantization);`
- `int swarm_spawn_executor(swarm_t *s, int group_id, const char *task, const char *model, executor_type_t executor);`
- `void swarm_detect_executors(swarm_t *s);`
- `void swarm_prepare_executor_env(swarm_t *s, executor_type_t executor);`
- `const char *executor_type_name(executor_type_t t);`
- `void swarm_set_budget(swarm_t *s, double budget_usd);`
- `double swarm_budget_remaining(swarm_t *s);`
- `bool swarm_child_is_subsidized(const swarm_child_t *c);`
- `double swarm_estimate_task_cost(swarm_t *s, const char *model);`
- `void swarm_enforce_budgets(swarm_t *s);`
- `int swarm_group_create(swarm_t *s, const char *name);`
- `int swarm_group_dispatch(swarm_t *s, int group_id, const char **tasks, int task_count, const char *model);`
- `bool swarm_group_complete(swarm_t *s, int group_id);`
- `bool swarm_group_reclaim(swarm_t *s, int group_id);`
- `int swarm_reclaimable_count(swarm_t *s);`
- `int swarm_reclaim_all_complete(swarm_t *s);`
- `int swarm_poll(swarm_t *s, int timeout_ms);`
- `int swarm_poll_stream(swarm_t *s, int timeout_ms, swarm_stream_cb cb, void *ctx);`
- `int swarm_wait_any(swarm_t *s, int timeout_ms);`
- `int swarm_wait_n(swarm_t *s, int n, int *out_ids, int timeout_ms);`
- `int swarm_completion_pop(swarm_t *s);`
- `int swarm_completion_pending(swarm_t *s);`
- `swarm_child_t *swarm_get(swarm_t *s, int child_id);`
- `const char *swarm_status_str(swarm_status_t st);`
- `int swarm_active_count(swarm_t *s);`
- `int swarm_group_active_count(swarm_t *s, int group_id);`
- `int swarm_group_done_count(swarm_t *s, int group_id);`
- `int swarm_group_error_count(swarm_t *s, int group_id);`
- `int swarm_group_killed_count(swarm_t *s, int group_id);`
- `double swarm_group_est_cost_usd(swarm_t *s, int group_id);`
- `double swarm_child_elapsed_sec(const swarm_child_t *c);`
- `bool swarm_kill(swarm_t *s, int child_id);`
- `void swarm_group_kill(swarm_t *s, int group_id);`
- `int swarm_status_json(swarm_t *s, char *buf, size_t len);`
- `int swarm_child_output(swarm_t *s, int child_id, char *buf, size_t len);`
- `int swarm_group_status_json(swarm_t *s, int group_id, char *buf, size_t len);`
- `int swarm_group_persist_run(swarm_t *s, int group_id, const char *run_id, const char *topology, const char *user_prompt, const char *coordinator_output, char *out_dir, size_t out_dir_len);`
- `int swarm_group_render_frame(swarm_t *s, int group_id, const char *run_id, const char *topology, char *buf, size_t len);`
- `int swarm_group_ensure_durable_run(swarm_t *s, int group_id, const char *topology, const char *suggested_run_id);`

## `swarm_daemon.h`

Function-like declarations: 1

### Declarations

- `int swarm_daemon_cli(int argc, char **argv);`

## `talons.h`

Function-like declarations: 36

### Declarations

- `void talons_init(talons_engine_t *t);`
- `int talons_goal_create(talons_engine_t *t, const char *description, double priority, grip_strength_t grip, strategy_type_t strategy, double deadline);`
- `int talons_goal_create_sub(talons_engine_t *t, int parent_id, const char *description, double priority);`
- `bool talons_goal_stalk(talons_engine_t *t, int goal_id);`
- `bool talons_goal_strike(talons_engine_t *t, int goal_id);`
- `bool talons_goal_grip(talons_engine_t *t, int goal_id);`
- `bool talons_goal_capture(talons_engine_t *t, int goal_id, const char *result, double cost);`
- `bool talons_goal_escaped(talons_engine_t *t, int goal_id, const char *reason, double cost);`
- `bool talons_goal_abandon(talons_engine_t *t, int goal_id, const char *reason);`
- `bool talons_goal_update_confidence(talons_engine_t *t, int goal_id, double new_confidence);`
- `const goal_t *talons_goal_get(const talons_engine_t *t, int goal_id);`
- `int talons_active_goals(const talons_engine_t *t, const goal_t **out, int max);`
- `bool talons_goal_depends_on(talons_engine_t *t, int goal_id, int dep_goal_id);`
- `bool talons_goal_deps_met(const talons_engine_t *t, int goal_id);`
- `int talons_resolve_blocked(talons_engine_t *t);`
- `int talons_tick(talons_engine_t *t, double now);`
- `int talons_tournament_begin(talons_engine_t *t, const char *objective, double deadline, double weight_quality, double weight_speed, double weight_cost);`
- `int talons_tournament_add(talons_engine_t *t, int tournament_id, const char *label, strategy_type_t strategy);`
- `bool talons_tournament_result(talons_engine_t *t, int tournament_id, int competitor_id, double score, double cost, const char *result);`
- `int talons_tournament_decide(talons_engine_t *t, int tournament_id);`
- `const tournament_t *talons_tournament_get(const talons_engine_t *t, int tournament_id);`
- `strategy_type_t talons_recommend_strategy(const talons_engine_t *t, double time_pressure, double resource_budget, double complexity);`
- `bool talons_should_retry(const talons_engine_t *t, int goal_id, strategy_type_t *suggested_strategy);`
- `bool talons_escalate_grip(talons_engine_t *t, int goal_id);`
- `double talons_win_rate(const talons_engine_t *t, int last_n);`
- `strategy_type_t talons_best_strategy(const talons_engine_t *t);`
- `int talons_to_json(const talons_engine_t *t, char *buf, size_t len);`
- `int talons_status_json(const talons_engine_t *t, char *buf, size_t len);`
- `int talons_goal_to_json(const goal_t *g, char *buf, size_t len);`
- `const char *talons_goal_state_name(goal_state_t s);`
- `const char *talons_strategy_name(strategy_type_t s);`
- `const char *talons_grip_name(grip_strength_t g);`
- `strategy_type_t talons_strategy_from_name(const char *name, strategy_type_t fallback);`
- `void talons_set_vfs(vfs_db_t *vfs);`
- `void talons_persist_strategy_history(void);`
- `void talons_restore_strategy_history(talons_engine_t *t);`

## `tamper.h`

Function-like declarations: 5

### Declarations

- `typedef void (*tamper_wiper_fn)(void *ctx);`
- `void tamper_init(void);`
- `void tamper_register_wiper(tamper_wiper_fn fn, void *ctx);`
- `bool tamper_check(void);`
- `void tamper_trigger(const char *reason);`

## `task_profile.h`

Function-like declarations: 5

### Declarations

- `task_profile_t *task_profile(const char *task, const char *api_key);`
- `void task_profile_free(task_profile_t *tp);`
- `int task_profile_json(const task_profile_t *tp, char *buf, size_t len);`
- `const topology_t *task_profile_best_topology(const task_profile_t *tp);`
- `int task_profile_explain(const task_profile_t *tp, char *buf, size_t len);`

## `tool_embeddings.h`

Function-like declarations: 0

### Declarations


## `toolmgmt.h`

Function-like declarations: 14

### Declarations

- `const char *toolmgmt_base_url(void);`
- `const char *toolmgmt_token(void);`
- `void toolmgmt_set_base_url(const char *url);`
- `void toolmgmt_set_token(const char *token);`
- `long toolmgmt_request(const char *method, const char *path, const char *body, char **out);`
- `char *toolmgmt_list_tools(int limit);`
- `char *toolmgmt_list_tools_paginated(int offset, int limit);`
- `char *toolmgmt_list_tools_all(int page_limit);`
- `char *toolmgmt_execute(const char *tool, const char *args_json, int timeout_ms);`
- `char *toolmgmt_batch(const char *calls_json, bool parallel);`
- `char *toolmgmt_recommend(const char *intent, const char *query, int max_steps);`
- `int toolmgmt_parallel(tm_call_t *calls, int n, int max_concurrency);`
- `int toolmgmt_register_tools(void);`
- `int toolmgmt_cli(int argc, char **argv);`

## `tools.h`

Function-like declarations: 108

### Declarations

- `bool (*execute)(const char *input_json, char *result, size_t result_len);`
- `void tools_init_profile(tools_init_profile_t profile);`
- `void tools_init(void);`
- `void tools_init_local_only(void);`
- `tools_init_profile_t tools_current_profile(void);`
- `bool tools_profile_allows_index(int index);`
- `void tools_set_vfs(struct vfs_db *vfs);`
- `void tools_set_runtime_api_key(const char *api_key);`
- `void tools_set_runtime_model(const char *model);`
- `const char *tools_runtime_api_key(void);`
- `const char *tools_runtime_model(void);`
- `void tools_set_self_exit_allowed(bool allowed);`
- `bool tools_self_exit_allowed(void);`
- `void tools_set_context_window(int tokens);`
- `int tools_context_window(void);`
- `void tools_set_context_usage(int input_tokens, int output_tokens);`
- `void tools_set_tool_schema_usage(int active_tools, int schema_tokens);`
- `void tools_set_inline_truncation(bool enabled);`
- `void tools_set_trace_context(const char *trace_id, const char *chronicle_parent_span_id, const char *chronicle_tool_span_id, const char *tool_name);`
- `void tools_clear_trace_context(void);`
- `void tools_context_turn_begin(void);`
- `swarm_t *tools_swarm_instance(void);`
- `const tool_def_t *tools_get_all(int *count);`
- `bool tools_invoke_by_name(const char *name, const char *input, char *result, size_t rlen);`
- `bool tools_is_offload_safe(const char *name);`
- `bool tools_meta_is_read_only(const char *name, bool *found);`
- `int tools_get_core_count(void);`
- `int tools_builtin_count(void);`
- `bool tools_execute(const char *name, const char *input_json, char *result, size_t result_len);`
- `bool tools_execute_raw_for_test(const char *name, const char *input_json, char *result, size_t result_len);`
- `bool tools_execute_for_tier(const char *name, const char *input_json, const char *tier, char *result, size_t result_len);`
- `void tools_governance_experiment_stats(unsigned long *gate_calls, unsigned long *bypassed, double *gate_ms_total);`
- `bool tools_is_allowed_for_tier(const char *name, const char *tier, char *reason, size_t reason_len);`
- `char *tools_normalize_input(const char *name, const char *input_json);`
- `bool dsco_run_ask_dialog(const char *spec_json, char *result, size_t result_len);`
- `bool dsco_tool_is_interactive(const char *name);`
- `void tools_loop_control_reset(void);`
- `bool tools_loop_control_has_active(void);`
- `int tools_loop_control_effective_max_turns(int default_max_turns);`
- `void tools_loop_control_decide(int current_turn, bool model_done, bool has_followup, loop_control_decision_t *out);`
- `void tools_register_vm_dispatch(vm_t *vm);`
- `void tool_map_init(tool_map_t *m);`
- `void tool_map_free(tool_map_t *m);`
- `void tool_map_insert(tool_map_t *m, const char *name, int index);`
- `int tool_map_lookup(tool_map_t *m, const char *name);`
- `int tools_lookup_index(const char *name);`
- `void tools_registry_map_free(void);`
- `typedef char *(*external_tool_cb)(const char *name, const char *input_json, void *ctx);`
- `void tools_register_external(const char *name, const char *description, const char *input_schema_json, external_tool_cb cb, void *ctx);`
- `void tools_register_external_with_output(const char *name, const char *description, const char *input_schema_json, const char *output_schema_json, external_tool_cb cb, void *ctx);`
- `void tools_register_external_metadata(const char *name, const char *integration_id, const char *display_name, const char *distribution_channel, const char *categories, const char *labels, const char *scope, unsigned action_flags, const char *catalog_status);`
- `void tools_reset_external(void);`
- `const char *tools_default_input_schema_json(void);`
- `const char *tools_default_output_schema_json(void);`
- `const char *tools_output_schema_for_def(const tool_def_t *tool);`
- `const char *tools_output_schema_for_name(const char *name);`
- `int tools_external_count(void);`
- `external_tool_snapshot_t tools_external_snapshot(void);`
- `void tools_external_snapshot_free(external_tool_snapshot_t *snapshot);`
- `int tools_rank_external_snapshot(const external_tool_snapshot_t *snapshot, const char *context, int *out_indices, int max_indices);`
- `void dsco_locks_init(dsco_locks_t *l);`
- `void dsco_locks_destroy(dsco_locks_t *l);`
- `void watchdog_start(tool_watchdog_t *wd, pthread_t target, const char *name, int timeout_s);`
- `void watchdog_stop(tool_watchdog_t *wd);`
- `int watchdog_renew(tool_watchdog_t *wd, int extra_s);`
- `int watchdog_renew_by_name(const char *name, int extra_s);`
- `int watchdog_active_snapshot(watchdog_info_t *out, int max);`
- `static inline int dsco_tool_default_timeout_s(void) { return dsco_env_int("DSCO_TOOL_DEFAULT_TIMEOUT", TOOL_DEFAULT_TIMEOUT_S, 1, TOOL_TIMEOUT_MAX_S);`
- `} static inline int dsco_tool_grace_period_s(void) { return dsco_env_int("DSCO_TOOL_GRACE_PERIOD_S", TOOL_GRACE_PERIOD_S, 0, 300);`
- `int tool_timeout_for(const char *name);`
- `bool tools_validate_input(const char *name, const char *input_json, char *error_buf, size_t error_len);`
- `int tools_retrieve(const char *context, int *out_indices, int max_tools);`
- `const tool_def_t **tools_get_filtered(const char *context, int max_tools, int *out_count);`
- `void tools_hint_init(void);`
- `void tools_hint_add(const tool_hint_t *h);`
- `void tools_hint_add_user(const char *input);`
- `void tools_hint_decay(void);`
- `void tools_hint_clear(void);`
- `int tools_hint_count(void);`
- `void tools_cooc_init(void);`
- `void tools_cooc_update(const char **tool_names, int n);`
- `void tools_cooc_persist(void);`
- `void tools_cooc_load(void);`
- `void tools_cooc_free(void);`
- `int tools_cooc_top_edges(tools_cooc_edge_t *out, int max);`
- `tool_page_result_t tools_get_paged(const char *context, int max_tools, float budget_ratio);`
- `void tool_page_result_free(tool_page_result_t *r);`
- `int tools_loaded_builtin_indices(int *out_indices, int max_indices);`
- `int tools_loaded_builtin_count(void);`
- `bool tools_is_builtin_loaded(const char *name);`
- `void tools_loaded_builtin_clear(void);`
- `void tools_cooc_inject_hints(const char **tool_names, int n);`
- `void tools_cooc_decay(float factor);`
- `void tool_quorum_gate_api(const char *context, const char *api_key);`
- `char *tools_build_compact_catalog(void);`
- `bool tool_is_progressive_schema(const tool_def_t *t, const tool_page_result_t *r);`
- `void tools_set_active_conversation(void *conv);`
- `void tools_set_active_session(void *session);`
- `void tools_playbook_advance_turn(void);`
- `typedef bool (*tool_profile_filter_fn_t)(const char *tool_name, const char *group_hint);`
- `void tools_set_profile_filter(tool_profile_filter_fn_t fn);`
- `void tools_clear_profile_filter(void);`
- `bool tools_is_parent_specified_core_tool(const char *tool_name);`
- `int safe_exec_argv(const char *const argv[], char *out, size_t out_len);`
- `float *tools_embed_text(const char *text, int *out_dim);`
- `void tools_set_agent_context(const char *recent_results, const char *working_memory_summary);`
- `int safe_exec_argv(const char *const argv[], char *out, size_t out_len);`
- `size_t tools_test_truncate_json(const char *json, size_t json_len, char *out, size_t out_len);`

## `topology.h`

Function-like declarations: 19

### Declarations

- `void topology_registry_init(void);`
- `const topology_t *topology_get(int id);`
- `const topology_t *topology_find(const char *name);`
- `const topology_t *topology_registry(int *count);`
- `const topology_t *topology_auto_select(const char *task);`
- `bool topology_is_runnable(const topology_t *t);`
- `bool topology_profile_task(const char *task, topology_task_profile_t *profile);`
- `bool topology_plan_build(const char *preferred_topology, bool auto_mode, const char *task, topology_plan_t *plan);`
- `bool topology_plan_run(const topology_plan_t *plan, const char *api_key, const char *coordinator_model, const char *task, char *result, size_t rlen, topology_run_stats_t *stats);`
- `int topology_render_ascii(const topology_t *t, char *buf, size_t buflen);`
- `double topology_estimate_cost(const topology_t *t, int input_tokens, int output_tokens);`
- `const char *topology_resolve_model_for_tier(const char *coordinator_model, const char *api_key, model_tier_t tier, char *buf, size_t buflen);`
- `bool topology_throughput_enabled(void);`
- `bool topology_resolve_throughput_lane_for_tier(const char *api_key, model_tier_t tier, int slot, char *provider_buf, size_t provider_len, char *model_buf, size_t model_len);`
- `bool topology_hetero_enabled(void);`
- `bool topology_run(const topology_t *t, const char *api_key, const char *coordinator_model, const char *task, char *result, size_t rlen, topology_run_stats_t *stats);`
- `void topology_set_scheduler(struct scheduler_t *sched);`
- `bool topology_run_scheduled(const topology_t *t, const char *api_key, const char *coordinator_model, const char *task, char *result, size_t rlen, topology_run_stats_t *stats);`
- `bool topology_plan_run_scheduled(const topology_plan_t *plan, const char *api_key, const char *coordinator_model, const char *task, char *result, size_t rlen, topology_run_stats_t *stats);`

## `touchid.h`

Function-like declarations: 4

### Declarations

- `typedef void (*touchid_cb_t)(bool success, const char *err_msg, void *ctx);`
- `bool touchid_available(void);`
- `void touchid_authenticate(const char *reason, touchid_cb_t cb, void *ctx);`
- `bool touchid_authenticate_sync(const char *reason);`

## `trace.h`

Function-like declarations: 5

### Declarations

- `void trace_init(void);`
- `void trace_shutdown(void);`
- `bool trace_enabled(trace_level_t lvl);`
- `void trace_log(trace_level_t lvl, const char *func, const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 5, 6)));`
- `void trace_log_kv(trace_level_t lvl, const char *func, const char *file, int line, const char *event, ...);`

## `trading.h`

Function-like declarations: 34

### Declarations

- `} static inline risk_limits_t risk_limits_from_env(void) { risk_limits_t r = risk_limits_default();`
- `r.dry_run = dsco_env_bool("DSCO_TRADING_DRY_RUN", r.dry_run);`
- `r.max_order_usd = dsco_env_double("DSCO_TRADING_MAX_ORDER", r.max_order_usd, 0.0, 1000000000.0);`
- `r.max_total_exposure_usd = dsco_env_double("DSCO_TRADING_MAX_EXPOSURE", r.max_total_exposure_usd, 0.0, 1000000000.0);`
- `r.min_arb_spread = dsco_env_double("DSCO_TRADING_MIN_ARB_SPREAD", r.min_arb_spread, 0.0, 1.0);`
- `r.max_open_orders = dsco_env_int("DSCO_TRADING_MAX_OPEN_ORDERS", r.max_open_orders, 1, 1000000);`
- `r.max_position_usd = dsco_env_double("DSCO_TRADING_MAX_POSITION", r.max_position_usd, 0.0, 1000000000.0);`
- `} bool tool_kalshi_balance(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_positions(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_portfolio(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_fills(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_create_order(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_batch_create_orders(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_cancel_order(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_cancel_all(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_amend_order(const char *input, char *result, size_t rlen);`
- `bool tool_kalshi_open_orders(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_balance(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_positions(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_open_orders(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_api_keys(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_derive_api_key(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_create_order(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_cancel_order(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_cancel_all(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_relayer_deploy(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_relayer_approve(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_relayer_execute(const char *input, char *result, size_t rlen);`
- `bool tool_polymarket_relayer_status(const char *input, char *result, size_t rlen);`
- `bool tool_arb_execute(const char *input, char *result, size_t rlen);`
- `bool tool_arb_monitor(const char *input, char *result, size_t rlen);`
- `bool tool_portfolio_cross(const char *input, char *result, size_t rlen);`
- `bool tool_risk_check(const char *input, char *result, size_t rlen);`
- `bool tool_risk_configure(const char *input, char *result, size_t rlen);`

## `trust.h`

Function-like declarations: 9

### Declarations

- `void dsco_trust_default_config(dsco_trust_config_t *cfg);`
- `int dsco_trust_init(const dsco_trust_config_t *cfg);`
- `void dsco_trust_shutdown(void);`
- `int dsco_trust_emit_attest(void);`
- `int dsco_trust_emit_heartbeat(const dsco_trust_runtime_t *r);`
- `int dsco_trust_emit(const char *event_type, const char *json_payload);`
- `void dsco_trust_stats(dsco_trust_stats_t *out);`
- `const char *dsco_trust_endpoint(void);`
- `bool dsco_trust_is_active(void);`

## `tui.h`

Function-like declarations: 310

### Declarations

- `const tui_box_chars_t *tui_box_chars(tui_box_style_t style);`
- `int tui_term_width(void);`
- `int tui_term_height(void);`
- `void tui_cursor_hide(void);`
- `void tui_cursor_show(void);`
- `void tui_cursor_move(int row, int col);`
- `void tui_clear_screen(void);`
- `void tui_screen_reset_full(void);`
- `void tui_clear_line(void);`
- `void tui_save_cursor(void);`
- `void tui_restore_cursor(void);`
- `void tui_terminal_restore_sane(void);`
- `bool tui_cursor_report_queries_enabled(void);`
- `void tui_term_lock(void);`
- `void tui_term_unlock(void);`
- `void tui_install_crash_handlers(void);`
- `void tui_cleanup(void);`
- `void tui_box(const char *title, const char *body, tui_box_style_t style, const char *border_color, int width);`
- `void tui_divider(tui_box_style_t style, const char *color, int width);`
- `void tui_panel(const tui_panel_t *p);`
- `void tui_spinner_init(tui_spinner_t *s, tui_spinner_type_t type, const char *label, const char *color);`
- `void tui_spinner_tick(tui_spinner_t *s);`
- `void tui_spinner_done(tui_spinner_t *s, const char *final_label);`
- `void tui_progress(const char *label, double pct, int width, const char *fill_color, const char *empty_color);`
- `void tui_table_init(tui_table_t *t, int cols, const char *header_color);`
- `void tui_table_header(tui_table_t *t, ...);`
- `void tui_table_row(tui_table_t *t, ...);`
- `void tui_table_render(const tui_table_t *t, int width);`
- `void tui_badge(const char *text, const char *fg, const char *bg);`
- `void tui_tag(const char *text, const char *color);`
- `void tui_header(const char *text, const char *color);`
- `void tui_subheader(const char *text);`
- `void tui_info(const char *text);`
- `void tui_success(const char *text);`
- `void tui_warning(const char *text);`
- `void tui_error(const char *text);`
- `void tui_welcome(const char *model, int core_count, int total_count, const char *version);`
- `void tui_stream_start(void);`
- `void tui_stream_text(const char *text);`
- `void tui_stream_tool(const char *name, const char *id);`
- `void tui_stream_tool_result(const char *name, bool ok, const char *preview);`
- `void tui_stream_end(void);`
- `tui_glyph_tier_t tui_detect_glyph_tier(void);`
- `const tui_glyphs_t *tui_glyph(void);`
- `void tui_set_glyph_tier(tui_glyph_tier_t tier);`
- `tui_color_level_t tui_detect_color_level(void);`
- `bool tui_supports_truecolor(void);`
- `tui_rgb_t tui_hsv_to_rgb(float h, float s, float v);`
- `uint64_t tui_color_name_hash(const char *name);`
- `int tui_rgb_to_256(tui_rgb_t c);`
- `tui_rgb_t tui_named_color_rgb(const char *name);`
- `void tui_named_color_sample(const char *name, tui_named_color_t *out);`
- `void tui_construct_color_sample(const char *kind, const char *name, const char *state, double weight, tui_named_color_t *out);`
- `void tui_fg_rgb(tui_rgb_t c);`
- `void tui_write_fg(FILE *out, tui_rgb_t c);`
- `void tui_write_bg(FILE *out, tui_rgb_t c);`
- `const char *tui_rgb_fg_escape(tui_rgb_t c);`
- `const char *tui_rgb_bg_escape(tui_rgb_t c);`
- `const char *tui_named_fg(const char *name);`
- `const char *tui_named_bg(const char *name);`
- `void tui_gradient_text(const char *text, float h_start, float h_end, float s, float v);`
- `void tui_gradient_divider(int width, float h_start, float h_end);`
- `void tui_transition_divider(void);`
- `tui_tool_type_t tui_classify_tool(const char *name);`
- `const char *tui_tool_color(tui_tool_type_t type);`
- `tui_rgb_t tui_tool_rgb(tui_tool_type_t type);`
- `void tui_async_spinner_start(tui_async_spinner_t *s, const char *label, tui_tool_type_t tool_type);`
- `bool tui_tool_is_display_art(const char *name);`
- `bool tui_print_tool_art(const char *name, const char *result);`
- `void tui_async_spinner_stop(tui_async_spinner_t *s, bool ok, const char *result_preview, double elapsed_ms, const char *suffix);`
- `void tui_batch_spinner_start(tui_batch_spinner_t *bs, const char **names, int count);`
- `void tui_batch_spinner_complete(tui_batch_spinner_t *bs, int idx, bool ok, const char *preview, double elapsed_ms);`
- `void tui_batch_spinner_stop(tui_batch_spinner_t *bs);`
- `void tui_batch_summary(const tui_batch_spinner_t *bs, const char *cost_suffix);`
- `void tui_status_bar_init(tui_status_bar_t *sb, const char *model);`
- `void tui_status_bar_set_model(tui_status_bar_t *sb, const char *model, const char *slot_name);`
- `void tui_status_bar_update(tui_status_bar_t *sb, int in_tok, int out_tok, double cost, int turn, int tools);`
- `void tui_status_bar_set_budget(tui_status_bar_t *sb, double budget_limit, double burn_rate, double percent, double runway);`
- `void tui_status_bar_enable(tui_status_bar_t *sb);`
- `void tui_status_bar_disable(tui_status_bar_t *sb);`
- `void tui_status_bar_render(tui_status_bar_t *sb);`
- `bool tui_motion_enabled(void);`
- `const char *tui_motion_activity_frame(int frame, bool unicode);`
- `void tui_panel_notify(tui_status_bar_t *sb, tui_panel_note_level_t level, const char *text);`
- `void tui_panel_notify_clear(tui_status_bar_t *sb);`
- `void tui_panel_set_active(tui_status_bar_t *sb, bool active);`
- `void tui_input_panel_render(tui_status_bar_t *sb, const char *prompt_hint);`
- `void tui_input_panel_clear(tui_status_bar_t *sb);`
- `void tui_bottom_panel_refresh(tui_status_bar_t *sb, const char *prompt_hint);`
- `bool tui_prepare_external_output(void);`
- `bool tui_prepare_external_output_locked(void);`
- `void tui_transcript_ensure_region(void);`
- `void tui_transcript_stream_set_live(bool live);`
- `bool tui_transcript_stream_is_live(void);`
- `void tui_pad_to_panel_anchor(void);`
- `char *tui_composer_read(tui_status_bar_t *sb, const char *prompt, char *out, size_t out_sz);`
- `typedef bool (*tui_composer_escape_hook_t)(void *ctx);`
- `void tui_composer_set_escape_hook(tui_composer_escape_hook_t hook, void *ctx);`
- `int tui_composer_signal_interrupt(void);`
- `bool tui_composer_is_reading(void);`
- `bool tui_composer_is_mounted(void);`
- `int tui_composer_transcript_row(void);`
- `void tui_composer_preserve_on_interrupt(bool preserve);`
- `void tui_swarm_panel(tui_swarm_entry_t *entries, int count, int width);`
- `void tui_retry_pulse(const char *label, int attempt, int max, double wait_sec);`
- `int tui_subpixel_hbar(FILE *out, double frac, int cells, const char *fill_color, const char *empty_glyph, const char *empty_color);`
- `void tui_braille_init(tui_braille_t *b, int px_w, int px_h);`
- `void tui_braille_free(tui_braille_t *b);`
- `void tui_braille_clear(tui_braille_t *b);`
- `void tui_braille_set(tui_braille_t *b, int x, int y);`
- `void tui_braille_line(tui_braille_t *b, int x0, int y0, int x1, int y1);`
- `void tui_braille_plot(tui_braille_t *b, const double *values, int n);`
- `void tui_braille_render(const tui_braille_t *b, FILE *out, const char *color);`
- `void tui_sparkline(const double *values, int count, const char *color);`
- `bool tui_try_sparkline(const char *text);`
- `void tui_sparkline_braille(const double *values, int count, int rows, const char *color);`
- `void tui_cached_badge(const char *tool_name);`
- `void tui_context_gauge(int used, int max_tok, int width);`
- `void tui_compact_flash(int before, int after);`
- `int tui_estimate_tokens(const char *text);`
- `void tui_prompt_token_display(int est, int remaining);`
- `void tui_ipc_message_line(const char *from, const char *to, const char *topic, const char *preview);`
- `void tui_agent_rollup(int total, int done, int running, int errored);`
- `tui_theme_t tui_detect_theme(void);`
- `void tui_apply_theme(tui_theme_t theme);`
- `const char *tui_theme_dim(void);`
- `const char *tui_theme_bright(void);`
- `const char *tui_theme_accent(void);`
- `void tui_section_divider(int turn, int tools, double cost, const char *model, double tok_per_sec);`
- `void tui_section_divider_ex(int turn, int tools_ok, int tools_fail, int cache_hits, double cost, const char *model, double tok_per_sec, double ctx_pct, const char *git_branch);`
- `void tui_error_typed(tui_err_type_t type, const char *msg);`
- `void tui_notify(const char *title, const char *body);`
- `typedef void (*tui_cadence_flush_cb)(const char *buf, int len, void *ctx);`
- `void tui_cadence_init(tui_cadence_t *c, tui_cadence_flush_cb cb, void *ctx);`
- `void tui_cadence_feed(tui_cadence_t *c, const char *text);`
- `void tui_cadence_flush(tui_cadence_t *c);`
- `void tui_cadence_drain(tui_cadence_t *c);`
- `void tui_thinking_init(tui_thinking_state_t *t);`
- `void tui_thinking_feed(tui_thinking_state_t *t, const char *text);`
- `void tui_thinking_end(tui_thinking_state_t *t);`
- `void tui_word_counter_init(tui_word_counter_t *w);`
- `void tui_word_counter_feed(tui_word_counter_t *w, const char *text);`
- `void tui_word_counter_render(tui_word_counter_t *w);`
- `void tui_word_counter_end(tui_word_counter_t *w);`
- `void tui_throughput_init(tui_throughput_t *t);`
- `void tui_throughput_tick(tui_throughput_t *t, int tokens);`
- `void tui_throughput_render(tui_throughput_t *t);`
- `void tui_flame_init(tui_flame_t *f);`
- `void tui_flame_add(tui_flame_t *f, const char *name, double start_ms, double end_ms, bool ok, tui_tool_type_t type);`
- `void tui_flame_render(tui_flame_t *f);`
- `void tui_dag_init(tui_dag_t *d);`
- `int tui_dag_add_node(tui_dag_t *d, const char *name);`
- `void tui_dag_add_edge(tui_dag_t *d, int from, int to);`
- `void tui_dag_render(tui_dag_t *d);`
- `void tui_tool_cost(const char *name, int in_tok, int out_tok, const char *model);`
- `void tui_chart(tui_chart_type_t type, const char **labels, const double *values, int count, int width, int height);`
- `void tui_citation_init(tui_citation_t *c);`
- `int tui_citation_add(tui_citation_t *c, const char *tool_name, const char *tool_id, const char *preview, double elapsed_ms);`
- `void tui_citation_render(tui_citation_t *c);`
- `void tui_render_diff(const char *text, FILE *out);`
- `bool tui_is_diff(const char *text);`
- `void tui_table_render_sorted(const tui_table_t *t, int width, int sort_col, bool ascending);`
- `void tui_json_tree(const char *json, int max_depth, bool color);`
- `void tui_minimap_render(const tui_minimap_entry_t *entries, int count, int height);`
- `void tui_branch_init(tui_branch_t *b);`
- `void tui_branch_push(tui_branch_t *b, const char *prompt);`
- `bool tui_branch_detect(tui_branch_t *b, const char *prompt);`
- `void tui_ghost_init(tui_ghost_t *g);`
- `void tui_ghost_push(tui_ghost_t *g, const char *cmd);`
- `const char *tui_ghost_match(tui_ghost_t *g, const char *prefix);`
- `bool tui_is_code_paste(const char *text);`
- `void tui_highlight_input(const char *text, FILE *out);`
- `void tui_image_preview_badge(const char *path, const char *media_type, long size, int w, int h);`
- `void tui_command_palette(const tui_cmd_entry_t *cmds, int count, const char *filter);`
- `void tui_composer_set_slash_commands(const tui_cmd_entry_t *cmds, int count);`
- `void tui_agent_topology(const tui_agent_node_t *agents, int count);`
- `void tui_swarm_cost(const tui_swarm_cost_entry_t *agents, int count, double total);`
- `void tui_scroller_init(tui_scroller_t *s, const char **lines, int count);`
- `void tui_scroller_render(tui_scroller_t *s);`
- `bool tui_scroller_handle_key(tui_scroller_t *s, int ch);`
- `void tui_latency_waterfall(const tui_latency_breakdown_t *b);`
- `void tui_session_diff(int msg_count, int tool_calls, int est_tokens, const char *model);`
- `void tui_heatmap_word(const char *word, int len, FILE *out);`
- `void tui_features_list(const tui_features_t *f);`
- `bool tui_features_toggle(tui_features_t *f, const char *name);`
- `void tui_status_bar_set_clock(tui_status_bar_t *sb, bool show);`
- `void tui_notif_queue_init(tui_notif_queue_t *q);`
- `int tui_notif_push(tui_notif_queue_t *q, tui_notif_level_t level, const char *tag, const char *fmt, ...);`
- `void tui_notif_dismiss(tui_notif_queue_t *q, int id);`
- `void tui_notif_dismiss_tag(tui_notif_queue_t *q, const char *tag);`
- `void tui_notif_gc(tui_notif_queue_t *q);`
- `void tui_notif_render(tui_notif_queue_t *q);`
- `int tui_notif_unread(tui_notif_queue_t *q);`
- `void tui_notif_clear_all(tui_notif_queue_t *q);`
- `void tui_toast_init(tui_toast_t *t);`
- `void tui_toast_show(tui_toast_t *t, tui_notif_level_t level, double duration_sec, const char *fmt, ...);`
- `void tui_toast_tick(tui_toast_t *t);`
- `void tui_toast_destroy(tui_toast_t *t);`
- `typedef void (*tui_fsm_action_fn)(void *ctx);`
- `typedef bool (*tui_fsm_guard_fn)(void *ctx);`
- `void tui_fsm_init(tui_fsm_t *fsm, const char *name, void *ctx);`
- `int tui_fsm_add_state(tui_fsm_t *fsm, const char *name, tui_fsm_action_fn on_enter, tui_fsm_action_fn on_exit, tui_fsm_action_fn on_tick);`
- `void tui_fsm_add_transition(tui_fsm_t *fsm, int from, int to, int event, tui_fsm_guard_fn guard, tui_fsm_action_fn action);`
- `bool tui_fsm_send(tui_fsm_t *fsm, int event);`
- `void tui_fsm_tick(tui_fsm_t *fsm);`
- `const char *tui_fsm_current_name(const tui_fsm_t *fsm);`
- `double tui_fsm_time_in_state(const tui_fsm_t *fsm);`
- `void tui_fsm_debug(const tui_fsm_t *fsm);`
- `void tui_render_ctx_init(tui_render_ctx_t *rc);`
- `int tui_render_slot_alloc(tui_render_ctx_t *rc, tui_slot_type_t type, int z_order);`
- `void tui_render_slot_update(tui_render_ctx_t *rc, int slot_id, const char *content);`
- `void tui_render_slot_free(tui_render_ctx_t *rc, int slot_id);`
- `void tui_render_slot_dirty(tui_render_ctx_t *rc, int slot_id);`
- `void tui_render_flush(tui_render_ctx_t *rc);`
- `void tui_render_ctx_destroy(tui_render_ctx_t *rc);`
- `void tui_multi_progress_init(tui_multi_progress_t *mp, const char *title);`
- `int tui_multi_progress_add_phase(tui_multi_progress_t *mp, const char *name, double weight);`
- `void tui_multi_progress_start_phase(tui_multi_progress_t *mp, int phase_idx);`
- `void tui_multi_progress_update(tui_multi_progress_t *mp, double progress);`
- `void tui_multi_progress_complete_phase(tui_multi_progress_t *mp);`
- `void tui_multi_progress_render(tui_multi_progress_t *mp);`
- `double tui_multi_progress_total(tui_multi_progress_t *mp);`
- `double tui_multi_progress_eta_sec(tui_multi_progress_t *mp);`
- `void tui_multi_progress_destroy(tui_multi_progress_t *mp);`
- `typedef void (*tui_event_handler_fn)(const tui_event_t *event, void *ctx);`
- `void tui_event_bus_init(tui_event_bus_t *bus);`
- `int tui_event_subscribe(tui_event_bus_t *bus, tui_event_type_t type, tui_event_handler_fn handler, void *ctx);`
- `void tui_event_unsubscribe(tui_event_bus_t *bus, int sub_id);`
- `void tui_event_emit(tui_event_bus_t *bus, const tui_event_t *event);`
- `void tui_event_emit_simple(tui_event_bus_t *bus, tui_event_type_t type, const char *source);`
- `void tui_event_bus_dump(const tui_event_bus_t *bus, int max_events);`
- `void tui_event_bus_destroy(tui_event_bus_t *bus);`
- `void tui_stream_state_init(tui_stream_state_t *ss);`
- `void tui_stream_state_transition(tui_stream_state_t *ss, tui_stream_phase_t new_phase);`
- `void tui_stream_state_token(tui_stream_state_t *ss, int count);`
- `void tui_stream_state_render_badge(const tui_stream_state_t *ss);`
- `const char *tui_stream_phase_name(tui_stream_phase_t phase);`
- `tui_stream_phase_t tui_stream_state_phase(const tui_stream_state_t *ss);`
- `void tui_stream_heartbeat_start(tui_stream_heartbeat_t *hb);`
- `void tui_stream_heartbeat_poke(tui_stream_heartbeat_t *hb, const char *phase);`
- `void tui_stream_heartbeat_recv(tui_stream_heartbeat_t *hb, size_t bytes);`
- `void tui_stream_heartbeat_stop(tui_stream_heartbeat_t *hb);`
- `void tui_outq_init(tui_output_queue_t *q, FILE *out);`
- `void tui_outq_destroy(tui_output_queue_t *q);`
- `void tui_outq_write(tui_output_queue_t *q, const char *text);`
- `void tui_outq_writef(tui_output_queue_t *q, const char *fmt, ...) __attribute__((format(printf, 2, 3)));`
- `void tui_outq_write_pri(tui_output_queue_t *q, int priority, const char *text);`
- `void tui_outq_clear_line(tui_output_queue_t *q);`
- `void tui_outq_flush_sync(tui_output_queue_t *q);`
- `void tui_outq_stats(const tui_output_queue_t *q, int *total_writes, int *total_flushes, int *dropped, double *avg_flush_ms);`
- `void tui_streaming_fsm_create(tui_fsm_t *fsm, void *ctx);`
- `typedef void (*tui_anim_cb)(double elapsed_ms, void *ctx);`
- `void tui_anim_clock_init(tui_anim_clock_t *clk, int interval_ms);`
- `void tui_anim_clock_destroy(tui_anim_clock_t *clk);`
- `int tui_anim_subscribe(tui_anim_clock_t *clk, tui_anim_cb callback, void *ctx, bool keep_alive);`
- `void tui_anim_unsubscribe(tui_anim_clock_t *clk, int sub_id);`
- `void tui_anim_set_active(tui_anim_clock_t *clk, int sub_id, bool active);`
- `void tui_screenbuf_init(tui_screenbuf_t *sb, int width, int height, FILE *out);`
- `void tui_screenbuf_free(tui_screenbuf_t *sb);`
- `void tui_screenbuf_resize(tui_screenbuf_t *sb, int width, int height);`
- `void tui_screenbuf_clear(tui_screenbuf_t *sb);`
- `void tui_screenbuf_put(tui_screenbuf_t *sb, int x, int y, const char *ch, int style_id, int char_width);`
- `void tui_screenbuf_write(tui_screenbuf_t *sb, int x, int y, const char *text, int style_id);`
- `void tui_screenbuf_flush(tui_screenbuf_t *sb);`
- `int tui_style_intern(const char *ansi_seq);`
- `const char *tui_style_get(int style_id);`
- `bool tui_supports_hyperlinks(void);`
- `void tui_hyperlink(FILE *out, const char *url, const char *display_text);`
- `void tui_file_line_link(FILE *out, const char *path, int line, const char *display);`
- `int tui_char_width(unsigned int codepoint);`
- `int tui_str_display_width(const char *s);`
- `int tui_utf8_decode(const char *s, unsigned int *codepoint);`
- `int tui_utf8_truncate(const char *src, char *dst, size_t dst_len, int max_width);`
- `void tui_set_title(const char *title);`
- `void tui_set_title_fmt(const char *fmt, ...);`
- `void tui_reset_title(void);`
- `void tui_shimmer_start(tui_shimmer_t *sh, const char *label, tui_rgb_t color_a, tui_rgb_t color_b);`
- `void tui_shimmer_stop(tui_shimmer_t *sh);`
- `void tui_shimmer_text(FILE *out, const char *text, tui_rgb_t color_a, tui_rgb_t color_b);`
- `tui_perm_result_t tui_permission_prompt(const char *tool_name, const char *description, const char *detail);`
- `bool tui_confirm(const char *question);`
- `tui_ask_status_t tui_ask_questions(tui_ask_question_t *qs, int n_questions, const char *intro, char *chat_out, size_t chat_len);`
- `const char *tui_ask_answer_value(const tui_ask_question_t *q, char *out, size_t out_len);`
- `bool tui_ask_question_visible(const tui_ask_question_t *qs, int n, int qi);`
- `void tui_diff_init(tui_diff_t *d);`
- `void tui_diff_free(tui_diff_t *d);`
- `bool tui_diff_parse(tui_diff_t *d, const char *unified_diff);`
- `void tui_diff_render(const tui_diff_t *d, FILE *out, int context_lines);`
- `void tui_diff_render_inline(const tui_diff_t *d, FILE *out);`
- `void tui_pager_init(tui_pager_t *p, const char **lines, int count, const char *title);`
- `void tui_pager_run(tui_pager_t *p);`
- `void tui_code_block(FILE *out, const char *code, const char *language, int start_line, bool show_line_numbers);`
- `void tui_breadcrumb(FILE *out, const char *path, int max_width);`
- `void tui_menu_init(tui_menu_t *m, const char *title, const char *subtitle);`
- `void tui_menu_free(tui_menu_t *m);`
- `tui_menu_item_t *tui_menu_add_group(tui_menu_t *m, const char *label);`
- `tui_menu_item_t *tui_menu_add_item(tui_menu_t *m, const char *label, int id);`
- `tui_menu_item_t *tui_menu_add_separator(tui_menu_t *m);`
- `tui_menu_item_t *tui_menu_add_child(tui_menu_item_t *parent, const char *label, int id);`
- `tui_menu_item_t *tui_menu_add_submenu(tui_menu_item_t *parent, const char *label, int id);`
- `tui_menu_item_t *tui_menu_add_action(tui_menu_item_t *parent, const char *label, int id);`
- `void tui_menu_set_badge(tui_menu_item_t *it, tui_menu_badge_t badge, const char *text);`
- `void tui_menu_set_detail(tui_menu_item_t *it, const char *detail);`
- `void tui_menu_set_hint(tui_menu_item_t *it, const char *hint);`
- `void tui_menu_set_disabled(tui_menu_item_t *it, bool disabled);`
- `void tui_menu_set_expanded(tui_menu_item_t *it, bool expanded);`
- `int tui_menu_run(tui_menu_t *m, tui_menu_item_t **out_item);`
- `int tui_test_decode_key_sequence(const char *seq);`
- `int tui_test_dialog_move_row(int row, int maxrow, int key);`
- `int tui_test_menu_move_selection(const bool *selectable, int nrows, int selected, int delta);`

## `ui_motion.h`

Function-like declarations: 7

### Declarations

- `void ui_motion_init(ui_motion_t *m, bool reduced_motion);`
- `void ui_motion_set(ui_motion_t *m, uint64_t key, uint16_t prop, double target, double duration_s, ui_motion_curve_t curve, double now);`
- `void ui_motion_snap(ui_motion_t *m, uint64_t key, uint16_t prop, double value);`
- `double ui_motion_value(const ui_motion_t *m, uint64_t key, uint16_t prop, double now, double fallback);`
- `bool ui_motion_active(const ui_motion_t *m, double now);`
- `void ui_motion_prune(ui_motion_t *m, double now, double linger_s);`
- `void ui_motion_clear(ui_motion_t *m, uint64_t key, uint16_t prop);`

## `vecstore.h`

Function-like declarations: 9

### Declarations

- `vecstore_t *vecstore_open(vfs_db_t *vfs, const char *collection);`
- `void vecstore_close(vecstore_t *vs);`
- `bool vecstore_insert(vecstore_t *vs, const char *id, const float *vec, int dim, const char *metadata);`
- `bool vecstore_delete(vecstore_t *vs, const char *id);`
- `float *vecstore_get(vecstore_t *vs, const char *id, int *out_dim);`
- `int vecstore_query(vecstore_t *vs, const float *query_vec, int dim, vecstore_result_t *out, int max_results);`
- `void vecstore_result_free(vecstore_result_t *results, int count);`
- `int vecstore_count(vecstore_t *vs);`
- `float cosine_similarity_f(const float *a, const float *b, int dim);`

## `vecstore_metal.h`

Function-like declarations: 3

### Declarations

- `bool vm_metal_available(void);`
- `bool vm_query_cosine(const float *query, unsigned int dim, const float *corpus, unsigned int n_docs, float *out_scores);`
- `void vm_topk(const float *scores, unsigned int n, unsigned int k, unsigned int *out_indices, float *out_vals);`

## `vfs.h`

Function-like declarations: 30

### Declarations

- `vfs_db_t *vfs_open(const char *path);`
- `void vfs_close(vfs_db_t *db);`
- `bool vfs_kv_put(vfs_db_t *db, const char *bucket, const char *key, const void *val, size_t val_len);`
- `bool vfs_kv_put_str(vfs_db_t *db, const char *bucket, const char *key, const char *val);`
- `void *vfs_kv_get(vfs_db_t *db, const char *bucket, const char *key, size_t *out_len);`
- `char *vfs_kv_get_str(vfs_db_t *db, const char *bucket, const char *key);`
- `bool vfs_kv_delete(vfs_db_t *db, const char *bucket, const char *key);`
- `char **vfs_kv_keys(vfs_db_t *db, const char *bucket, int *out_count);`
- `bool vfs_conv_append(vfs_db_t *db, const char *session_id, const char *role, const char *content_json);`
- `vfs_conv_turn_t *vfs_conv_load(vfs_db_t *db, const char *session_id, int *out_count);`
- `void vfs_conv_free(vfs_conv_turn_t *turns, int count);`
- `bool vfs_conv_delete(vfs_db_t *db, const char *session_id);`
- `char **vfs_conv_sessions(vfs_db_t *db, int *out_count);`
- `bool vfs_log_event(vfs_db_t *db, const char *category, const char *action, const char *detail);`
- `vfs_event_t *vfs_log_query(vfs_db_t *db, const char *category, int limit, int *out_count);`
- `void vfs_log_free(vfs_event_t *events, int count);`
- `bool vfs_cache_put(vfs_db_t *db, const char *tool_name, const char *input_hash, const char *result, int ttl_seconds);`
- `char *vfs_cache_get(vfs_db_t *db, const char *tool_name, const char *input_hash);`
- `int vfs_cache_evict(vfs_db_t *db);`
- `int vfs_schema_version(vfs_db_t *db);`
- `vfs_stats_t vfs_get_stats(vfs_db_t *db);`
- `bool vfs_result_put(vfs_db_t *db, const char *tool_name, const char *input_hash, const char *result, int ttl_seconds);`
- `char *vfs_result_get(vfs_db_t *db, const char *key);`
- `int vfs_result_evict(vfs_db_t *db);`
- `char **vfs_result_list(vfs_db_t *db, int *out_count);`
- `bool vfs_pin_put(vfs_db_t *db, const char *key, const char *namespace_, const char *reason, const char *owner, int ttl_seconds);`
- `bool vfs_pin_delete(vfs_db_t *db, const char *key);`
- `bool vfs_pin_is_active(vfs_db_t *db, const char *key);`
- `int vfs_pin_evict_expired(vfs_db_t *db);`
- `int vfs_maintain(vfs_db_t *db, int64_t max_db_bytes);`

## `vm.h`

Function-like declarations: 15

### Declarations

- `typedef bool (*vm_tool_fn)(const char *input_json, char *result, size_t result_len);`
- `void vm_init(vm_t *vm);`
- `void vm_reset(vm_t *vm);`
- `int vm_emit(vm_t *vm, vm_opcode_t op, int32_t operand);`
- `int vm_add_string(vm_t *vm, const char *s);`
- `void vm_register_tool(vm_t *vm, const char *name, vm_tool_fn func, int tool_index);`
- `void vm_build_dispatch_index(vm_t *vm);`
- `bool vm_dispatch_tool(vm_t *vm, const char *tool_name, const char *input_json, char *result, size_t result_len);`
- `int vm_run(vm_t *vm);`
- `int vm_resume(vm_t *vm);`
- `void vm_push_int(vm_t *vm, int64_t val);`
- `void vm_push_str(vm_t *vm, const char *s);`
- `vm_val_t vm_pop(vm_t *vm);`
- `vm_val_t vm_peek(vm_t *vm);`
- `vm_stats_t vm_get_stats(vm_t *vm);`

## `waiter.h`

Function-like declarations: 7

### Declarations

- `int dsco_waiter_init(dsco_waiter_t *w);`
- `void dsco_waiter_destroy(dsco_waiter_t *w);`
- `bool dsco_waiter_wait_ms(dsco_waiter_t *w, long timeout_ms);`
- `void dsco_waiter_signal(dsco_waiter_t *w);`
- `void dsco_waiter_stop(dsco_waiter_t *w);`
- `void dsco_waiter_reset(dsco_waiter_t *w);`
- `bool dsco_waiter_stopped(dsco_waiter_t *w);`

## `wasm_core.h`

Function-like declarations: 9

### Declarations

- `const char *dsco_wasm_version(void);`
- `const char *dsco_wasm_exports_json(void);`
- `const char *dsco_wasm_models_json(void);`
- `const char *dsco_wasm_tools_json(void);`
- `const char *dsco_wasm_route_explain(const char *model);`
- `const char *dsco_wasm_tool_exec(const char *name, const char *input_json);`
- `const char *dsco_wasm_session_reset(void);`
- `const char *dsco_wasm_session_add(const char *role, const char *content);`
- `const char *dsco_wasm_session_state(void);`

## `watchdog.h`

Function-like declarations: 4

### Declarations

- `int watchdog_install(const char *label, const char **args, int argc);`
- `int watchdog_uninstall(const char *label);`
- `int watchdog_status(const char *label, char *buf, size_t buf_len);`
- `void watchdog_ping(void);`

## `weather_batch.h`

Function-like declarations: 1

### Declarations

- `bool tool_weather_batch(const char *input, char *result, size_t rlen);`

## `webhook_security.h`

Function-like declarations: 4

### Declarations

- `bool webhook_verify_hmac_sha256_hex(const uint8_t *secret, size_t secret_len, const uint8_t *body, size_t body_len, const char *signature_header);`
- `bool webhook_hmac_sha256_header(const uint8_t *secret, size_t secret_len, const uint8_t *body, size_t body_len, char *out, size_t out_len);`
- `webhook_ssrf_decision_t webhook_ssrf_guard_url(const char *url, char *reason, size_t reason_len);`
- `bool webhook_egress_url_allowed(const char *url, char *reason, size_t reason_len);`

## `workspace.h`

Function-like declarations: 15

### Declarations

- `const char *dsco_workspace_root(void);`
- `int dsco_workspace_bootstrap(char *summary, size_t summary_len);`
- `int dsco_workspace_status(dsco_workspace_status_t *status, char *summary, size_t summary_len);`
- `int dsco_workspace_list_skills(char *out, size_t out_len);`
- `int dsco_workspace_show_skill(const char *name, char *out, size_t out_len);`
- `bool dsco_workspace_skill_exists(const char *name);`
- `int dsco_workspace_create_skill(const char *name, const char *body, bool overwrite);`
- `int dsco_workspace_delete_skill(const char *name, bool force);`
- `int dsco_workspace_count_auto_skills(void);`
- `int dsco_workspace_list_auto_skills(char (*names)[128], int max);`
- `int dsco_workspace_read_doc(const char *name, char *out, size_t out_len);`
- `void dsco_workspace_doc_path(const char *name, char *out, size_t out_len);`
- `const char *dsco_workspace_prompt(void);`
- `const char *dsco_workspace_skill_prompt(const char *name);`
- `void dsco_workspace_prompt_invalidate(void);`
