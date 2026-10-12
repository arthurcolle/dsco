/* Offline eval suites: `dsco eval run SUITE.json --out DIR`.
 * Each task is graded by one deterministic rule. Paths resolve against the
 * current directory. DIR/results.json is written atomically. */
#include "eval_suite.h"
#include "../vendor/yyjson.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define ES_SUITE_LIMIT (1024 * 1024)
#define ES_FILE_LIMIT (4 * 1024 * 1024)
#define ES_MAX_TASKS 256
#define ES_ID_MAX 128
#define ES_PATH_MAX 1024
#define ES_NEEDLE_MAX 4096
#define ES_DETAIL_MAX 256

typedef enum { ES_RULE_FILE_EXISTS, ES_RULE_FILE_CONTAINS } es_rule_t;

typedef struct {
    const char *id;
    const char *path;
    const char *needle;
    es_rule_t rule;
} es_task_t;

typedef struct {
    bool pass;
    char detail[ES_DETAIL_MAX];
} es_result_t;

typedef struct {
    const char *suite_path;
    const char *suite_name;
    const es_task_t *tasks;
    const es_result_t *results;
    size_t count;
    size_t passed;
} es_run_t;

static void es_usage(FILE *out) {
    fputs(
        "Usage: dsco eval run SUITE.json --out DIR\n"
        "  Grades each task and writes DIR/results.json.\n"
        "  Rules: file_exists {id,path} | file_contains {id,path,needle}.\n"
        "  Paths resolve against the current directory.\n"
        "  Exit 0: suite ran and results were written (grades are in results.json).\n"
        "  Exit 2: invalid usage or invalid suite. Exit 1: I/O failure.\n",
        out);
}

static char *es_read_file(const char *path, size_t limit, size_t *len_out, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s", strerror(errno));
        return NULL;
    }
    char *buf = malloc(limit + 1);
    if (!buf) {
        fclose(f);
        snprintf(err, errlen, "%s", "allocation failed");
        return NULL;
    }
    size_t n = fread(buf, 1, limit + 1, f);
    bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) {
        free(buf);
        snprintf(err, errlen, "%s", "read failed");
        return NULL;
    }
    if (n > limit) {
        free(buf);
        snprintf(err, errlen, "larger than %zu bytes", limit);
        return NULL;
    }
    buf[n] = '\0';
    *len_out = n;
    return buf;
}

static bool es_contains(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return true;
    if (nlen > hlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

static es_result_t es_grade(const es_task_t *t) {
    es_result_t r = {0};
    if (t->rule == ES_RULE_FILE_EXISTS) {
        struct stat st;
        if (stat(t->path, &st) == 0) {
            r.pass = true;
            snprintf(r.detail, sizeof r.detail, "%s", "path exists");
        } else {
            snprintf(r.detail, sizeof r.detail, "stat failed: %s", strerror(errno));
        }
        return r;
    }
    char err[128] = {0};
    size_t n = 0;
    char *buf = es_read_file(t->path, ES_FILE_LIMIT, &n, err, sizeof err);
    if (!buf) {
        snprintf(r.detail, sizeof r.detail, "read failed: %s", err);
        return r;
    }
    r.pass = es_contains(buf, n, t->needle);
    free(buf);
    snprintf(r.detail, sizeof r.detail, "%s", r.pass ? "needle found" : "needle not found");
    return r;
}

static bool es_fail(char *err, size_t errlen, size_t index, const char *msg) {
    snprintf(err, errlen, "task %zu: %s", index, msg);
    return false;
}

static const char *es_field(yyjson_val *obj, const char *key, size_t max) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!v || !yyjson_is_str(v))
        return NULL;
    size_t len = yyjson_get_len(v);
    const char *s = yyjson_get_str(v);
    if (len == 0 || len > max || strlen(s) != len)
        return NULL;
    return s;
}

