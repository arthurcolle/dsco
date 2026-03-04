# API Reference

This file is auto-generated from header declarations in the repository root.

- Generator: `./scripts/gen_api_reference.sh`
- Headers scanned: 22

## Regeneration

```bash
./scripts/gen_api_reference.sh
```

## `agent.h`

Function-like declarations: 1

### Declarations

- `void agent_run(const char *api_key, const char *model);`

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

## `baseline.h`

Function-like declarations: 11

### Declarations

- `bool baseline_start(const char *model, const char *mode);`
- `void baseline_stop(void);`
- `bool baseline_log(const char *category, const char *title, const char *detail, const char *metadata_json);`
- `const char *baseline_instance_id(void);`
- `const char *baseline_db_path(void);`
- `int baseline_serve_http(int port, const char *default_instance_filter);`
- `void trace_new_id(char *out, size_t out_len);`
- `bool trace_span_begin(const char *trace_id, const char *name, const char *parent_span, char *span_id_out);`
- `bool trace_span_end(const char *span_id, const char *status, const char *metadata_json);`
- `void trace_query_recent(int limit);`
- `void trace_print_waterfall(const char *trace_id);`

## `config.h`

Function-like declarations: 3

### Declarations

- `} static inline const char *model_resolve_alias(const char *name) { const model_info_t *m = model_lookup(name);`
- `} static inline int model_context_window(const char *name) { const model_info_t *m = model_lookup(name);`
- `static inline void tui_features_init(tui_features_t *f) { memset(f, 1, sizeof(*f));`

## `coroutine.h`

Function-like declarations: 2

### Declarations

- `typedef int (*coro_func_t)(void *ctx);`
- `int ret = slot->func(slot->ctx);`

## `crypto.h`

Function-like declarations: 22

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

## `integrations.h`

Function-like declarations: 142

### Declarations

- `bool tool_tavily_search(const char *input, char *result, size_t rlen);`
- `bool tool_brave_search(const char *input, char *result, size_t rlen);`
- `bool tool_serpapi(const char *input, char *result, size_t rlen);`
- `bool tool_jina_read(const char *input, char *result, size_t rlen);`
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

## `ipc.h`

Function-like declarations: 27

### Declarations

- `bool ipc_init(const char *db_path, const char *agent_id);`
- `void ipc_shutdown(void);`
- `const char *ipc_self_id(void);`
- `const char *ipc_db_path(void);`
- `bool ipc_register(const char *parent_id, int depth, const char *role, const char *toolkit);`
- `bool ipc_set_status(ipc_agent_status_t status, const char *current_task);`
- `bool ipc_heartbeat(void);`
- `int ipc_list_agents(ipc_agent_info_t *out, int max);`
- `bool ipc_get_agent(const char *agent_id, ipc_agent_info_t *out);`
- `bool ipc_agent_alive(const char *agent_id);`
- `bool ipc_send(const char *to_agent, const char *topic, const char *body);`
- `int ipc_recv(ipc_message_t *out, int max);`
- `int ipc_recv_topic(const char *topic, ipc_message_t *out, int max);`
- `int ipc_unread_count(void);`
- `int ipc_task_submit(const char *description, int priority, int parent_task_id);`
- `bool ipc_task_claim(ipc_task_t *out);`
- `bool ipc_task_start(int task_id);`
- `bool ipc_task_complete(int task_id, const char *result);`
- `bool ipc_task_fail(int task_id, const char *error);`
- `int ipc_task_list(const char *assigned_to, ipc_task_t *out, int max);`
- `int ipc_task_pending_count(void);`
- `bool ipc_scratch_put(const char *key, const char *value);`
- `char *ipc_scratch_get(const char *key);`
- `bool ipc_scratch_del(const char *key);`
- `int ipc_scratch_keys(const char *prefix, char keys[][IPC_MAX_KEY], int max);`
- `int ipc_poll(void);`
- `int ipc_status_json(char *buf, size_t len);`

## `json_util.h`

Function-like declarations: 26

### Declarations

