#include "structured_process.h"
#include "json_util.h"
#include "plan.h"
#include "task_profile.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SP_MAX_STEPS 64
#define SP_MAX_ATOMS 128
#define SP_EXT_ID_LEN 32

static const char *SP_SCHEMA =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"schema_version\":{\"type\":\"string\",\"enum\":[\"dsco.structured_process.v1\"]},"
      "\"request\":{\"type\":\"object\",\"properties\":{"
        "\"raw_input\":{\"type\":\"string\"},"
        "\"intent\":{\"type\":\"string\",\"enum\":[\"chat\",\"code\",\"review\",\"research\",\"operate\",\"profile\",\"plan\"]},"
        "\"risk\":{\"type\":\"string\",\"enum\":[\"low\",\"medium\",\"high\"]},"
        "\"confidence\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}"
      "},\"required\":[\"raw_input\",\"intent\",\"risk\",\"confidence\"],\"additionalProperties\":false},"
      "\"budgets\":{\"type\":\"object\",\"properties\":{"
        "\"model_budget_pct\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
        "\"background_budget_pct\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
        "\"max_concurrency\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":64},"
        "\"max_iterations\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256}"
      "},\"required\":[\"model_budget_pct\",\"background_budget_pct\",\"max_concurrency\",\"max_iterations\"],\"additionalProperties\":false},"
      "\"goals\":{\"type\":\"array\",\"maxItems\":64,\"items\":{\"$ref\":\"#/$defs/goal\"}},"
      "\"steps\":{\"type\":\"array\",\"maxItems\":64,\"items\":{\"$ref\":\"#/$defs/step\"}},"
      "\"promotion_rules\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"$ref\":\"#/$defs/promotion_rule\"}},"
      "\"acceptance\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"string\"}}"
    "},"
    "\"required\":[\"schema_version\",\"request\",\"budgets\",\"goals\",\"steps\",\"promotion_rules\",\"acceptance\"],"
    "\"additionalProperties\":false,"
    "\"$defs\":{"
      "\"goal\":{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\"},"
        "\"parent_id\":{\"type\":\"string\"},"
        "\"title\":{\"type\":\"string\"},"
        "\"success_criteria\":{\"type\":\"array\",\"maxItems\":8,\"items\":{\"type\":\"string\"}}"
      "},\"required\":[\"id\",\"parent_id\",\"title\",\"success_criteria\"],\"additionalProperties\":false},"
      "\"step\":{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\"},"
        "\"parent_id\":{\"type\":\"string\"},"
        "\"goal_id\":{\"type\":\"string\"},"
        "\"title\":{\"type\":\"string\"},"
        "\"objective\":{\"type\":\"string\"},"
        "\"kind\":{\"type\":\"string\",\"enum\":[\"composite\",\"atomic\",\"gate\",\"dialog\",\"milestone\"]},"
        "\"priority\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
        "\"depends_on\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"string\"}},"
        "\"max_iterations\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256},"
        "\"atoms\":{\"type\":\"array\",\"maxItems\":64,\"items\":{\"$ref\":\"#/$defs/atom\"}}"
      "},\"required\":[\"id\",\"parent_id\",\"goal_id\",\"title\",\"objective\",\"kind\",\"priority\",\"depends_on\",\"max_iterations\",\"atoms\"],\"additionalProperties\":false},"
      "\"atom\":{\"type\":\"object\",\"properties\":{"
        "\"id\":{\"type\":\"string\"},"
        "\"title\":{\"type\":\"string\"},"
        "\"kind\":{\"type\":\"string\",\"enum\":[\"tool_call\",\"shell\",\"dialog\",\"assert\",\"noop\"]},"
        "\"lane\":{\"type\":\"string\",\"enum\":[\"local\",\"background\",\"model\",\"user\"]},"
        "\"tool_name\":{\"type\":\"string\"},"
        "\"input_json\":{\"type\":\"string\"},"
        "\"depends_on\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"string\"}},"
        "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000},"
        "\"max_attempts\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8},"
        "\"promotion_trigger\":{\"type\":\"string\",\"enum\":[\"never\",\"on_fail\",\"low_confidence\",\"blocked\",\"always\"]},"
        "\"model_tier\":{\"type\":\"string\",\"enum\":[\"none\",\"cheap\",\"smart\"]}"
      "},\"required\":[\"id\",\"title\",\"kind\",\"lane\",\"tool_name\",\"input_json\",\"depends_on\",\"timeout_ms\",\"max_attempts\",\"promotion_trigger\",\"model_tier\"],\"additionalProperties\":false},"
      "\"promotion_rule\":{\"type\":\"object\",\"properties\":{"
        "\"when\":{\"type\":\"string\",\"enum\":[\"on_fail\",\"low_confidence\",\"blocked\",\"budget_exhausted\",\"user_request\"]},"
        "\"promote_to\":{\"type\":\"string\",\"enum\":[\"smart_model\",\"user_dialog\",\"stop\"]},"
        "\"reason\":{\"type\":\"string\"}"
      "},\"required\":[\"when\",\"promote_to\",\"reason\"],\"additionalProperties\":false}"
    "}"
    "}";

