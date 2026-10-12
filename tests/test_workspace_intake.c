/* Standalone: cc -std=c11 -Wall -Wextra -Werror -Iinclude
 * tests/test_workspace_intake.c src/workspace_intake.c src/json_fast.c
 * -o /tmp/test_workspace_intake && /tmp/test_workspace_intake
 * Add -g -fsanitize=address,undefined -fno-omit-frame-pointer for sanitizers.
 */
#include "workspace_intake.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char output[WORKSPACE_INTAKE_MAX_OUTPUT + 1];
static unsigned cases;
static yyjson_val *get(yyjson_val *v, const char *k) { return yyjson_obj_get(v, k); }
static uint64_t num(yyjson_val *v, const char *k) { return yyjson_get_uint(get(v, k)); }
static const char *str(yyjson_val *v, const char *k) { return yyjson_get_str(get(v, k)); }
static bool eq(yyjson_val *v, const char *k, const char *s) {
    const char *p = str(v, k); return p && !strcmp(p, s);
}
static yyjson_val *find(yyjson_val *a, const char *k, const char *s) {
    size_t i, n; yyjson_val *v;
    yyjson_arr_foreach(a, i, n, v) { if (eq(v, k, s)) return v; }
    return NULL;
}
static void validate(yyjson_val *root) {
    static const char *kinds[] = {"brief", "goals", "relationships", "analysis", "sources"};
    assert(eq(root, "schema", "dsco.workspace.intake.v1"));
    assert(yyjson_is_true(get(root, "ok")));
    assert(eq(root, "project", "fixture"));
    yyjson_val *docs = get(root, "documents"), *nodes = get(root, "nodes");
    assert(yyjson_arr_size(docs) == 5);
    for (size_t i = 0; i < 5; i++) {
        yyjson_val *d = yyjson_arr_get(docs, i);
        assert(eq(d, "kind", kinds[i]) && str(d, "title") && str(d, "text"));
        assert(strstr(str(d, "text"), "Source: "));
        assert(strstr(str(d, "text"), "Basis: deterministic observed extraction"));
    }
    size_t i, n; yyjson_val *v;
    yyjson_arr_foreach(nodes, i, n, v) {
        assert(num(v, "id") == i && eq(v, "basis", "observed"));
    }
    yyjson_arr_foreach(get(root, "edges"), i, n, v) {
        assert(num(v, "from") < yyjson_arr_size(nodes));
        assert(num(v, "to") < yyjson_arr_size(nodes));
        assert(eq(v, "basis", "observed"));
    }
}
static yyjson_doc *success(const char *source, const char *content) {
    assert(workspace_intake_analyze("fixture", source, content, output, sizeof(output)));
    assert(strlen(output) <= WORKSPACE_INTAKE_MAX_OUTPUT);
    yyjson_doc *d = yyjson_read(output, strlen(output), 0); assert(d);
    validate(yyjson_doc_get_root(d)); cases++; return d;
}
static void error(const char *source, const char *content, const char *code) {
    memset(output, '?', sizeof(output));
    assert(!workspace_intake_analyze("fixture", source, content, output, sizeof(output)));
    yyjson_doc *d = yyjson_read(output, strlen(output), 0); assert(d);
    yyjson_val *r = yyjson_doc_get_root(d);
    assert(eq(r, "schema", "dsco.workspace.intake.v1"));
    assert(yyjson_is_false(get(r, "ok")) && eq(get(r, "error"), "code", code));
    assert(!get(r, "documents")); yyjson_doc_free(d); cases++;
}
static void markdown_test(void) {
    const char *input = "# Launch\r\n## Plan ##\n- [ ] Ship\n- [x] Test\n1. [X] Review\nTODO: Check data\nTitle\n=====\n~~~sh\n# Hidden\n- [ ] Hidden\n~~~\n    TODO: hidden\n\t# Hidden\n";
    yyjson_doc *d = success("notes.md", input); yyjson_val *r = yyjson_doc_get_root(d);
    yyjson_val *s = get(r, "summary"), *nodes = get(r, "nodes");
    assert(eq(s, "format", "markdown"));
    assert(num(s, "headings") == 3 && num(s, "goals") == 4);
    assert(num(s, "checked_goals") == 2 && num(s, "todo_markers") == 1);
    yyjson_val *v = find(nodes, "text", "Plan"); assert(v && num(v, "level") == 2);
    assert(num(find(nodes, "text", "Title"), "line") == 7);
    assert(!find(nodes, "text", "Hidden"));
    assert(strstr(str(yyjson_arr_get(get(r, "documents"), 1), "text"), "Suggested goals (not observed)"));
    yyjson_doc_free(d);
    d = success("notes.md", "````\n# hidden\n```\nTODO: hidden\n````\n# Visible\n");
    s = get(yyjson_doc_get_root(d), "summary");
    assert(num(s, "headings") == 1 && num(s, "goals") == 0); yyjson_doc_free(d);
}
static void associations_test(void) {
    const char *input = "Email Alice+ops@Example.COM and bob@example.org; visit example.net.\nNot a mailbox: x@localhost; not a domain: 192.168.1.1\n";
    yyjson_doc *d = success("people.txt", input); yyjson_val *r = yyjson_doc_get_root(d);
    assert(num(get(r, "summary"), "email_occurrences") == 2);
    assert(num(get(r, "summary"), "domain_occurrences") == 3);
    yyjson_val *v = find(get(r, "nodes"), "text", "Alice+ops@Example.COM");
    assert(v && eq(v, "kind", "email_occurrence") && num(v, "text_byte_offset") == 6);
    assert(find(get(r, "edges"), "relation", "observed_email_domain"));
    const char *text = str(yyjson_arr_get(get(r, "documents"), 2), "text");
    assert(strstr(text, "do not establish people, ownership, affiliation")); yyjson_doc_free(d);
}
static void json_test(void) {
    yyjson_doc *d = success("data.JSON", "{\"name\":\"A\",\"items\":[1,true,null,{\"email\":\"a@example.com\"}],\"big\":123456789012345678901234567890,\"decimal\":1e999}");
    yyjson_val *r = yyjson_doc_get_root(d), *nodes = get(r, "nodes");
    assert(eq(get(r, "summary"), "format", "json"));
    assert(eq(find(nodes, "key", "name"), "text", "A"));
    assert(eq(find(nodes, "key", "big"), "text", "123456789012345678901234567890"));
    assert(eq(find(nodes, "key", "decimal"), "text", "1e999"));
    assert(num(get(r, "summary"), "email_occurrences") == 1);
    assert(find(get(r, "edges"), "relation", "contains")); yyjson_doc_free(d);
    const char *scalars[] = {"null", "true", "123", "\"hello\"", "[]", "{}"};
    for (size_t i = 0; i < sizeof(scalars)/sizeof(*scalars); i++) {
        d = success("value.json", scalars[i]); yyjson_doc_free(d);
    }
    d = success("no-extension", " {\"x\":[2]}"); yyjson_doc_free(d);
    error("bad.json", "{\"a\":1,}", "invalid_json");
    error("bad.txt", "[1,]", "invalid_json");
    error("bad.json", "{broken", "invalid_json");
    error("bad.json", "{} trailing", "invalid_json");
    error("bad.json", "", "invalid_json");
    error("bad.json", "{\"a\":1,\"a\":2}", "duplicate_json_key");
    error("bad.json", "{\"a\":1,\"\\u0061\":2}", "duplicate_json_key");
    error("bad.json", "\"\\u0000\"", "invalid_text");
    error("bad.json", "{\"\\u001b\":0}", "invalid_text");
    error("bad.json", "\"\\u202e\"", "invalid_text");
}
static void csv_test(void) {
    const char *csv = "name,email,note\r\n\"Doe, Jane\",jane@example.com,\"first\nsecond \"\"quoted\"\"\"\r\nBob,bob@example.org,\"\"\r\n";
    yyjson_doc *d = success("contacts.csv", csv); yyjson_val *r = yyjson_doc_get_root(d);
    yyjson_val *c = get(r, "csv"), *profiles = get(c, "column_profiles");
    assert(eq(get(r, "summary"), "format", "csv"));
    assert(num(c, "records") == 3 && num(c, "columns") == 3);
    assert(yyjson_is_false(get(c, "header_assumed")));
    assert(num(yyjson_arr_get(profiles, 0), "max_bytes") == 9);
    assert(num(yyjson_arr_get(profiles, 1), "email_occurrences") == 2);
    assert(num(yyjson_arr_get(profiles, 2), "empty") == 1);
    assert(num(yyjson_arr_get(profiles, 2), "max_bytes") == strlen("first\nsecond \"quoted\""));
    yyjson_val *v = find(get(r, "nodes"), "text", "jane@example.com");
    assert(num(v, "row") == 2 && num(v, "column") == 2 && num(v, "cell_byte_offset") == 0);
    yyjson_doc_free(d);
    d = success("auto.txt", "a,b\n1,2\n"); assert(get(yyjson_doc_get_root(d), "csv")); yyjson_doc_free(d);
    d = success("ragged.csv", "a,b,c\n1,\n\n"); c = get(yyjson_doc_get_root(d), "csv");
    assert(num(c, "records") == 3 && num(c, "columns") == 3);
    profiles = get(c, "column_profiles");
    assert(num(yyjson_arr_get(profiles, 1), "empty") == 1);
    assert(num(yyjson_arr_get(profiles, 2), "missing") == 2); yyjson_doc_free(d);
    d = success("empty.csv", ""); assert(num(get(yyjson_doc_get_root(d), "csv"), "records") == 0); yyjson_doc_free(d);
    d = success("last.csv", "a,b,"); assert(num(get(yyjson_doc_get_root(d), "csv"), "columns") == 3); yyjson_doc_free(d);
    error("bad.csv", "a,\"unterminated", "invalid_csv");
    error("bad.csv", "a,\"quoted\"junk", "invalid_csv");
    error("bad.csv", "a,b\"c", "invalid_csv");
}
static void text_and_safety_test(void) {
    yyjson_doc *d = success("source.txt", "Ordinary text café 世界 😀\n");
    yyjson_val *r = yyjson_doc_get_root(d);
    assert(eq(get(r, "summary"), "format", "text") && num(get(r, "summary"), "lines") == 1);
    assert(num(get(r, "summary"), "whitespace_tokens") == 5); yyjson_doc_free(d);
    d = success("empty.txt", ""); assert(num(get(yyjson_doc_get_root(d), "summary"), "bytes") == 0); yyjson_doc_free(d);
    const char *hostile = "```\n<script>alert(1)</script>\n```\n![x](file:///etc/passwd)\n$(touch /tmp/do-not-execute-intake)\n&#60;iframe&#62;\n";
    d = success("<script>\n```&.md", hostile); r = yyjson_doc_get_root(d);
    size_t i, n; yyjson_val *v;
    yyjson_arr_foreach(get(r, "documents"), i, n, v) {
        const char *text = str(v, "text");
        assert(!strstr(text, "<script>") && !strstr(text, "```") && !strstr(text, "![x]"));
        assert(strstr(text, "Source: &#60;script&#62;&#10;&#96;&#96;&#96;&#38;&#46;md"));
    }
    const char *text = str(yyjson_arr_get(get(r, "documents"), 4), "text");
    assert(strstr(text, "&#38;&#35;60&#59;iframe")); yyjson_doc_free(d);
    error("bad.txt", "abc\x01" "def", "invalid_text");
    error("bad.txt", "\x7f", "invalid_text");
    error("bad.txt", "\xc0\xaf", "invalid_text");
    error("bad.txt", "\xed\xa0\x80", "invalid_text");
    error("bad.txt", "\xf4\x90\x80\x80", "invalid_text");
    error("bad.txt", "\xe2\x82", "invalid_text");
    error("bad.txt", "\xc2\x85", "invalid_text");
    error("bad.txt", "\xe2\x80\xae", "invalid_text");
    error("\x1b", "fine", "invalid_text");
}
static void limits_test(void) {
    char *large = malloc(WORKSPACE_INTAKE_MAX_INPUT + 2); assert(large);
    memset(large, 'a', WORKSPACE_INTAKE_MAX_INPUT + 1); large[WORKSPACE_INTAKE_MAX_INPUT + 1] = 0;
    error("input.txt", large, "input_limit");
    large[WORKSPACE_INTAKE_MAX_INPUT] = 0; error("output.txt", large, "output_limit");
    large[120000] = 0; yyjson_doc *d = success("fits.txt", large); yyjson_doc_free(d);
    memset(large, '#', 30000); large[30000] = 0; error("escaped.txt", large, "output_limit");
    memset(large, 'x', 4097); large[4097] = 0; error(large, "a", "metadata_limit");
    memset(large, '\n', 4097); large[4097] = 0; error("rows.csv", large, "csv_row_limit");
    memset(large, ',', 256); large[256] = 0; error("cols.csv", large, "csv_column_limit");
    for (size_t i = 0; i < 2048; i++) memcpy(large + i * 4, "# h\n", 4);
    large[2048 * 4] = 0; error("nodes.md", large, "node_limit");
    memset(large, '[', 66); memset(large + 66, ']', 66); large[132] = 0;
    error("deep.json", large, "json_depth_limit"); free(large);
    d = success("capacity.txt", "hello"); yyjson_doc_free(d); size_t len = strlen(output);
    char *exact = malloc(len + 2); assert(exact); exact[len + 1] = 'Z';
    assert(workspace_intake_analyze("fixture", "capacity.txt", "hello", exact, len + 1));
    assert(strlen(exact) == len && exact[len + 1] == 'Z');
    assert(!workspace_intake_analyze("fixture", "capacity.txt", "hello", exact, len));
    d = yyjson_read(exact, strlen(exact), 0); assert(d);
    assert(eq(get(yyjson_doc_get_root(d), "error"), "code", "output_capacity")); yyjson_doc_free(d); free(exact);
    char tiny[2] = {'Q', 'Z'};
    assert(!workspace_intake_analyze("fixture", "x", "hello", tiny, 1)); assert(!tiny[0] && tiny[1] == 'Z');
    tiny[0] = 'Q'; assert(!workspace_intake_analyze("fixture", "x", "hello", tiny, 0)); assert(tiny[0] == 'Q');
    assert(!workspace_intake_analyze("fixture", "x", "hello", NULL, 20));
    error("x", NULL, "invalid_argument"); error(NULL, "x", "invalid_argument");
    assert(!workspace_intake_analyze(NULL, "x", "x", output, sizeof(output)));
    char diagnostic[WORKSPACE_INTAKE_ERROR_CAP];
    assert(!workspace_intake_analyze(NULL, "x", "x", diagnostic, sizeof(diagnostic)));
    d = yyjson_read(diagnostic, strlen(diagnostic), 0); assert(d); yyjson_doc_free(d); cases++;
}
static void deterministic_fuzz_test(void) {
    uint32_t rng = 12345;
    const char alphabet[] = "ab12 #[]{}\"`,@.-_<>\n\r\t";
    char input[257], again[WORKSPACE_INTAKE_MAX_OUTPUT + 1];
    const char *sources[] = {"sample.txt", "sample.md", "sample.csv", "sample.json"};
    for (unsigned trial = 0; trial < 400; trial++) {
        size_t n = trial % sizeof(input);
        for (size_t i = 0; i < n; i++) { rng = rng * 1664525u + 1013904223u; input[i] = alphabet[rng % (sizeof(alphabet) - 1)]; }
        input[n] = 0;
        const char *source = sources[trial % 4];
        bool ok = workspace_intake_analyze("fixture", source, input, output, sizeof(output));
        bool repeated = workspace_intake_analyze("fixture", source, input, again, sizeof(again));
        assert(ok == repeated && !strcmp(output, again));
        yyjson_doc *d = yyjson_read(output, strlen(output), 0); assert(d);
        if (ok) validate(yyjson_doc_get_root(d));
        else assert(yyjson_is_false(get(yyjson_doc_get_root(d), "ok")));
        yyjson_doc_free(d); cases++;
    }
}
int main(void) {
    markdown_test(); associations_test(); json_test(); csv_test(); text_and_safety_test();
    limits_test(); deterministic_fuzz_test();
    printf("workspace_intake: %u cases passed\n", cases); return 0;
}