- `void arena_init(arena_t *a);`
- `void *arena_alloc(arena_t *a, size_t size);`
- `char *arena_strdup(arena_t *a, const char *s);`
- `void arena_reset(arena_t *a);`
- `void arena_free(arena_t *a);`
- `json_validation_t json_validate_schema(const char *json, const char *schema_json);`
- `void *safe_malloc(size_t size);`
- `void *safe_realloc(void *ptr, size_t size);`
- `char *safe_strdup(const char *s);`
- `void jbuf_init(jbuf_t *b, size_t initial_cap);`
- `void jbuf_free(jbuf_t *b);`
- `void jbuf_reset(jbuf_t *b);`
- `void jbuf_append(jbuf_t *b, const char *s);`
- `void jbuf_append_len(jbuf_t *b, const char *s, size_t n);`
- `void jbuf_append_char(jbuf_t *b, char c);`
- `void jbuf_append_json_str(jbuf_t *b, const char *s);`
- `void jbuf_append_int(jbuf_t *b, int v);`
- `bool json_parse_response(const char *json, parsed_response_t *out);`
- `bool json_parse_response_arena(const char *json, parsed_response_t *out, arena_t *arena);`
- `void json_free_response(parsed_response_t *r);`
- `char *json_get_str(const char *json, const char *key);`
- `char *json_get_raw(const char *json, const char *key);`
- `int json_get_int(const char *json, const char *key, int def);`
- `bool json_get_bool(const char *json, const char *key, bool def);`
- `typedef void (*json_array_cb)(const char *element_start, void *ctx);`
- `int json_array_foreach(const char *json, const char *key, json_array_cb cb, void *ctx);`

## `llm.h`

Function-like declarations: 35

### Declarations

- `typedef void (*stream_text_cb)(const char *text, void *ctx);`
- `typedef void (*stream_tool_start_cb)(const char *name, const char *id, void *ctx);`
- `typedef void (*stream_thinking_cb)(const char *text, void *ctx);`
- `void session_state_init(session_state_t *s, const char *model);`
- `void conv_init(conversation_t *c);`
- `void conv_free(conversation_t *c);`
- `void conv_add_user_text(conversation_t *c, const char *text);`
- `void conv_add_assistant_text(conversation_t *c, const char *text);`
- `void conv_add_assistant_tool_use(conversation_t *c, const char *tool_id, const char *tool_name, const char *tool_input);`
- `void conv_add_tool_result(conversation_t *c, const char *tool_id, const char *result, bool is_error);`
- `void conv_add_assistant_raw(conversation_t *c, parsed_response_t *resp);`
- `void conv_add_user_image_base64(conversation_t *c, const char *media_type, const char *base64_data, const char *text);`
- `void conv_add_user_image_url(conversation_t *c, const char *url, const char *text);`
- `void conv_add_user_document(conversation_t *c, const char *media_type, const char *base64_data, const char *title, const char *text);`
- `void conv_pop_last(conversation_t *c);`
- `void conv_trim_old_results(conversation_t *c, int keep_recent, int max_chars);`
- `bool conv_save(conversation_t *c, const char *path);`
- `bool conv_load(conversation_t *c, const char *path);`
- `char *llm_build_request(conversation_t *c, const char *model, int max_tokens);`
- `char *llm_build_request_ex(conversation_t *c, session_state_t *session, int max_tokens);`
- `int llm_count_tokens(const char *api_key, const char *request_json);`
- `const char *llm_get_custom_system_prompt(void);`
- `void llm_debug_save_request(const char *request_json, int http_status);`
- `stream_result_t llm_stream(const char *api_key, const char *request_json, stream_text_cb text_cb, stream_tool_start_cb tool_cb, stream_thinking_cb thinking_cb, void *cb_ctx);`
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

## `mcp.h`

Function-like declarations: 4

### Declarations

- `int mcp_init(mcp_registry_t *reg);`
- `void mcp_shutdown(mcp_registry_t *reg);`
- `const mcp_tool_t *mcp_get_tools(mcp_registry_t *reg, int *count);`
- `char *mcp_call_tool(mcp_registry_t *reg, const char *tool_name, const char *arguments_json);`

## `md.h`

Function-like declarations: 5

### Declarations