static bool es_parse_task(yyjson_val *item, size_t index, es_task_t *t, char *err, size_t errlen) {
    if (!yyjson_is_obj(item))
        return es_fail(err, errlen, index, "must be an object");
    t->id = es_field(item, "id", ES_ID_MAX);
    if (!t->id)
        return es_fail(err, errlen, index, "id must be a non-empty string");
    t->path = es_field(item, "path", ES_PATH_MAX);
    if (!t->path)
        return es_fail(err, errlen, index, "path must be a non-empty string");
    const char *kind = es_field(item, "kind", 32);
    if (kind && strcmp(kind, "file_exists") == 0) {
        t->rule = ES_RULE_FILE_EXISTS;
        t->needle = NULL;
        return true;
    }
    if (kind && strcmp(kind, "file_contains") == 0) {
        t->rule = ES_RULE_FILE_CONTAINS;
        t->needle = es_field(item, "needle", ES_NEEDLE_MAX);
        if (!t->needle)
            return es_fail(err, errlen, index, "needle must be a non-empty string");
        return true;
    }
    return es_fail(err, errlen, index, "kind must be file_exists or file_contains");
}

static bool es_load_suite(const char *path, yyjson_doc **doc_out, es_task_t *tasks, size_t *count_out,
                          const char **name_out, char *err, size_t errlen) {
    char read_err[128] = {0};
    size_t n = 0;
    char *text = es_read_file(path, ES_SUITE_LIMIT, &n, read_err, sizeof read_err);
    if (!text) {
        snprintf(err, errlen, "%s", read_err);
        return false;
    }
    yyjson_doc *doc = yyjson_read(text, n, 0);
    free(text);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "%s", "not valid JSON");
        return false;
    }
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "%s", "root must be a JSON object");
        return false;
    }
    yyjson_val *name = yyjson_obj_get(root, "name");
    if (name && !yyjson_is_str(name)) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "%s", "name must be a string");
        return false;
    }
    yyjson_val *arr = yyjson_obj_get(root, "tasks");
    if (!arr || !yyjson_is_arr(arr)) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "%s", "tasks must be an array");
        return false;
    }
    size_t total = yyjson_arr_size(arr);
    if (total == 0 || total > ES_MAX_TASKS) {
        yyjson_doc_free(doc);
        snprintf(err, errlen, "tasks must hold 1..%d entries", ES_MAX_TASKS);
        return false;
    }
    size_t i = 0;
    yyjson_arr_iter iter = yyjson_arr_iter_with(arr);
    yyjson_val *item;
    while ((item = yyjson_arr_iter_next(&iter))) {
        if (!es_parse_task(item, i, &tasks[i], err, errlen)) {
            yyjson_doc_free(doc);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(tasks[j].id, tasks[i].id) == 0) {
                es_fail(err, errlen, i, "duplicate id");
                yyjson_doc_free(doc);
                return false;
            }
        }
        i++;
    }
    *doc_out = doc;
    *count_out = i;
    *name_out = name ? yyjson_get_str(name) : NULL;
    return true;
}

static char *es_render(const es_run_t *run, size_t *len_out) {
    yyjson_mut_doc *w = yyjson_mut_doc_new(NULL);
    if (!w)
        return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(w);
    yyjson_mut_doc_set_root(w, root);
    yyjson_mut_obj_add_str(w, root, "schema", "dsco.eval.results.v1");
    yyjson_mut_obj_add_strcpy(w, root, "suite", run->suite_name);
    yyjson_mut_obj_add_strcpy(w, root, "suite_path", run->suite_path);
    yyjson_mut_obj_add_int(w, root, "generated_at_unix", (int64_t)time(NULL));
    yyjson_mut_obj_add_int(w, root, "total", (int64_t)run->count);
    yyjson_mut_obj_add_int(w, root, "passed", (int64_t)run->passed);
    yyjson_mut_obj_add_int(w, root, "failed", (int64_t)(run->count - run->passed));
    yyjson_mut_val *list = yyjson_mut_arr(w);
    yyjson_mut_obj_add_val(w, root, "results", list);
    for (size_t i = 0; i < run->count; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(w);
        yyjson_mut_obj_add_strcpy(w, o, "id", run->tasks[i].id);
        yyjson_mut_obj_add_str(w, o, "kind",
                               run->tasks[i].rule == ES_RULE_FILE_EXISTS ? "file_exists" : "file_contains");
        yyjson_mut_obj_add_strcpy(w, o, "path", run->tasks[i].path);
        yyjson_mut_obj_add_str(w, o, "status", run->results[i].pass ? "pass" : "fail");
        yyjson_mut_obj_add_strcpy(w, o, "detail", run->results[i].detail);
        yyjson_mut_arr_add_val(list, o);
    }
    char *json = yyjson_mut_write(w, YYJSON_WRITE_PRETTY, len_out);
    yyjson_mut_doc_free(w);
    return json;
}