typedef struct {
    char id[SP_EXT_ID_LEN];
    int  step_id;
} sp_step_map_t;

typedef struct {
    char id[SP_EXT_ID_LEN];
    int  atom_id;
    char deps[16][SP_EXT_ID_LEN];
    int  dep_count;
} sp_atom_map_t;

typedef struct {
    int plan_id;
    sp_step_map_t steps[SP_MAX_STEPS];
    int step_count;
    sp_atom_map_t atoms[SP_MAX_ATOMS];
    int atom_count;
} sp_import_ctx_t;

static bool contains_word(const char *text, const char *needle) {
    if (!text || !needle || !*needle)
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = text; *p; p++) {
        if ((p == text || !isalnum((unsigned char)p[-1])) &&
            strncasecmp(p, needle, nlen) == 0 &&
            !isalnum((unsigned char)p[nlen])) {
            return true;
        }
    }
    return false;
}

static const char *intent_name(sp_intent_t intent) {
    switch (intent) {
        case SP_INTENT_CODE: return "code";
        case SP_INTENT_REVIEW: return "review";
        case SP_INTENT_RESEARCH: return "research";
        case SP_INTENT_OPERATE: return "operate";
        case SP_INTENT_PROFILE: return "profile";
        case SP_INTENT_PLAN: return "plan";
        case SP_INTENT_CHAT:
        default: return "chat";
    }
}

static sp_intent_t classify_intent(const char *input) {
    if (contains_word(input, "flamegraph") || contains_word(input, "profile") ||
        contains_word(input, "instrument"))
        return SP_INTENT_PROFILE;
    if (contains_word(input, "review") || contains_word(input, "audit"))
        return SP_INTENT_REVIEW;
    if (contains_word(input, "implement") || contains_word(input, "fix") ||
        contains_word(input, "code") || contains_word(input, "build"))
        return SP_INTENT_CODE;
    if (contains_word(input, "research") || contains_word(input, "study") ||
        contains_word(input, "find"))
        return SP_INTENT_RESEARCH;
    if (contains_word(input, "run") || contains_word(input, "deploy") ||
        contains_word(input, "operate"))
        return SP_INTENT_OPERATE;
    if (contains_word(input, "plan") || contains_word(input, "decompose") ||
        contains_word(input, "goal"))
        return SP_INTENT_PLAN;
    return SP_INTENT_CHAT;
}

static bool high_risk_text(const char *input) {
    return contains_word(input, "delete") || contains_word(input, "remove") ||
           contains_word(input, "rm") || contains_word(input, "deploy") ||
           contains_word(input, "credential") || contains_word(input, "secret");
}

const char *structured_process_schema_json(void) {
    return SP_SCHEMA;
}

int structured_process_schema_response_format_json(char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;
    jbuf_t b;
    jbuf_init(&b, 8192);
    jbuf_append(&b, "{\"type\":\"json_schema\",\"name\":\"dsco_structured_process\",");
    jbuf_append(&b, "\"strict\":true,\"schema\":");
    jbuf_append(&b, SP_SCHEMA);
    jbuf_append(&b, "}");
    size_t copy = b.len < len - 1 ? b.len : len - 1;
    memcpy(buf, b.data, copy);
    buf[copy] = '\0';
    jbuf_free(&b);
    return (int)copy;
}

