#include "skill_lifecycle.h"
#include "json_util.h"
#include "tools.h"
#include "workspace.h"
#include "../vendor/yyjson.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKILL_LIMIT 32768
#define RECEIPT_LIMIT 196608

static bool fail(char *out, size_t size, const char *message) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":false,\"kind\":\"skill\",\"status\":\"error\",\"error\":");
    jbuf_append_json_str(&b, message);
    jbuf_append(&b, "}");
    if (size)
        snprintf(out, size, "%s", b.len < size ? b.data : "{}");
    jbuf_free(&b);
    return false;
}

/* Deliberately small portable frontmatter subset. New lifecycle writes accept
 * single-line name/description (quoted values supported); legacy skills remain
 * readable through /skills. Unknown metadata and body bytes are preserved. */
static bool metadata(const char *body, char name[65], char description[1025]) {
    if (!body || strlen(body) > SKILL_LIMIT || strncmp(body, "---\n", 4))
        return false;
    name[0] = description[0] = 0;
    const char *p = body + 4;
    bool closed = false;
    while (*p) {
        const char *end = strchr(p, '\n');
        if (!end)
            return false;
        size_t len = (size_t)(end - p);
        if (len == 3 && !strncmp(p, "---", 3)) {
            closed = true;
            break;
        }
        char *dest = NULL;
        size_t cap = 0, prefix = 0;
        if (len >= 5 && !strncmp(p, "name:", 5)) {
            dest = name;
            cap = 65;
            prefix = 5;
        }
        if (len >= 12 && !strncmp(p, "description:", 12)) {
            dest = description;
            cap = 1025;
            prefix = 12;
        }
        if (dest) {
            if (*dest)
                return false;
            const char *value = p + prefix;
            while (value < end && (*value == ' ' || *value == '\t'))
                value++;
            const char *tail = end;
            while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t' || tail[-1] == '\r'))
                tail--;
            if (tail > value && (*value == '\'' || *value == '"')) {
                if (tail[-1] != *value || tail - value < 2)
                    return false;
                value++;
                tail--;
            }
            size_t n = (size_t)(tail - value);
            if (!n || n >= cap || *value == '|' || *value == '>')
                return false;
            memcpy(dest, value, n);
            dest[n] = 0;
        }
        p = end + 1;
    }
    if (!closed || !*name || !*description || name[0] == '-' || name[strlen(name) - 1] == '-' ||
        strstr(name, "--"))
        return false;
    for (const char *q = name; *q; q++)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '-'))
            return false;
    return true;
}

static bool nested(const char *tool, jbuf_t *input, char *out, size_t size) {
    return tools_execute_for_tier(tool, input->data, tools_execution_tier(), out, size);
}

static yyjson_doc *inspect(const char *path, char *result) {
    jbuf_t input;
    jbuf_init(&input, 256);
    jbuf_append(&input, "{\"path\":");
    jbuf_append_json_str(&input, path);
    jbuf_append(&input, ",\"limit\":1,\"include_content\":true}");
    bool ok = nested("code_read", &input, result, RECEIPT_LIMIT);
    jbuf_free(&input);
    return ok ? yyjson_read(result, strlen(result), 0) : NULL;
}

static bool replace(const char *path, const char *expected, const char *body, bool preview,
                    char *result) {
    jbuf_t input;
    jbuf_init(&input, 1024);
    jbuf_append(&input, "{\"path\":");
    jbuf_append_json_str(&input, path);
    jbuf_append(&input, ",\"expected_sha256\":");
    jbuf_append_json_str(&input, expected);
    jbuf_append(&input, ",\"content\":");
    jbuf_append_json_str(&input, body);
    jbuf_appendf(&input, ",\"dry_run\":%s}", preview ? "true" : "false");
    bool ok = nested("code_edit", &input, result, RECEIPT_LIMIT);
    jbuf_free(&input);
    return ok;
}