static bool es_mkdirs(const char *dir) {
    char buf[1024];
    if (!*dir || strlen(dir) >= sizeof buf)
        return false;
    snprintf(buf, sizeof buf, "%s", dir);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return false;
    struct stat st;
    return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool es_write_atomic(const char *dir, const char *data, size_t len, char *final_path, size_t cap) {
    char tmp[ES_PATH_MAX + 64];
    int a = snprintf(final_path, cap, "%s/results.json", dir);
    int b = snprintf(tmp, sizeof tmp, "%s/results.json.tmp", dir);
    if (a < 0 || (size_t)a >= cap || b < 0 || (size_t)b >= sizeof tmp)
        return false;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool wrote = fwrite(data, 1, len, f) == len;
    bool closed = fclose(f) == 0;
    bool ok = wrote && closed && rename(tmp, final_path) == 0;
    if (!ok)
        remove(tmp);
    return ok;
}

int eval_suite_cli(int argc, char **argv) {
    if (argc < 3) {
        es_usage(stderr);
        return 2;
    }
    if (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "help") == 0) {
        es_usage(stdout);
        return 0;
    }
    if (strcmp(argv[2], "run") != 0) {
        fprintf(stderr, "dsco eval: unknown action '%s'\n", argv[2]);
        es_usage(stderr);
        return 2;
    }
    const char *suite_path = NULL, *out_dir = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc && !out_dir)
            out_dir = argv[++i];
        else if (argv[i][0] != '-' && !suite_path)
            suite_path = argv[i];
        else {
            fprintf(stderr, "dsco eval: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!suite_path || !out_dir || !*out_dir || strlen(out_dir) >= ES_PATH_MAX) {
        fprintf(stderr, "dsco eval: run needs SUITE.json and --out DIR\n");
        es_usage(stderr);
        return 2;
    }

    es_task_t tasks[ES_MAX_TASKS];
    yyjson_doc *doc = NULL;
    const char *suite_name = NULL;
    size_t count = 0;
    char err[256] = {0};
    if (!es_load_suite(suite_path, &doc, tasks, &count, &suite_name, err, sizeof err)) {
        fprintf(stderr, "dsco eval: invalid suite %s: %s\n", suite_path, err);
        return 2;
    }
    es_result_t *results = calloc(count, sizeof *results);
    if (!results) {
        yyjson_doc_free(doc);
        fprintf(stderr, "dsco eval: allocation failed\n");
        return 1;
    }
    size_t passed = 0;
    for (size_t i = 0; i < count; i++) {
        results[i] = es_grade(&tasks[i]);
        if (results[i].pass)
            passed++;
        printf("%s %s: %s\n", results[i].pass ? "PASS" : "FAIL", tasks[i].id, results[i].detail);
    }

    es_run_t run = {suite_path, suite_name ? suite_name : suite_path, tasks, results, count, passed};
    size_t json_len = 0;
    char *json = es_render(&run, &json_len);
    char final_path[ES_PATH_MAX + 64];
    int rc = 0;
    if (!json || !es_mkdirs(out_dir) || !es_write_atomic(out_dir, json, json_len, final_path, sizeof final_path)) {
        fprintf(stderr, "dsco eval: could not write results to %s\n", out_dir);
        rc = 1;
    } else {
        printf("eval: suite=%s total=%zu passed=%zu failed=%zu results=%s\n", run.suite_name, count, passed,
               count - passed, final_path);
    }
    free(json);
    free(results);
    yyjson_doc_free(doc);
    return rc;
}