bool structured_process_classify(const char *input, sp_classification_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->intent = classify_intent(input ? input : "");
    snprintf(out->intent_name, sizeof(out->intent_name), "%s", intent_name(out->intent));
    snprintf(out->risk, sizeof(out->risk), "%s", high_risk_text(input) ? "high" : "low");
    out->confidence = 72;
    out->model_budget_pct = 20;
    out->background_budget_pct = 80;
    out->max_concurrency = 4;
    out->max_iterations = 16;
    out->can_background = true;

    task_profile_t *tp = task_profile(input ? input : "", NULL);
    if (tp) {
        int complexity = (int)(tp->complexity_score * 100.0);
        int convergence = (int)(tp->convergence_score * 100.0);
        out->confidence = 65 + (tp->keyword_match_count > 8 ? 15 : tp->keyword_match_count);
        if (out->confidence > 95)
            out->confidence = 95;
        out->needs_model_gate = complexity >= 65 || convergence >= 65 || high_risk_text(input);
        out->max_concurrency = tp->parallelism_score >= 0.7 ? 8 : 4;
        if (tp->latency_score >= 0.8)
            out->max_concurrency = 2;
        task_profile_free(tp);
    } else {
        out->needs_model_gate = false;
    }
    if (out->needs_model_gate && out->model_budget_pct < 20)
        out->model_budget_pct = 20;
    return true;
}

static void append_json_string_array(jbuf_t *b, const char **items, int count) {
    jbuf_append_char(b, '[');
    for (int i = 0; i < count; i++) {
        if (i)
            jbuf_append_char(b, ',');
        jbuf_append_json_str(b, items[i] ? items[i] : "");
    }
    jbuf_append_char(b, ']');
}

static void append_atom(jbuf_t *b, const char *id, const char *title, const char *kind,
                        const char *lane, const char *tool, const char *input_json,
                        const char *dep) {
    jbuf_append(b, "{\"id\":");
    jbuf_append_json_str(b, id);
    jbuf_append(b, ",\"title\":");
    jbuf_append_json_str(b, title);
    jbuf_append(b, ",\"kind\":");
    jbuf_append_json_str(b, kind);
    jbuf_append(b, ",\"lane\":");
    jbuf_append_json_str(b, lane);
    jbuf_append(b, ",\"tool_name\":");
    jbuf_append_json_str(b, tool ? tool : "");
    jbuf_append(b, ",\"input_json\":");
    jbuf_append_json_str(b, input_json ? input_json : "{}");
    jbuf_append(b, ",\"depends_on\":");
    if (dep && *dep) {
        const char *deps[1] = {dep};
        append_json_string_array(b, deps, 1);
    } else {
        jbuf_append(b, "[]");
    }
    jbuf_append(b, ",\"timeout_ms\":30000,\"max_attempts\":1,\"promotion_trigger\":");
    jbuf_append_json_str(b, strcmp(lane, "model") == 0 ? "low_confidence" : "on_fail");
    jbuf_append(b, ",\"model_tier\":");
    jbuf_append_json_str(b, strcmp(lane, "model") == 0 ? "smart" : "none");
    jbuf_append_char(b, '}');
}

static void append_step_open(jbuf_t *b, const char *id, const char *parent_id, const char *goal_id,
                             const char *title, const char *objective, const char *kind,
                             int priority) {
    jbuf_append(b, "{\"id\":");
    jbuf_append_json_str(b, id);
    jbuf_append(b, ",\"parent_id\":");
    jbuf_append_json_str(b, parent_id ? parent_id : "");
    jbuf_append(b, ",\"goal_id\":");
    jbuf_append_json_str(b, goal_id ? goal_id : "");
    jbuf_append(b, ",\"title\":");
    jbuf_append_json_str(b, title);
    jbuf_append(b, ",\"objective\":");
    jbuf_append_json_str(b, objective);
    jbuf_append(b, ",\"kind\":");
    jbuf_append_json_str(b, kind);
    jbuf_appendf(b, ",\"priority\":%d,\"depends_on\":[],\"max_iterations\":1,\"atoms\":[", priority);
}