- `void md_init(md_renderer_t *r, FILE *out);`
- `void md_feed(md_renderer_t *r, const char *text, size_t len);`
- `void md_feed_str(md_renderer_t *r, const char *text);`
- `void md_flush(md_renderer_t *r);`
- `void md_reset(md_renderer_t *r);`

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

## `plugin.h`

Function-like declarations: 7

### Declarations

- `void plugin_init(plugin_registry_t *reg);`
- `void plugin_reload(plugin_registry_t *reg);`
- `void plugin_cleanup(plugin_registry_t *reg);`
- `const tool_def_t *plugin_get_tools(const plugin_registry_t *reg, int *count);`
- `void plugin_list(const plugin_registry_t *reg, char *out, size_t out_len);`
- `bool plugin_load(plugin_registry_t *reg, const char *path);`
- `bool plugin_unload(plugin_registry_t *reg, const char *name);`

## `provider.h`

Function-like declarations: 7

### Declarations

- `char *(*build_request)(provider_t *p, conversation_t *conv, session_state_t *session, int max_tokens);`
- `struct curl_slist *(*build_headers)(provider_t *p, const char *api_key);`
- `stream_result_t (*stream)(provider_t *p, const char *api_key, const char *request_json, stream_text_cb text_cb, stream_tool_start_cb tool_cb, stream_thinking_cb thinking_cb, void *cb_ctx);`
- `provider_t *provider_create(const char *name);`
- `void provider_free(provider_t *p);`
- `const char *provider_detect(const char *model, const char *api_key);`
- `const char *provider_resolve_api_key(const char *provider_name);`

## `semantic.h`

Function-like declarations: 12

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
- `int sem_score_messages(tfidf_index_t *idx, const char *query, const char **msg_texts, int msg_count, msg_score_t *results, int max_results);`

## `setup.h`

Function-like declarations: 6

### Declarations

- `const char *dsco_setup_profile_name(void);`
- `int dsco_setup_load_saved_env(void);`
- `int dsco_setup_autopopulate(bool overwrite, bool include_generic, char *summary, size_t summary_len);`
- `int dsco_setup_bootstrap_from_env(char *summary, size_t summary_len);`
- `int dsco_setup_report(char *out, size_t out_len);`
- `const char *dsco_setup_env_path(void);`

## `swarm.h`

Function-like declarations: 18

### Declarations

- `typedef void (*swarm_stream_cb)(int child_id, const char *data, size_t len, void *ctx);`
- `void swarm_init(swarm_t *s, const char *api_key, const char *model);`
- `void swarm_destroy(swarm_t *s);`
- `int swarm_spawn(swarm_t *s, const char *task, const char *model);`
- `int swarm_spawn_in_group(swarm_t *s, int group_id, const char *task, const char *model);`
- `int swarm_group_create(swarm_t *s, const char *name);`
- `int swarm_group_dispatch(swarm_t *s, int group_id, const char **tasks, int task_count, const char *model);`
- `bool swarm_group_complete(swarm_t *s, int group_id);`
- `int swarm_poll(swarm_t *s, int timeout_ms);`
- `int swarm_poll_stream(swarm_t *s, int timeout_ms, swarm_stream_cb cb, void *ctx);`
- `swarm_child_t *swarm_get(swarm_t *s, int child_id);`
- `const char *swarm_status_str(swarm_status_t st);`
- `int swarm_active_count(swarm_t *s);`
- `bool swarm_kill(swarm_t *s, int child_id);`
- `void swarm_group_kill(swarm_t *s, int group_id);`
- `int swarm_status_json(swarm_t *s, char *buf, size_t len);`
- `int swarm_child_output(swarm_t *s, int child_id, char *buf, size_t len);`
- `int swarm_group_status_json(swarm_t *s, int group_id, char *buf, size_t len);`

## `tools.h`

Function-like declarations: 17

### Declarations

