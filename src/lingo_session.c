#include "lingo_session.h"
#include "crypto.h"
#include "json_util.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define SESSION_LIMIT 4
#define FILE_LIMIT (256u * 1024u)
#define RESPONSE_LIMIT (512u * 1024u)

const char lingo_session_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"open\",\"read\",\"why\",\"controls\","
    "\"set\",\"reset\",\"select\",\"inspect\",\"save\",\"close\"]},"
    "\"session_id\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},"
    "\"args\":{\"type\":\"object\"},\"restore_path\":{\"type\":\"string\"},"
    "\"control\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16},"
    "\"value\":{},\"object\":{\"type\":\"string\"},\"field\":{\"type\":\"string\"}},"
    "\"required\":[\"action\"],\"additionalProperties\":false}";

typedef struct {
    char id[37], source_hash[65];
    char *path, *arguments;
    lingo_vm *vm;
} session;
static session sessions[SESSION_LIMIT];
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool dispatching;
static bool cleanup_registered;

static bool error_result(char *out, size_t cap, const char *error) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"error\":");
    jbuf_append_json_str(&b, error);
    jbuf_append(&b, "}");
    if (b.len < cap)
        memcpy(out, b.data, b.len + 1);
    else if (cap)
        snprintf(out, cap, "{}");
    jbuf_free(&b);
    return false;
}
static void release(session *s) {
    lingo_vm_close(s->vm);
    free(s->path);
    free(s->arguments);
    memset(s, 0, sizeof(*s));
}
static void cleanup(void) {
    for (size_t i = 0; i < SESSION_LIMIT; i++)
        release(&sessions[i]);
}
static const char *string(yyjson_val *v) {
    const char *s = yyjson_get_str(v);
    return s && *s && strlen(s) == yyjson_get_len(v) ? s : NULL;
}
static bool fields(yyjson_val *value, const char *allowed) {
    if (!yyjson_is_obj(value))
        return false;
    size_t i, n;
    yyjson_val *key, *item;
    yyjson_obj_foreach(value, i, n, key, item) {
        const char *s = string(key);
        if (!s || yyjson_obj_get(value, s) != item)
            return false;
        char match[96];
        if (strlen(s) > 80)
            return false;
        snprintf(match, sizeof(match), "|%s|", s);
        if (!strstr(allowed, match))
            return false;
    }
    return true;
}
static char *read_file(const char *path) {
    if (!path || strlen(path) > 4096)
        return NULL;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (unsigned long long)st.st_size > FILE_LIMIT) {
        close(fd);
        return NULL;
    }
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        close(fd);
        return NULL;
    }
    char *text = malloc(FILE_LIMIT + 1);
    if (!text) {
        fclose(f);
        return NULL;
    }
    size_t size = fread(text, 1, FILE_LIMIT + 1, f);
    bool bad = ferror(f) || size > FILE_LIMIT || memchr(text, 0, size);
    fclose(f);
    if (bad) {
        free(text);
        return NULL;
    }
    text[size] = 0;
    return text;
}
static bool envelope(session *s, const char *value, unsigned before, char *out, size_t cap) {
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_appendf(&b,
                 "{\"session_id\":\"%s\",\"source_sha256\":\"%s\","
                 "\"tool_calls\":%u,\"command_tool_calls\":%u,\"view\":",
                 s->id, s->source_hash, lingo_vm_calls(s->vm), lingo_vm_calls(s->vm) - before);
    jbuf_append(&b, value);
    jbuf_append(&b, "}");
    bool ok = b.len < cap;
    if (ok)
        memcpy(out, b.data, b.len + 1);
    else
        error_result(out, cap, "session response exceeds caller capacity");
    jbuf_free(&b);
    return ok;
}
static bool open_session(yyjson_val *root, char *out, size_t cap) {
    if (!fields(root, "|action||path||args||restore_path|"))
        return error_result(out, cap, "open accepts path, optional args and restore_path");
    const char *path = string(yyjson_obj_get(root, "path"));
    yyjson_val *args = yyjson_obj_get(root, "args");
    yyjson_val *restore_arg = yyjson_obj_get(root, "restore_path");
    const char *restore_path = string(restore_arg);
    if (!path || (args && !yyjson_is_obj(args)) || (restore_arg && !restore_path))
        return error_result(out, cap, "open requires a source path and valid argument records");
    session *s = NULL;
    for (size_t i = 0; i < SESSION_LIMIT; i++)
        if (!sessions[i].vm) {
            s = &sessions[i];
            break;
        }
    if (!s)
        return error_result(out, cap, "four Lingo sessions are already open; close one first");
    char *source = read_file(path), *saved = restore_path ? read_file(restore_path) : NULL;
    if (!source || (restore_path && !saved)) {
        free(source);
        free(saved);
        return error_result(out, cap, "cannot read bounded regular program or session file");
    }
    char hash[65];
    sha256_hex((const unsigned char *)source, strlen(source), hash);
    yyjson_doc *saved_doc = saved ? yyjson_read(saved, strlen(saved), 0) : NULL;
    yyjson_val *saved_root = saved_doc ? yyjson_doc_get_root(saved_doc) : NULL;
    yyjson_val *restored_view = NULL;
    bool ok = false;
    if (restore_path) {
        char view_hash[65];
        lingo_vm_view_hash(view_hash);
        if (!fields(saved_root, "|format||source_sha256||source_path||args||view_sha256||view|") ||
            !yyjson_equals_str(yyjson_obj_get(saved_root, "format"), "lingo.session/1") ||
            !yyjson_equals_str(yyjson_obj_get(saved_root, "source_sha256"), hash) ||
            !yyjson_equals_str(yyjson_obj_get(saved_root, "view_sha256"), view_hash) ||
            !yyjson_is_obj(yyjson_obj_get(saved_root, "args")) ||
            !yyjson_is_obj(yyjson_obj_get(saved_root, "view"))) {
            error_result(out, cap, "session format or program source identity mismatch");
            goto done;
        }
        yyjson_val *saved_args = yyjson_obj_get(saved_root, "args");
        if (args && !yyjson_equals(args, saved_args)) {
            error_result(out, cap, "restoring requires the saved program arguments");
            goto done;
        }
        args = saved_args;
        restored_view = yyjson_obj_get(saved_root, "view");
    }
    char *arguments = args ? yyjson_val_write(args, 0, NULL) : strdup("{}");
    char *restore = restored_view ? yyjson_val_write(restored_view, 0, NULL) : NULL;
    if (!arguments || (restored_view && !restore)) {
        free(arguments);
        free(restore);
        error_result(out, cap, "session allocation failed");
        goto done;
    }
    jbuf_t request;
    jbuf_init(&request, 1024);
    jbuf_append(&request, "{\"source\":");
    jbuf_append_json_str(&request, source);
    jbuf_append(&request, ",\"args\":");
    jbuf_append(&request, arguments);
    jbuf_append(&request, "}");
    char *scratch = calloc(1, RESPONSE_LIMIT);
    if (!scratch)
        error_result(out, cap, "session allocation failed");
    else if (lingo_vm_open(request.data, restore, &s->vm, scratch, RESPONSE_LIMIT)) {
        s->arguments = arguments;
        arguments = NULL;
        s->path = strdup(path);
        uuid_v4(s->id);
        memcpy(s->source_hash, hash, sizeof(hash));
        if (!s->path)
            error_result(out, cap, "session allocation failed");
        else if (lingo_vm_command(s->vm, "{\"action\":\"read\"}", scratch, cap - 512))
            ok = envelope(s, scratch, 0, out, cap);
        else
            error_result(out, cap, scratch);
        if (!ok)
            release(s);
    } else
        error_result(out, cap, scratch);
    free(scratch);
    jbuf_free(&request);
    free(arguments);
    free(restore);
done:
    yyjson_doc_free(saved_doc);
    free(saved);
    free(source);
    return ok;
}
static bool save_session(session *s, const char *path, char *out, size_t cap) {
    char *packet = calloc(1, RESPONSE_LIMIT), *receipt = calloc(1, RESPONSE_LIMIT);
    if (!packet || !receipt) {
        free(packet);
        free(receipt);
        return error_result(out, cap, "session allocation failed");
    }
    bool ok = false;
    unsigned before = lingo_vm_calls(s->vm);
    if (!lingo_vm_command(s->vm, "{\"action\":\"snapshot\"}", packet, RESPONSE_LIMIT)) {
        error_result(out, cap, packet);
        goto done;
    }
    jbuf_t artifact;
    jbuf_init(&artifact, 1024);
    char view_hash[65];
    lingo_vm_view_hash(view_hash);
    jbuf_appendf(&artifact,
                 "{\"format\":\"lingo.session/"
                 "1\",\"source_sha256\":\"%s\",\"view_sha256\":\"%s\",\"source_path\":",
                 s->source_hash, view_hash);
    jbuf_append_json_str(&artifact, s->path);
    jbuf_append(&artifact, ",\"args\":");
    jbuf_append(&artifact, s->arguments);
    jbuf_append(&artifact, ",\"view\":");
    jbuf_append(&artifact, packet);
    jbuf_append(&artifact, "}\n");
    if (artifact.len > FILE_LIMIT) {
        error_result(out, cap, "saved session exceeds 256 KiB");
        jbuf_free(&artifact);
        goto done;
    }
    jbuf_t request;
    jbuf_init(&request, 1024);
    jbuf_append(&request, "{\"path\":");
    jbuf_append_json_str(&request, path);
    jbuf_append(&request, ",\"content\":");
    jbuf_append_json_str(&request, artifact.data);
    jbuf_append(&request, "}");
    char hash[65];
    sha256_hex((const unsigned char *)artifact.data, artifact.len, hash);
    jbuf_t result;
    jbuf_init(&result, 256);
    jbuf_append(&result, "{\"saved\":true,\"path\":");
    jbuf_append_json_str(&result, path);
    jbuf_appendf(&result, ",\"sha256\":\"%s\",\"bytes\":%zu}", hash, artifact.len);
    if (result.len > cap - 512) {
        error_result(out, cap, "save response exceeds caller capacity");
        jbuf_free(&result);
        jbuf_free(&request);
        jbuf_free(&artifact);
        goto done;
    }
    lingo_vm_record_call(s->vm);
    if (!tools_execute_for_tier("write_file", request.data, tools_execution_tier(), receipt,
                                RESPONSE_LIMIT))
        error_result(out, cap, receipt);
    else {
        ok = envelope(s, result.data, before, out, cap);
    }
    jbuf_free(&result);
    jbuf_free(&request);
    jbuf_free(&artifact);
done:
    free(packet);
    free(receipt);
    return ok;
}
static bool dispatch(yyjson_val *root, char *out, size_t cap) {
    const char *action = string(yyjson_obj_get(root, "action"));
    if (!action)
        return error_result(out, cap, "session action is required");
    if (!strcmp(action, "open"))
        return open_session(root, out, cap);
    const char *id = string(yyjson_obj_get(root, "session_id"));
    session *s = NULL;
    for (size_t i = 0; i < SESSION_LIMIT; i++)
        if (id && sessions[i].vm && !strcmp(sessions[i].id, id)) {
            s = &sessions[i];
            break;
        }
    if (!s)
        return error_result(out, cap, "unknown session_id in this DSCO process");
    const char *allowed = "|action||session_id|";
    if (!strcmp(action, "set"))
        allowed = "|action||session_id||control||value|";
    else if (!strcmp(action, "select"))
        allowed = "|action||session_id||object||field||args|";
    else if (!strcmp(action, "inspect"))
        allowed = "|action||session_id||object|";
    else if (!strcmp(action, "save"))
        allowed = "|action||session_id||path|";
    if (!fields(root, allowed))
        return error_result(out, cap, "unexpected or duplicate session command fields");
    unsigned before = lingo_vm_calls(s->vm);
    if (!strcmp(action, "close")) {
        bool ok = envelope(s, "{\"closed\":true}", before, out, cap);
        release(s);
        return ok;
    }
    if (!strcmp(action, "save")) {
        const char *path = string(yyjson_obj_get(root, "path"));
        if (!path || strlen(path) > 4096)
            return error_result(out, cap, "save requires a bounded path");
        return save_session(s, path, out, cap);
    }
    if (strcmp(action, "read") && strcmp(action, "why") && strcmp(action, "controls") &&
        strcmp(action, "set") && strcmp(action, "reset") && strcmp(action, "select") &&
        strcmp(action, "inspect"))
        return error_result(out, cap, "unsupported session action");
    /* Values are serialized into an independent object: no session routing
     * metadata reaches the Lua semantic dispatcher. */
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{");
    size_t i, n;
    yyjson_val *key, *value;
    bool first = true;
    yyjson_obj_foreach(root, i, n, key, value) {
        if (yyjson_equals_str(key, "session_id"))
            continue;
        char *encoded = yyjson_val_write(value, 0, NULL);
        if (!encoded) {
            jbuf_free(&b);
            return error_result(out, cap, "command allocation failed");
        }
        if (!first)
            jbuf_append(&b, ",");
        first = false;
        jbuf_append_json_str(&b, yyjson_get_str(key));
        jbuf_append(&b, ":");
        jbuf_append(&b, encoded);
        free(encoded);
    }
    jbuf_append(&b, "}");
    char *value_json = calloc(1, RESPONSE_LIMIT);
    bool ok = false;
    if (!value_json)
        error_result(out, cap, "command allocation failed");
    else if (lingo_vm_command(s->vm, b.data, value_json, cap - 512))
        ok = envelope(s, value_json, before, out, cap);
    else
        error_result(out, cap, value_json);
    free(value_json);
    jbuf_free(&b);
    return ok;
}
bool lingo_session_execute(const char *input, char *out, size_t cap) {
    if (cap < 1024)
        return error_result(out, cap, "session output capacity must be at least 1024 bytes");
    if (cap > RESPONSE_LIMIT)
        cap = RESPONSE_LIMIT;
    if (dispatching)
        return error_result(out, cap, "recursive Lingo sessions are disabled");
    if (!input || strlen(input) > RESPONSE_LIMIT)
        return error_result(out, cap, "session request exceeds 512 KiB");
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!fields(root,
                "|action||session_id||path||args||restore_path||control||value||object||field|")) {
        yyjson_doc_free(doc);
        return error_result(out, cap, "invalid session request fields");
    }
    dispatching = true;
    pthread_mutex_lock(&session_mutex);
    if (!cleanup_registered) {
        atexit(cleanup);
        cleanup_registered = true;
    }
    bool ok = dispatch(root, out, cap);
    pthread_mutex_unlock(&session_mutex);
    dispatching = false;
    yyjson_doc_free(doc);
    return ok;
}
