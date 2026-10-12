#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#include "code_edit.h"
#include "skill_lifecycle.h"
#include "json_util.h"
#include "crypto.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks, nested_calls;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
static char output[196608];
static const char *deny_path;
const char *tools_execution_tier(void) {
    return "untrusted";
}
void dsco_workspace_prompt_invalidate(void) {}
bool tools_execute_for_tier(const char *tool, const char *input, const char *tier, char *out,
                            size_t size) {
    CHECK(!strcmp(tier, "untrusted"));
    nested_calls++;
    if (deny_path && strstr(input, deny_path)) {
        snprintf(out, size, "{\"error\":\"fixture path denial\"}");
        return false;
    }
    if (!strcmp(tool, "code_read"))
        return code_read_execute(input, out, size);
    CHECK(!strcmp(tool, "code_edit"));
    return code_edit_execute(input, out, size);
}
static void put(const char *path, const char *text, size_t len) {
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite(text, 1, len, f) == len);
    CHECK(!fclose(f));
}
static void equals(const char *path, const char *text) {
    char buf[4096];
    FILE *f = fopen(path, "rb");
    CHECK(f);
    size_t n = fread(buf, 1, sizeof(buf), f);
    CHECK(!fclose(f));
    CHECK(n == strlen(text) && !memcmp(buf, text, n));
}
static yyjson_doc *parse(void) {
    yyjson_doc *doc = yyjson_read(output, strlen(output), 0);
    CHECK(doc);
    return doc;
}
static void read_refs(const char *path, char hash[65], char anchors[][40], size_t expected_lines) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"path\":");
    jbuf_append_json_str(&b, path);
    jbuf_append(&b, "}");
    CHECK(code_read_execute(b.data, output, sizeof(output)));
    jbuf_free(&b);
    yyjson_doc *doc = parse();
    yyjson_val *root = yyjson_doc_get_root(doc);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "total_lines")) == expected_lines);
    snprintf(hash, 65, "%s", yyjson_get_str(yyjson_obj_get(root, "sha256")));
    yyjson_val *lines = yyjson_obj_get(root, "lines");
    for (size_t i = 0; i < expected_lines; i++)
        snprintf(anchors[i], 40, "%s",
                 yyjson_get_str(yyjson_obj_get(yyjson_arr_get(lines, i), "anchor")));
    yyjson_doc_free(doc);
}
static bool edit(const char *path, const char *hash, const char *ops, bool dry) {
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"path\":");
    jbuf_append_json_str(&b, path);
    jbuf_appendf(&b, ",\"expected_sha256\":\"%s\",\"edits\":%s,\"dry_run\":%s}", hash, ops,
                 dry ? "true" : "false");
    bool ok = code_edit_execute(b.data, output, sizeof(output));
    jbuf_free(&b);
    return ok;
}
static bool skill(const char *path, const char *action, const char *hash, const char *body,
                  size_t cap) {
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_appendf(&b, "{\"action\":\"%s\",\"path\":", action);
    jbuf_append_json_str(&b, path);
    if (hash) {
        jbuf_append(&b, ",\"expected_sha256\":");
        jbuf_append_json_str(&b, hash);
    }
    if (body) {
        jbuf_append(&b, ",\"body\":");
        jbuf_append_json_str(&b, body);
    }
    jbuf_append(&b, "}");
    bool ok = skill_lifecycle_execute(b.data, output, cap);
    jbuf_free(&b);
    return ok;
}
int main(void) {
    char root[] = "/tmp/dsco-code-edit.XXXXXX";
    CHECK(mkdtemp(root));
    char path[1024], other[1024], hash[65], anchors[8][40], ops[4096];
    snprintf(path, sizeof(path), "%s/source.c", root);
    snprintf(other, sizeof(other), "%s/other.c", root);
    const char *before = "α\r\nbeta\r\nlast";
    put(path, before, strlen(before));
    CHECK(!chmod(path, 0751));
    read_refs(path, hash, anchors, 3);
    snprintf(ops, sizeof(ops),
             "[{\"kind\":\"replace\",\"start\":\"%s\",\"new_text\":\"NEW\\r\\n\"},{\"kind\":"
             "\"insert_after\",\"start\":\"%s\",\"new_text\":\"!\"}]",
             anchors[1], anchors[2]);
    CHECK(edit(path, hash, ops, true));
    CHECK(strstr(output, "preview"));
    equals(path, before);
    CHECK(edit(path, hash, ops, false));
    CHECK(strstr(output, "\"verified\":true"));
    equals(path, "α\r\nNEW\r\nlast!");
    struct stat st;
    CHECK(!stat(path, &st));
    CHECK((st.st_mode & 0777) == 0751);
    CHECK(!edit(path, hash, ops, false));
    CHECK(strstr(output, "stale_snapshot"));
    read_refs(path, hash, anchors, 3);
    snprintf(ops, sizeof(ops),
             "[{\"kind\":\"delete\",\"start\":\"%s\"},{\"kind\":\"replace\",\"start\":\"%s\",\"new_"
             "text\":\"x\"}]",
             anchors[0], anchors[0]);
    CHECK(!edit(path, hash, ops, false));
    equals(path, "α\r\nNEW\r\nlast!");
    snprintf(ops, sizeof(ops), "[{\"kind\":\"delete\",\"start\":\"2:0000000000000000\"}]");
    CHECK(!edit(path, hash, ops, false));
    snprintf(ops, sizeof(ops), "[{\"kind\":\"delete\",\"start\":\"%s\"}]", anchors[2]);
    CHECK(edit(path, hash, ops, false));
    equals(path, "α\r\nNEW\r\n");
    char input[4096];
    snprintf(input, sizeof(input),
             "{\"path\":\"%s\",\"expected_sha256\":\"absent\",\"content\":\"\"}", other);
    CHECK(!code_edit_execute(input, output, 100));
    CHECK(access(other, F_OK) != 0);
    CHECK(code_edit_execute(input, output, sizeof(output)));
    equals(other, "");
    CHECK(!code_edit_execute(input, output, sizeof(output)));
    CHECK(strstr(output, "stale_snapshot"));
    read_refs(other, hash, anchors, 0);
    CHECK(!code_read_execute("{\"path\":\"bad\",\"path\":\"duplicate\"}", output, sizeof(output)));
    CHECK(!code_read_execute("{\"path\":\"x\\u0000hidden\"}", output, sizeof(output)));
    CHECK(!code_read_execute("{\"path\":\"x\",\"limit\":1.5}", output, sizeof(output)));
    put(other, "a\0b", 3);
    snprintf(input, sizeof(input), "{\"path\":\"%s\"}", other);
    CHECK(!code_read_execute(input, output, sizeof(output)));
    put(other, "\xff", 1);
    CHECK(!code_read_execute(input, output, sizeof(output)));
    CHECK(!unlink(other));
    CHECK(!symlink(path, other));
    CHECK(!code_read_execute(input, output, sizeof(output)));
    CHECK(!unlink(other));
    CHECK(!link(path, other));
    CHECK(!code_read_execute(input, output, sizeof(output)));
    CHECK(!unlink(other));
    int lock_fd = open(root, O_RDONLY | O_DIRECTORY);
    CHECK(lock_fd >= 0);
    CHECK(!flock(lock_fd, LOCK_EX));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(lock_fd);
        read_refs(path, hash, anchors, 2);
        snprintf(ops, sizeof(ops), "[{\"kind\":\"delete\",\"start\":\"%s\"}]", anchors[0]);
        CHECK(!edit(path, hash, ops, false));
        CHECK(strstr(output, "busy"));
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!flock(lock_fd, LOCK_UN));
    close(lock_fd);
    snprintf(path, sizeof(path), "%s/SKILL.md", root);
    const char *v1 = "---\nname: fixture\ndescription: Check precise edits.\n---\n# First\n";
    const char *v2 = "---\nname: fixture\ndescription: Check precise edits.\n---\n# Second α\n";
    CHECK(!skill(path, "create", "absent", v1, 100));
    CHECK(access(path, F_OK) != 0);
    CHECK(skill(path, "create", "absent", v1, sizeof(output)));
    CHECK(strstr(output, "\"activated\":false"));
    CHECK(skill(path, "invoke", NULL, NULL, sizeof(output)));
    CHECK(strstr(output, "# First"));
    yyjson_doc *doc = parse();
    snprintf(hash, sizeof(hash), "%s",
             yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "sha256")));
    yyjson_doc_free(doc);
    deny_path = ".SKILL.";
    CHECK(!skill(path, "update", hash, v2, sizeof(output)));
    equals(path, v1);
    deny_path = NULL;
    CHECK(!skill(path, "update", hash, v2, 100));
    equals(path, v1);
    CHECK(skill(path, "update", hash, v2, sizeof(output)));
    equals(path, v2);
    doc = parse();
    snprintf(other, sizeof(other), "%s",
             yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "previous_path")));
    yyjson_doc_free(doc);
    equals(other, v1);
    CHECK(skill(other, "invoke", NULL, NULL, sizeof(output)));
    CHECK(strstr(output, "# First"));
    CHECK(!skill(path, "update", hash, v1, sizeof(output)));
    equals(path, v2);
    CHECK(skill(path, "inspect", NULL, NULL, sizeof(output)));
    doc = parse();
    snprintf(hash, sizeof(hash), "%s",
             yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "sha256")));
    yyjson_doc_free(doc);
    CHECK(skill(path, "update", hash, v1, sizeof(output)));
    equals(path, v1);
    CHECK(!skill(path, "update", hash, "# not portable\n", sizeof(output)));
    CHECK(nested_calls > 15);
    printf("PASS: %u checks; exact anchors/CAS, CRLF/Unicode, preflight, metadata, binary/link "
           "rejection, lock contention, governed skill history/rollback\n",
           checks);
    return 0;
}