- `bool (*execute)(const char *input_json, char *result, size_t result_len);`
- `void tools_init(void);`
- `const tool_def_t *tools_get_all(int *count);`
- `int tools_builtin_count(void);`
- `bool tools_execute(const char *name, const char *input_json, char *result, size_t result_len);`
- `void tool_map_init(tool_map_t *m);`
- `void tool_map_free(tool_map_t *m);`
- `void tool_map_insert(tool_map_t *m, const char *name, int index);`
- `int tool_map_lookup(tool_map_t *m, const char *name);`
- `typedef char *(*external_tool_cb)(const char *name, const char *input_json, void *ctx);`
- `void tools_register_external(const char *name, const char *description, const char *input_schema_json, external_tool_cb cb, void *ctx);`
- `void dsco_locks_init(dsco_locks_t *l);`
- `void dsco_locks_destroy(dsco_locks_t *l);`
- `void watchdog_start(tool_watchdog_t *wd, pthread_t target, const char *name, int timeout_s);`
- `void watchdog_stop(tool_watchdog_t *wd);`
- `int tool_timeout_for(const char *name);`
- `bool tools_validate_input(const char *name, const char *input_json, char *error_buf, size_t error_len);`

## `tui.h`

Function-like declarations: 125

### Declarations

- `const tui_box_chars_t *tui_box_chars(tui_box_style_t style);`
- `int tui_term_width(void);`
- `int tui_term_height(void);`
- `void tui_cursor_hide(void);`
- `void tui_cursor_show(void);`
- `void tui_cursor_move(int row, int col);`
- `void tui_clear_screen(void);`
- `void tui_clear_line(void);`
- `void tui_save_cursor(void);`
- `void tui_restore_cursor(void);`
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
- `void tui_welcome(const char *model, int tool_count, const char *version);`
- `void tui_stream_start(void);`
- `void tui_stream_text(const char *text);`
- `void tui_stream_tool(const char *name, const char *id);`
- `void tui_stream_tool_result(const char *name, bool ok, const char *preview);`
- `void tui_stream_end(void);`
- `tui_color_level_t tui_detect_color_level(void);`
- `bool tui_supports_truecolor(void);`
- `tui_rgb_t tui_hsv_to_rgb(float h, float s, float v);`
- `void tui_fg_rgb(tui_rgb_t c);`
- `void tui_gradient_text(const char *text, float h_start, float h_end, float s, float v);`
- `void tui_gradient_divider(int width, float h_start, float h_end);`
- `void tui_transition_divider(void);`
- `tui_tool_type_t tui_classify_tool(const char *name);`
- `const char *tui_tool_color(tui_tool_type_t type);`
- `tui_rgb_t tui_tool_rgb(tui_tool_type_t type);`
- `void tui_async_spinner_start(tui_async_spinner_t *s, const char *label, tui_tool_type_t tool_type);`
- `void tui_async_spinner_stop(tui_async_spinner_t *s, bool ok, const char *result_preview, double elapsed_ms);`
- `void tui_batch_spinner_start(tui_batch_spinner_t *bs, const char **names, int count);`
- `void tui_batch_spinner_complete(tui_batch_spinner_t *bs, int idx, bool ok, const char *preview, double elapsed_ms);`
- `void tui_batch_spinner_stop(tui_batch_spinner_t *bs);`
- `void tui_status_bar_init(tui_status_bar_t *sb, const char *model);`
- `void tui_status_bar_update(tui_status_bar_t *sb, int in_tok, int out_tok, double cost, int turn, int tools);`
- `void tui_status_bar_enable(tui_status_bar_t *sb);`
- `void tui_status_bar_disable(tui_status_bar_t *sb);`
- `void tui_status_bar_render(tui_status_bar_t *sb);`
- `void tui_swarm_panel(tui_swarm_entry_t *entries, int count, int width);`
- `void tui_retry_pulse(const char *label, int attempt, int max, double wait_sec);`
- `void tui_sparkline(const double *values, int count, const char *color);`
- `bool tui_try_sparkline(const char *text);`
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
- `void tui_section_divider(int turn, int tools, double cost, const char *model);`
- `void tui_error_typed(tui_err_type_t type, const char *msg);`
- `void tui_notify(const char *title, const char *body);`
- `void tui_cadence_init(tui_cadence_t *c, void *md_renderer);`
- `void tui_cadence_feed(tui_cadence_t *c, const char *text);`
- `void tui_cadence_flush(tui_cadence_t *c);`
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