bool skill_lifecycle_execute(const char *input, char *out, size_t size) {
    if (!input || strlen(input) > SKILL_LIMIT * 6u + PATH_MAX * 6u + 4096u)
        return fail(out, size, "Skill request exceeds its byte bound");
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0), *old = NULL, *write = NULL;
    yyjson_val *obj = doc ? yyjson_doc_get_root(doc) : NULL;
    char *result = malloc(RECEIPT_LIMIT);
    jbuf_t b;
    jbuf_init(&b, 1024);
    bool ok = false;
    const char *message = "Require action and path, with portable single-line name/description "
                          "frontmatter for writes";
    if (!result || !yyjson_is_obj(obj))
        goto done;
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(obj, i, n, key, value) {
        const char *k = yyjson_get_str(key);
        if (strlen(k) != yyjson_get_len(key) ||
            (strcmp(k, "action") && strcmp(k, "path") && strcmp(k, "body") &&
             strcmp(k, "expected_sha256")) ||
            !yyjson_is_str(value) || strlen(yyjson_get_str(value)) != yyjson_get_len(value))
            goto done;
        size_t j, m;
        yyjson_val *other, *unused;
        yyjson_obj_foreach(obj, j, m, other,
                           unused) if (j < i && !strcmp(k, yyjson_get_str(other))) goto done;
    }
    const char *action = yyjson_get_str(yyjson_obj_get(obj, "action"));
    const char *path = yyjson_get_str(yyjson_obj_get(obj, "path"));
    const char *body = yyjson_get_str(yyjson_obj_get(obj, "body"));
    const char *expected = yyjson_get_str(yyjson_obj_get(obj, "expected_sha256"));
    if (!action || !path || !*path || strlen(path) > PATH_MAX - 100)
        goto done;
    bool create = !strcmp(action, "create"), update = !strcmp(action, "update");
    bool invoke = !strcmp(action, "invoke"), read = !strcmp(action, "inspect") || invoke;
    if ((!create && !update && !read) || (read && (body || expected)) ||
        ((create || update) && (!body || !expected)))
        goto done;
    char name[65], description[1025], previous[65] = "", history[PATH_MAX] = "";
    const char *hash = NULL;
    if (read || update) {
        old = inspect(path, result);
        if (!old) {
            message = result;
            goto done;
        }
        yyjson_val *snapshot = yyjson_doc_get_root(old);
        const char *prior_body = yyjson_get_str(yyjson_obj_get(snapshot, "content"));
        hash = yyjson_get_str(yyjson_obj_get(snapshot, "sha256"));
        if (!prior_body || !hash)
            goto done;
        if (read) {
            if (!metadata(prior_body, name, description)) {
                message = "Skill requires portable single-line name/description frontmatter; use "
                          "/skills show for legacy files";
                goto done;
            }
            body = prior_body;
        } else {
            if (strcmp(expected, hash)) {
                message = "Skill version changed; inspect before updating";
                goto done;
            }
            snprintf(previous, sizeof(previous), "%s", hash);
        }
    }
    if (create || update) {
        if (!metadata(body, name, description) || (create && strcmp(expected, "absent")))
            goto done;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (strcmp(base, "SKILL.md")) {
            message = "Create/update require a SKILL.md path in an existing skill directory";
            goto done;
        }
        /* The preview traverses the same capability/path gate and constructs
         * the escaped diff before either the history or live file is changed. */
        if (!replace(path, expected, body, true, result)) {
            message = result;
            goto done;
        }
        size_t worst = strlen(result) + 1024;
        const char *strings[] = {path, name, description, path};
        for (size_t j = 0; j < sizeof(strings) / sizeof(strings[0]); j++) {
            jbuf_t escaped;
            jbuf_init(&escaped, 64);
            jbuf_append_json_str(&escaped, strings[j]);
            worst += escaped.len;
            jbuf_free(&escaped);
        }
        if (worst >= size) {
            message = "Skill receipt exceeds the result buffer; no bytes written";
            goto done;
        }
        if (update) {
            size_t prefix = (size_t)(base - path);
            snprintf(history, sizeof(history), "%.*s.SKILL.%s.md", (int)prefix, path, previous);
            const char *prior_body =
                yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(old), "content"));
            if (!replace(history, "absent", prior_body, false, result)) {
                yyjson_doc *revision = inspect(history, result);
                const char *saved =
                    revision
                        ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(revision), "sha256"))
                        : NULL;
                bool same = saved && !strcmp(saved, previous);
                if (revision)
                    yyjson_doc_free(revision);
                if (!same) {
                    message = "Unable to retain the previous revision through the governed editor; "
                              "skill was not updated";
                    goto done;
                }
            }
        }
        if (!replace(path, expected, body, false, result)) {
            message = result;
            goto done;
        }
        write = yyjson_read(result, strlen(result), 0);
        hash = write ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(write), "after_sha256"))
                     : NULL;
        if (!hash) {
            message = "Editor committed without a parseable receipt; inspect before retrying";
            goto done;
        }
        dsco_workspace_prompt_invalidate();
    }
    jbuf_append(&b, "{\"ok\":true,\"kind\":\"skill\",\"action\":");
    jbuf_append_json_str(&b, action);
    jbuf_append(&b, ",\"name\":");
    jbuf_append_json_str(&b, name);
    jbuf_append(&b, ",\"description\":");
    jbuf_append_json_str(&b, description);
    jbuf_append(&b, ",\"path\":");
    jbuf_append_json_str(&b, path);
    jbuf_append(&b, ",\"sha256\":");
    jbuf_append_json_str(&b, hash);
    jbuf_append(&b, ",\"scope\":\"invocation\",\"activated\":false");
    if (read) {
        jbuf_append(&b, ",\"instructions\":");
        jbuf_append_json_str(&b, body);
    }
    if (*history) {
        jbuf_append(&b, ",\"previous_sha256\":");
        jbuf_append_json_str(&b, previous);
        jbuf_append(&b, ",\"previous_path\":");
        jbuf_append_json_str(&b, history);
    }
    if (write) {
        jbuf_append(&b, ",\"edit_receipt\":");
        jbuf_append(&b, result);
    }
    jbuf_append(&b, "}");
    if (b.len >= size) {
        message = "Skill receipt exceeds the result buffer; use a larger buffer";
        goto done;
    }
    memcpy(out, b.data, b.len + 1);
    ok = true;
done:
    if (!ok)
        fail(out, size, message);
    if (doc)
        yyjson_doc_free(doc);
    if (old)
        yyjson_doc_free(old);
    if (write)
        yyjson_doc_free(write);
    free(result);
    jbuf_free(&b);
    return ok;
}
