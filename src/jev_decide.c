#include "jev_decide.h"
#include "capability.h"
#include "crypto.h"
#include "json_util.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* jev_decide.c — general cheap typed-decision client. See jev_decide.h.
 * Mirrors jev_router's governance: capability-gated, bounded, validated,
 * recommend-only, receipt-backed. The HTTP goes through the governed
 * tools_execute_for_tier("curl_raw") path exactly like jev_router. */

static atomic_uint g_attempts = 0;
#define JEV_DECIDE_ATTEMPT_LIMIT 32

static bool finish(char *out, size_t len, const char *status) {
    if (out && len)
        snprintf(out, len,
            "{\"schema\":\"dsco.jev_decide.v1\",\"status\":\"%s\",\"applied\":false}", status);
    return false;
}

static bool prob(yyjson_val *v) {
    return yyjson_is_num(v) && isfinite(yyjson_get_num(v)) &&
           yyjson_get_num(v) >= 0 && yyjson_get_num(v) <= 1;
}

/* JSON escaping is not shell escaping: a user's apostrophe must survive as
 * data, never terminate curl's quoted --data-raw argument. */
static void quote_shell(jbuf_t *out, const char *s) {
    jbuf_append(out, "'");
    for (; *s; s++) {
        if (*s == '\'') jbuf_append(out, "'\\''");
        else { char c[2] = {*s, 0}; jbuf_append(out, c); }
    }
    jbuf_append(out, "'");
}

static const char *kind_name(jev_decide_kind_t k) {
    switch (k) {
        case JEV_DECIDE_BOOL: return "noul";
        case JEV_DECIDE_SCORE: return "score";
        case JEV_DECIDE_CHOICE: return "choice";
    }
    return "noul";
}

/* Execute one systemone call with a single question "q" of the given kind.
 * On success fills result and writes an OK receipt; on any failure writes a
 * finish() receipt and returns false. Shared by all three typed wrappers. */