int structured_process_synthesize_json(const char *input, char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;
    sp_classification_t c;
    structured_process_classify(input, &c);

    jbuf_t b;
    jbuf_init(&b, 8192);
    jbuf_append(&b, "{\"schema_version\":\"dsco.structured_process.v1\",\"request\":{");
    jbuf_append(&b, "\"raw_input\":");
    jbuf_append_json_str(&b, input ? input : "");
    jbuf_append(&b, ",\"intent\":");
    jbuf_append_json_str(&b, c.intent_name);
    jbuf_append(&b, ",\"risk\":");
    jbuf_append_json_str(&b, c.risk);
    jbuf_appendf(&b, ",\"confidence\":%d},\"budgets\":{", c.confidence);
    jbuf_appendf(&b, "\"model_budget_pct\":%d,\"background_budget_pct\":%d,"
                    "\"max_concurrency\":%d,\"max_iterations\":%d},",
                 c.model_budget_pct, c.background_budget_pct, c.max_concurrency,
                 c.max_iterations);

    jbuf_append(&b, "\"goals\":[{\"id\":\"g1\",\"parent_id\":\"\",\"title\":");
    jbuf_append_json_str(&b, input && *input ? input : "Handle request");
    jbuf_append(&b, ",\"success_criteria\":[\"all required atoms are complete\","
                   "\"no step exceeds its iteration or attempt budget\"]}],\"steps\":[");

    append_step_open(&b, "s1", "", "g1", "Classify and bound request",
                     "Produce a typed route, risk level, and execution budget.", "atomic", 100);
    append_atom(&b, "a1", "Classify request locally", "noop", "background", "", "{}", "");
    jbuf_append(&b, "]},");

    append_step_open(&b, "s2", "", "g1", "Gather local execution context",
                     "Collect cheap deterministic context before spending model budget.",
                     "atomic", 80);
    append_atom(&b, "a2", "Read current working directory", "tool_call", "background",
                "cwd", "{}", "a1");
    jbuf_append(&b, "]},");

    append_step_open(&b, "s3", "", "g1", "Decompose into bounded atoms",
                     "Create small units that can run in the background and promote only when blocked.",
                     "atomic", 70);
    append_atom(&b, "a3", "Build deterministic work queue", "noop", "background", "",
                "{}", "a2");
    jbuf_append_char(&b, ',');
    append_atom(&b, "a4",
                c.needs_model_gate ? "Review decomposition with smart model"
                                   : "Hold smart-model promotion gate",
                "noop", "model", "", "{}", "a3");
    jbuf_append(&b, "]}],\"promotion_rules\":[");
    jbuf_append(&b, "{\"when\":\"on_fail\",\"promote_to\":\"smart_model\","
                   "\"reason\":\"failed deterministic atom needs model judgment\"},");
    jbuf_append(&b, "{\"when\":\"blocked\",\"promote_to\":\"user_dialog\","
                   "\"reason\":\"missing external decision or unsafe action\"}");
    jbuf_append(&b, "],\"acceptance\":[\"plan is valid JSON matching the structured process schema\","
                   "\"background atoms are bounded by max_attempts and max_iterations\","
                   "\"smart model use is reserved for promotion gates\"]}");

    size_t copy = b.len < len - 1 ? b.len : len - 1;
    memcpy(buf, b.data, copy);
    buf[copy] = '\0';
    jbuf_free(&b);
    return (int)copy;
}

static step_type_t parse_step_kind(const char *kind) {
    if (!kind)
        return STEP_COMPOSITE;
    return step_type_parse(kind);
}

static atom_type_t parse_atom_kind(const char *kind) {
    if (!kind)
        return ATOM_NOOP;
    if (strcmp(kind, "tool_call") == 0)
        return ATOM_TOOL_CALL;
    return atom_type_parse(kind);
}

static bool parse_string_value(const char *p, char *buf, size_t len) {
    if (!p || !buf || len == 0)
        return false;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '"')
        return false;
    p++;
    size_t w = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            if (c == 'n')
                c = '\n';
            else if (c == 't')
                c = '\t';
            else if (c == 'r')
                c = '\r';
        }
        if (w + 1 < len)
            buf[w++] = c;
    }
    buf[w] = '\0';
    return true;
}

static int find_step_id(sp_import_ctx_t *ctx, const char *ext_id) {
    if (!ctx || !ext_id || !*ext_id)
        return 0;
    for (int i = 0; i < ctx->step_count; i++) {
        if (strcmp(ctx->steps[i].id, ext_id) == 0)
            return ctx->steps[i].step_id;
    }
    return 0;
}

static int find_atom_id(sp_import_ctx_t *ctx, const char *ext_id) {
    if (!ctx || !ext_id || !*ext_id)
        return 0;
    for (int i = 0; i < ctx->atom_count; i++) {
        if (strcmp(ctx->atoms[i].id, ext_id) == 0)
            return ctx->atoms[i].atom_id;
    }
    return 0;
}

static void dep_cb(const char *element, void *opaque) {
    sp_atom_map_t *m = (sp_atom_map_t *)opaque;
    if (!m || m->dep_count >= 16)
        return;
    parse_string_value(element, m->deps[m->dep_count], sizeof(m->deps[m->dep_count]));
    if (m->deps[m->dep_count][0])
        m->dep_count++;
}

typedef struct {
    sp_import_ctx_t *ctx;
    int step_id;
} sp_atom_ctx_t;

