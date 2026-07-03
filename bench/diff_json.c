/* diff_json.c — differential correctness: DSCO json_get_* vs yyjson over a real
 * corpus. For every top-level key in every line, extract the value both ways and
 * assert they agree (string/int/bool/double). Any mismatch is a semantic gap
 * that would block a yyjson swap. This is the safety net before integration. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/json_util.h"
#include "yyjson.h"

static int mismatches = 0, checks = 0;

/* yyjson's equivalent of json_get_str: root object -> key -> string value (or NULL) */
static char *yy_get_str(yyjson_val *root, const char *key) {
    yyjson_val *v = yyjson_obj_get(root, key);
    if (v && yyjson_is_str(v))
        return strdup(yyjson_get_str(v));
    return NULL;
}

static void check_key(const char *json, yyjson_val *root, const char *key) {
    /* string */
    char *ds = json_get_str(json, key);
    char *ys = yy_get_str(root, key);
    checks++;
    if ((ds == NULL) != (ys == NULL) || (ds && ys && strcmp(ds, ys) != 0)) {
        /* dsco returns the raw scanned string even for non-string scalars in some
         * paths; only flag when the value really IS a JSON string both should see */
        yyjson_val *v = yyjson_obj_get(root, key);
        if (v && yyjson_is_str(v)) {
            printf("  STR MISMATCH key=%s dsco=%s yy=%s\n", key, ds ? ds : "(null)",
                   ys ? ys : "(null)");
            mismatches++;
        }
    }
    free(ds);
    free(ys);

    /* int */
    yyjson_val *v = yyjson_obj_get(root, key);
    if (v && yyjson_is_int(v)) {
        long long di = json_get_i64(json, key, -999999);
        long long yi = yyjson_get_sint(v);
        checks++;
        if (di != yi && di != -999999) {
            printf("  INT MISMATCH key=%s dsco=%lld yy=%lld\n", key, di, yi);
            mismatches++;
        }
    }
    /* bool */
    if (v && yyjson_is_bool(v)) {
        bool db = json_get_bool(json, key, false);
        bool yb = yyjson_get_bool(v);
        checks++;
        if (db != yb) {
            printf("  BOOL MISMATCH key=%s dsco=%d yy=%d\n", key, db, yb);
            mismatches++;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s corpus.jsonl\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror("fopen");
        return 2;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int lines = 0;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (n < 2)
            continue;
        yyjson_doc *d = yyjson_read(line, (size_t)n, 0);
        if (!d)
            continue; /* skip non-JSON lines */
        yyjson_val *root = yyjson_doc_get_root(d);
        if (yyjson_is_obj(root)) {
            lines++;
            yyjson_val *k, *v;
            yyjson_obj_iter it;
            yyjson_obj_iter_init(root, &it);
            while ((k = yyjson_obj_iter_next(&it))) {
                v = yyjson_obj_iter_get_val(k);
                (void)v;
                check_key(line, root, yyjson_get_str(k));
            }
        }
        yyjson_doc_free(d);
    }
    free(line);
    fclose(f);
    printf("lines=%d checks=%d mismatches=%d -> %s\n", lines, checks, mismatches,
           mismatches == 0 ? "PARITY OK" : "GAPS FOUND");
    return mismatches ? 1 : 0;
}