static bool decide_call(jev_decide_kind_t kind, const char *domain,
                        const char *state, const char *instruction,
                        const char *const *options, size_t n_options,
                        jev_decide_result_t *result,
                        char *receipt, size_t rlen, const char *tier) {
    if (!result || !receipt || rlen < 256) return finish(receipt, rlen, "invalid_output");
    memset(result, 0, sizeof(*result));
    result->kind = kind;
    if (!domain || !domain[0]) domain = "general";
    if (!state || !state[0]) return finish(receipt, rlen, "empty_state");
    if (!instruction || !instruction[0]) return finish(receipt, rlen, "empty_instruction");
    if (strnlen(state, JEV_DECIDE_STATE_MAX + 1) > JEV_DECIDE_STATE_MAX)
        return finish(receipt, rlen, "state_too_large");
    if (strnlen(instruction, JEV_DECIDE_INSTRUCTION_MAX + 1) > JEV_DECIDE_INSTRUCTION_MAX)
        return finish(receipt, rlen, "instruction_too_large");

    /* Options required for CHOICE (2..64) and SCORE (2..10); ignored for BOOL. */
    if (kind == JEV_DECIDE_CHOICE) {
        if (!options || n_options < 2 || n_options > JEV_DECIDE_MAX_OPTIONS)
            return finish(receipt, rlen, "invalid_options");
    } else if (kind == JEV_DECIDE_SCORE) {
        if (!options || n_options < 2 || n_options > 10)
            return finish(receipt, rlen, "invalid_score_levels");
    }
    if (options) {
        for (size_t i = 0; i < n_options; i++) {
            if (!options[i] || !options[i][0] ||
                strnlen(options[i], JEV_DECIDE_OPTION_MAX + 1) > JEV_DECIDE_OPTION_MAX)
                return finish(receipt, rlen, "invalid_option");
        }
    }

    if (atomic_load(&g_attempts) >= JEV_DECIDE_ATTEMPT_LIMIT)
        return finish(receipt, rlen, "attempt_limit");
    if (!tier || !*tier) tier = "standard";
    if (!dsco_cap_granted(CAP_SECRETS, tier)) return finish(receipt, rlen, "secrets_denied");
    const char *key = getenv("TYPESAFE_API_KEY");
    if (!key || !*key) return finish(receipt, rlen, "missing_key");

    /* Build the request body: single question "q" of the requested kind. */
    jbuf_t body; jbuf_init(&body, 4096);
    jbuf_append(&body, "{\"model\":\"" JEV_DECIDE_MODEL "\",\"state\":");
    jbuf_append_json_str(&body, state);
    jbuf_append(&body, ",\"questions\":{\"q\":{\"type\":\"");
    jbuf_append(&body, kind_name(kind));
    jbuf_append(&body, "\",\"instructions\":");
    jbuf_append_json_str(&body, instruction);
    if (kind == JEV_DECIDE_CHOICE || kind == JEV_DECIDE_SCORE) {
        /* System One expects criteria as an ordered list, not the legacy
         * label->null object. Keeping the caller's labels preserves the
         * typed score/choice contract while matching the live API schema. */
        jbuf_append(&body, ",\"criteria\":[");
        for (size_t i = 0; i < n_options; i++) {
            if (i) jbuf_append(&body, ",");
            jbuf_append_json_str(&body, options[i]);
        }
        jbuf_append(&body, "]");
    }
    jbuf_append(&body, "}}}");
    char payload_hash[65];
    sha256_hex((const unsigned char *)body.data, body.len, payload_hash);

    jbuf_t args; jbuf_init(&args, 8192);
    jbuf_append(&args, "-q --silent --show-error --fail --proto '=https' --max-redirs 0 "
        "--connect-timeout 1 --max-time 2 --max-filesize 16384 "
        "--variable %TYPESAFE_API_KEY --expand-header 'Authorization: Bearer {{TYPESAFE_API_KEY}}' "
        "--header 'Content-Type: application/json' --data-raw ");
    quote_shell(&args, body.data);
    jbuf_append(&args, " https://api.typesafe.ai/v1/systemone");
    jbuf_free(&body);
    if (args.len + 6 >= 8192) { jbuf_free(&args); return finish(receipt, rlen, "request_too_large"); }

    unsigned used = atomic_load(&g_attempts);
    do {
        if (used >= JEV_DECIDE_ATTEMPT_LIMIT) { jbuf_free(&args); return finish(receipt, rlen, "attempt_limit"); }
    } while (!atomic_compare_exchange_weak(&g_attempts, &used, used + 1));
    dsco_flow_note(CAP_SECRETS);

    jbuf_t input; jbuf_init(&input, 8192);
    jbuf_append(&input, "{\"args\":");
    jbuf_append_json_str(&input, args.data);
    jbuf_append(&input, "}");
    jbuf_free(&args);
    char response[16384] = {0};
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    bool ok = tools_execute_for_tier("curl_raw", input.data, tier, response, sizeof(response));
    clock_gettime(CLOCK_MONOTONIC, &b);
    jbuf_free(&input);
    double elapsed = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1000000.0;
    if (!ok) return finish(receipt, rlen, "transport_denied_or_failed");

    yyjson_doc *doc = yyjson_read(response, strnlen(response, sizeof(response)), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *answers = yyjson_obj_get(root, "answers");
    yyjson_val *answer = yyjson_obj_get(answers, "q");
    yyjson_val *tokens = yyjson_obj_get(yyjson_obj_get(root, "usage"), "input_tokens");
    bool valid = yyjson_equals_str(yyjson_obj_get(root, "model"), JEV_DECIDE_MODEL) &&
                 yyjson_is_obj(answers) && yyjson_is_obj(answer) && yyjson_is_uint(tokens);

    double confidence = 0.0;
    if (valid && kind == JEV_DECIDE_BOOL) {
        yyjson_val *v = yyjson_obj_get(answer, "noul");
        valid = yyjson_equals_str(yyjson_obj_get(answer, "type"), "noul") && prob(v);
        if (valid) { result->p_yes = yyjson_get_num(v); confidence = result->p_yes; }
    } else if (valid && kind == JEV_DECIDE_SCORE) {
        yyjson_val *sc = yyjson_obj_get(answer, "score");
        yyjson_val *cf = yyjson_obj_get(answer, "confidence");
        valid = yyjson_equals_str(yyjson_obj_get(answer, "type"), "score") &&
                yyjson_is_num(sc) && prob(cf);
        if (valid) {
            result->score = yyjson_get_num(sc);
            /* System One has historically emitted either the requested
             * labelled scale (e.g. 1..10) or a normalized 0..1 value for
             * the same score question. Normalize the latter into the
             * caller's numeric criteria scale so typed decisions never
             * leak mixed units. */
            if (result->score >= 0.0 && result->score <= 1.0 && n_options >= 2) {
                char *end_lo = NULL, *end_hi = NULL;
                double lo = strtod(options[0], &end_lo);
                double hi = strtod(options[n_options - 1], &end_hi);
                if (end_lo && *end_lo == '\0' && end_hi && *end_hi == '\0' && hi > lo)
                    result->score = lo + result->score * (hi - lo);
            }
            confidence = yyjson_get_num(cf);
        }
    } else if (valid && kind == JEV_DECIDE_CHOICE) {
        yyjson_val *dist = yyjson_obj_get(answer, "probabilities");
        yyjson_val *cf = yyjson_obj_get(answer, "confidence");
        const char *choice = yyjson_get_str(yyjson_obj_get(answer, "choice"));
        valid = yyjson_equals_str(yyjson_obj_get(answer, "type"), "choice") &&
                yyjson_is_obj(dist) && prob(cf) && choice &&
                yyjson_obj_size(dist) == n_options;
        double sum = 0, maximum = 0, chosen = -1;
        for (size_t i = 0; valid && i < n_options; i++) {
            yyjson_val *v = yyjson_obj_get(dist, options[i]);
            if (!prob(v)) { valid = false; break; }
            double p = yyjson_get_num(v); sum += p; if (p > maximum) maximum = p;
            if (!strcmp(choice, options[i])) chosen = p;
        }
        valid = valid && chosen >= 0 && fabs(sum - 1) <= .005 * (n_options + 1) + .000001 &&
                chosen + .010001 >= maximum;
        if (valid) {
            snprintf(result->choice, sizeof(result->choice), "%s", choice);
            confidence = yyjson_get_num(cf);
        }
    }
    if (!valid) { yyjson_doc_free(doc); return finish(receipt, rlen, "invalid_response"); }

    unsigned long long nt = yyjson_get_uint(tokens);
    if (nt > 1000000) { yyjson_doc_free(doc); return finish(receipt, rlen, "invalid_usage"); }
    result->confidence = confidence;

    int written = snprintf(receipt, rlen,
        "{\"schema\":\"dsco.jev_decide.v1\",\"status\":\"ok\",\"domain\":\"%s\",\"kind\":\"%s\","
        "\"requested_model\":\"" JEV_DECIDE_MODEL "\",\"payload_sha256\":\"%s\",\"confidence\":%.4f,"
        "\"input_tokens\":%llu,\"latency_ms\":%.3f,\"attempt\":%u,\"applied\":false}",
        domain, kind_name(kind), payload_hash, confidence, nt, elapsed, used + 1);
    yyjson_doc_free(doc);
    if (written < 0 || (size_t)written >= rlen) return finish(receipt, rlen, "receipt_too_large");
    return true;
}

bool jev_decide(jev_decide_kind_t kind, const char *domain, const char *state,
                const char *instruction, const char *const *options, size_t n_options,
                jev_decide_result_t *result, char *receipt, size_t receipt_len,
                const char *tier) {
    return decide_call(kind, domain, state, instruction, options, n_options,
                       result, receipt, receipt_len, tier);
}

bool jev_decide_bool(const char *domain, const char *state, const char *instruction,
                     double threshold, bool *decision_out, double *p_yes_out,
                     char *receipt, size_t receipt_len, const char *tier) {
    if (decision_out) *decision_out = false;
    if (p_yes_out) *p_yes_out = 0.0;
    if (threshold < 0.0) threshold = 0.0;
    if (threshold > 1.0) threshold = 1.0;
    jev_decide_result_t r;
    if (!decide_call(JEV_DECIDE_BOOL, domain, state, instruction, NULL, 0,
                     &r, receipt, receipt_len, tier))
        return false;
    if (p_yes_out) *p_yes_out = r.p_yes;
    if (decision_out) *decision_out = r.p_yes >= threshold;
    return true;
}

/* ------------------------------------------------------------------ */
/* Compaction domain: tiered turn summaries with context keys.         */
/* ------------------------------------------------------------------ */

/* Summarize `state` (a block of older turns) down to a target level, returning
 * a compact summary plus a content-addressed context key for later retrieval.
 * Levels compress progressively: a higher level is a shorter, more abstract
 * summary. The context key lets the original (or this summary) be fetched from
 * the CAS/memory layer when detail is needed again. */
bool jev_compact_turns(const char *domain, const char *turns_text,
                       int level, char *summary, size_t summary_len,
                       char *context_key, size_t context_key_len,
                       char *receipt, size_t receipt_len, const char *tier) {
    if (!summary || summary_len < 64 || !context_key || context_key_len < 16)
        return finish(receipt, receipt_len, "invalid_output");
    summary[0] = '\0';
    if (!turns_text || !turns_text[0]) return finish(receipt, receipt_len, "empty_state");
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    if (!domain || !domain[0]) domain = "compact";

    /* The context key is content-addressed: sha256 of the source turns +
     * level, so identical compaction requests dedup and the key always
     * resolves to the same artifact in the memory layer. */
    {
        jbuf_t mk; jbuf_init(&mk, 1024);
        jbuf_appendf(&mk, "compact:v1:L%d:", level);
        jbuf_append(&mk, turns_text);
        char h[65];
        sha256_hex((const unsigned char *)mk.data, mk.len, h);
        jbuf_free(&mk);
        h[24] = '\0';
        snprintf(context_key, context_key_len, "ctx:compact:L%d:%s", level, h);
    }

    /* Instruction varies by level: higher levels demand terser, more abstract
     * summaries preserving only decisions, open threads, and durable facts. */
    char instr[JEV_DECIDE_INSTRUCTION_MAX];
    const char *detail;
    switch (level) {
        case 1: detail = "a detailed summary preserving most specifics, names, numbers, and reasoning"; break;
        case 2: detail = "a concise summary of the key points, decisions, and conclusions"; break;
        case 3: detail = "a terse abstract of only the durable facts, decisions made, and open threads"; break;
        default: detail = "a one-paragraph gist of only what must not be forgotten"; break;
    }
    snprintf(instr, sizeof(instr),
        "Summarize this conversation segment as %s. Preserve: decisions made, "
        "open questions/threads, durable facts and figures, and anything later "
        "turns depend on. Drop: filler, repetition, and transient detail. "
        "Output only the summary text.", detail);

    /* Reuse the governed call: a Score on summary length is not what we want;
     * we need text back. But the typed API returns decisions, not prose. The
     * compaction therefore uses a Noul-guided extractive approach: ask which
     * candidate sentences to KEEP, then concatenate the kept ones. This is a
     * typed decision (keep/drop per sentence) that composes into a summary. */

    /* Split turns_text into sentences (bounded). */
    char buf[JEV_DECIDE_STATE_MAX + 1];
    snprintf(buf, sizeof(buf), "%s", turns_text);
    char *sents[64];
    size_t ns = 0;
    for (char *p = buf; *p && ns < 64; ) {
        sents[ns++] = p;
        char *e = strpbrk(p, ".!?\n");
        if (!e) break;
        *e = '\0';
        p = e + 1;
        while (*p == ' ' || *p == '\n') p++;
    }
    if (ns == 0) { return finish(receipt, receipt_len, "no_sentences"); }

    /* Keep fraction by level: L1 keeps most, L4 keeps least. */
    double keep_frac = (level == 1) ? 0.8 : (level == 2) ? 0.5 : (level == 3) ? 0.3 : 0.15;

    /* One Noul per sentence: keep? (batched in a single request would be ideal;
     * the governed client is single-question, so we do a bounded loop — but
     * the attempt budget (32) caps total work. For long segments the caller
     * should pre-chunk; here we keep the highest-value sentences within budget. */
    jbuf_t out; jbuf_init(&out, summary_len);
    size_t kept = 0;
    /* We ask "is this sentence important enough to keep?" and keep p>=0.5,
     * stopping when we reach the level's target count. */
    size_t target = (size_t)(ns * keep_frac) + 1;
    for (size_t i = 0; i < ns && kept < target; i++) {
        if (!sents[i] || !sents[i][0]) continue;
        char sinstr[JEV_DECIDE_INSTRUCTION_MAX];
        snprintf(sinstr, sizeof(sinstr),
            "In compacting a conversation to level %d, is this sentence important "
            "enough to preserve (a decision, durable fact, open thread, or something "
            "later turns depend on)? Sentence: %s", level, sents[i]);
        bool keep = false; double p = 0.0;
        char rbuf[JEV_DECIDE_RECEIPT_MAX];
        if (!jev_decide_bool(domain, turns_text, sinstr, 0.5, &keep, &p,
                             rbuf, sizeof(rbuf), tier)) {
            /* On any failure, fail closed for the whole compaction. */
            jbuf_free(&out);
            return finish(receipt, receipt_len, "sentence_decision_failed");
        }
        if (keep) {
            jbuf_append(&out, sents[i]);
            jbuf_append(&out, ". ");
            kept++;
        }
    }

    if (out.len + 1 >= summary_len) {
        /* Truncated but usable; still a valid compaction. */
    }
    snprintf(summary, summary_len, "%s", out.data ? out.data : "");
    jbuf_free(&out);

    /* Final receipt records the compaction ratio and context key. */
    size_t in_chars = strnlen(turns_text, JEV_DECIDE_STATE_MAX);
    size_t out_chars = strnlen(summary, summary_len);
    char r2[JEV_DECIDE_RECEIPT_MAX];
    snprintf(r2, sizeof(r2),
        "{\"schema\":\"dsco.jev_compact.v1\",\"status\":\"ok\",\"domain\":\"%s\",\"level\":%d,"
        "\"context_key\":\"%s\",\"sentences\":%zu,\"kept\":%zu,\"in_chars\":%zu,\"out_chars\":%zu,"
        "\"compression\":%.3f}",
        domain, level, context_key, ns, kept, in_chars, out_chars,
        in_chars ? (double)out_chars / (double)in_chars : 0.0);
    if (receipt && receipt_len) snprintf(receipt, receipt_len, "%s", r2);
    return true;
}