static void atom_cb(const char *element, void *opaque) {
    sp_atom_ctx_t *actx = (sp_atom_ctx_t *)opaque;
    sp_import_ctx_t *ctx = actx ? actx->ctx : NULL;
    if (!ctx || ctx->atom_count >= SP_MAX_ATOMS)
        return;
    char *id = json_get_str(element, "id");
    char *title = json_get_str(element, "title");
    char *kind = json_get_str(element, "kind");
    char *lane = json_get_str(element, "lane");
    char *tool_name = json_get_str(element, "tool_name");
    char *input_json = json_get_str(element, "input_json");

    int atom_id = step_add_atom(actx->step_id, title ? title : "atom", parse_atom_kind(kind));
    if (atom_id > 0) {
        atom_t *a = atom_get(atom_id);
        if (a) {
            if (a->type == ATOM_TOOL_CALL)
                atom_set_tool(atom_id, tool_name ? tool_name : "", input_json ? input_json : "{}");
            else if (a->type == ATOM_SHELL)
                atom_set_shell(atom_id, input_json ? input_json : "");
            else if (a->type == ATOM_DIALOG)
                atom_set_dialog_prompt(atom_id, input_json ? input_json : "");
            else if (a->type == ATOM_ASSERT)
                atom_set_assert(atom_id, input_json ? input_json : "true");
        }
        sp_atom_map_t *m = &ctx->atoms[ctx->atom_count++];
        snprintf(m->id, sizeof(m->id), "%s", id ? id : "");
        m->atom_id = atom_id;
        json_array_foreach(element, "depends_on", dep_cb, m);
        if (lane && *lane) {
            char note[96];
            snprintf(note, sizeof(note), "lane=%s", lane);
            step_add_note(actx->step_id, note);
        }
    }

    free(id);
    free(title);
    free(kind);
    free(lane);
    free(tool_name);
    free(input_json);
}

static void step_cb(const char *element, void *opaque) {
    sp_import_ctx_t *ctx = (sp_import_ctx_t *)opaque;
    if (!ctx || ctx->step_count >= SP_MAX_STEPS)
        return;
    char *id = json_get_str(element, "id");
    char *parent_id = json_get_str(element, "parent_id");
    char *title = json_get_str(element, "title");
    char *objective = json_get_str(element, "objective");
    char *kind = json_get_str(element, "kind");
    int priority = json_get_int(element, "priority", 0);
    int parent_step_id = find_step_id(ctx, parent_id ? parent_id : "");
    int step_id = plan_add_step(ctx->plan_id, parent_step_id, title ? title : "step",
                                parse_step_kind(kind));
    if (step_id > 0) {
        step_set_description(step_id, objective ? objective : "");
        step_set_priority(step_id, priority);
        sp_step_map_t *m = &ctx->steps[ctx->step_count++];
        snprintf(m->id, sizeof(m->id), "%s", id ? id : "");
        m->step_id = step_id;
        sp_atom_ctx_t actx = {.ctx = ctx, .step_id = step_id};
        json_array_foreach(element, "atoms", atom_cb, &actx);
    }
    free(id);
    free(parent_id);
    free(title);
    free(objective);
    free(kind);
}

int structured_process_create_plan_from_json(const char *process_json) {
    if (!process_json || !json_is_valid_container(process_json))
        return -1;
    char *schema_version = json_get_str(process_json, "schema_version");
    if (!schema_version || strcmp(schema_version, "dsco.structured_process.v1") != 0) {
        free(schema_version);
        return -1;
    }
    free(schema_version);

    char *request = json_get_raw(process_json, "request");
    char *raw_input = request ? json_get_str(request, "raw_input") : NULL;
    char *intent = request ? json_get_str(request, "intent") : NULL;
    char title[256];
    snprintf(title, sizeof(title), "%s request", intent ? intent : "structured");

    int plan_id = plan_create(title, raw_input ? raw_input : "", PLAN_MODE_HYBRID);
    if (plan_id < 0) {
        free(request);
        free(raw_input);
        free(intent);
        return -1;
    }

    sp_import_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.plan_id = plan_id;
    json_array_foreach(process_json, "steps", step_cb, &ctx);

    for (int i = 0; i < ctx.atom_count; i++) {
        for (int j = 0; j < ctx.atoms[i].dep_count; j++) {
            int src = find_atom_id(&ctx, ctx.atoms[i].deps[j]);
            if (src > 0)
                atom_wire(src, ctx.atoms[i].atom_id, NULL);
        }
    }

    free(request);
    free(raw_input);
    free(intent);
    return plan_id;
}
