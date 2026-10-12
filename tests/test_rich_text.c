/* Headless parser tests; no renderer or window creation. */
#include "rich_text.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CAPACITY 512
static rich_token_t tokens[CAPACITY];

/* Concatenating code TEXT and BREAK tokens must recover every source byte. */
static size_t parse_code(const char *hint, const char *body) {
    char markdown[20000], recovered[20000];
    int len = snprintf(markdown, sizeof(markdown), "```%s\n%s```\n", hint, body);
    assert(len > 0 && (size_t)len < sizeof(markdown));
    size_t n = rich_text_parse(markdown, tokens, CAPACITY);
    size_t begin = *hint ? 2 : 0, used = 0;
    bool first = true;
    assert(n >= begin && n < CAPACITY);
    if (begin) {
        assert(tokens[0].style == RICH_STYLE_MUTED);
        assert(tokens[1].type == RICH_TOKEN_BREAK);
    }
    for (size_t i = begin; i < n; i++) {
        const rich_token_t *t = &tokens[i];
        assert(rich_style_is_code(t->style));
        assert(t->indent == 0 && t->level == 0);
        if (t->type == RICH_TOKEN_TEXT) {
            assert(t->block_start == first);
            size_t len = strlen(t->text);
            assert(used + len + 1 < sizeof(recovered));
            memcpy(recovered + used, t->text, len);
            used += len;
            if (!len) assert(t->style == RICH_STYLE_CODE);
            first = false;
        } else {
            assert(t->type == RICH_TOKEN_BREAK && !first && !t->block_start);
            recovered[used++] = '\n';
            first = true;
        }
    }
    recovered[used] = 0;
    assert(!strcmp(recovered, body));
    return n;
}

static void expect(size_t n, rich_style_t style, const char *text) {
    for (size_t i = 0; i < n; i++)
        if (tokens[i].type == RICH_TOKEN_TEXT && tokens[i].style == style &&
            !strcmp(tokens[i].text, text)) return;
    fprintf(stderr, "missing style %d: [%s]\n", style, text);
    assert(0);
}

static void test_classes(void) {
    size_t n = parse_code("c", "  \tint x = 42;  // comment\t\nconst char *s = \"if \\\"x\\\"\";\n");
    expect(n, RICH_STYLE_CODE_KEYWORD, "int");
    expect(n, RICH_STYLE_CODE_KEYWORD, "const");
    expect(n, RICH_STYLE_CODE_NUMBER, "42");
    expect(n, RICH_STYLE_CODE_STRING, "\"if \\\"x\\\"\"");
    expect(n, RICH_STYLE_CODE_COMMENT, "// comment\t");
    expect(n, RICH_STYLE_CODE, "  \t");

    n = parse_code("Python linenos", "\tdef f():\n   return 'value' + 1.25e-3 # note\n");
    expect(n, RICH_STYLE_CODE_KEYWORD, "def");
    expect(n, RICH_STYLE_CODE_KEYWORD, "return");
    expect(n, RICH_STYLE_CODE_STRING, "'value'");
    expect(n, RICH_STYLE_CODE_NUMBER, "1.25e-3");
    expect(n, RICH_STYLE_CODE_COMMENT, "# note");

    n = parse_code("js", " const x = `hello`; // note\nlet z = 0xff;\n");
    expect(n, RICH_STYLE_CODE_KEYWORD, "const");
    expect(n, RICH_STYLE_CODE_STRING, "`hello`");
    expect(n, RICH_STYLE_CODE_NUMBER, "0xff");
    expect(n, RICH_STYLE_CODE_COMMENT, "// note");

    const char *aliases[] = {"cpp", "java", "go", "rust", "ts", "jsx"};
    for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); i++) {
        n = parse_code(aliases[i], "return 7;\n");
        expect(n, RICH_STYLE_CODE_KEYWORD, "return");
        expect(n, RICH_STYLE_CODE_NUMBER, "7");
    }
    n = parse_code("bash", "if true; then echo 'yes'; fi # shell\n");
    expect(n, RICH_STYLE_CODE_KEYWORD, "then");
    expect(n, RICH_STYLE_CODE_STRING, "'yes'");
    expect(n, RICH_STYLE_CODE_COMMENT, "# shell");
    n = parse_code("json", "{\"ok\": true, \"n\": 12}\n");
    expect(n, RICH_STYLE_CODE_STRING, "\"ok\"");
    expect(n, RICH_STYLE_CODE_KEYWORD, "true");
    expect(n, RICH_STYLE_CODE_NUMBER, "12");
}

static void test_whitespace_and_fallback(void) {
    const char *body = "\n \t  x\t=\t42;  \t\n\n\t \n   \n\n";
    size_t n = parse_code("c", body), empty = 0;
    for (size_t i = 0; i < n; i++)
        empty += tokens[i].type == RICH_TOKEN_TEXT && tokens[i].style == RICH_STYLE_CODE &&
                 !*tokens[i].text;
    assert(empty == 3);
    parse_code("c", "\tint x = 1;  \r\n\r\n"); /* CR bytes are not trimmed. */
    const char *hints[] = {"", "unknown"};
    for (size_t h = 0; h < sizeof(hints) / sizeof(*hints); h++) {
        n = parse_code(hints[h], " \tint x = 42; // plain\n\n");
        for (size_t i = *hints[h] ? 2 : 0; i < n; i++)
            assert(tokens[i].style == RICH_STYLE_CODE);
    }
    n = parse_code("diff", " +added\n\t-removed\n@@ hunk\n\n");
    expect(n, RICH_STYLE_DIFF_ADD, " +added");
    expect(n, RICH_STYLE_DIFF_REMOVE, "\t-removed");
    expect(n, RICH_STYLE_DIFF_HUNK, "@@ hunk");
}

static void test_multiline_and_inline(void) {
    size_t n = parse_code("c", "/* int 42\n\n  comment */ return 2;\n");
    expect(n, RICH_STYLE_CODE_COMMENT, "/* int 42");
    expect(n, RICH_STYLE_CODE_COMMENT, "  comment */");
    expect(n, RICH_STYLE_CODE_KEYWORD, "return");
    n = parse_code("py", "s = \"\"\"first\n\n\t# still string\nlast\"\"\"\nreturn 2\n");
    expect(n, RICH_STYLE_CODE_STRING, "\t# still string");
    expect(n, RICH_STYLE_CODE_STRING, "last\"\"\"");
    expect(n, RICH_STYLE_CODE_KEYWORD, "return");
    n = parse_code("js", "const s = `first\n\t// string`;\n");
    expect(n, RICH_STYLE_CODE_STRING, "\t// string`");
    n = rich_text_parse("```c\n/* unfinished\n```\n```c\nreturn 3;\n```", tokens, CAPACITY);
    expect(n, RICH_STYLE_CODE_KEYWORD, "return");
    n = rich_text_parse("use `  int\tx = 42; // raw  ` now", tokens, CAPACITY);
    expect(n, RICH_STYLE_CODE, "  int\tx = 42; // raw  ");
    for (size_t i = 0; i < n; i++)
        assert(tokens[i].style < RICH_STYLE_CODE_KEYWORD);
    n = rich_text_parse("tool_response and *emphasis*", tokens, CAPACITY);
    expect(n, RICH_STYLE_BODY, "tool_response and ");
    expect(n, RICH_STYLE_EMPHASIS, "emphasis");
}

static void test_long_lines_and_bounds(void) {
    char body[10000];
    memset(body, ' ', 5000);
    strcpy(body + 5000, "int x = 42;\t\n");
    size_t n = parse_code("c", body);
    expect(n, RICH_STYLE_CODE_KEYWORD, "int");
    memset(body, 'x', 6000);
    strcpy(body + 6000, "\t \n");
    parse_code("", body);
    parse_code("c", body);
    n = rich_text_parse("````c\n```\n~~~\n  int x;\n````\n", tokens, CAPACITY);
    expect(n, RICH_STYLE_CODE, "```");
    expect(n, RICH_STYLE_CODE, "~~~");
    expect(n, RICH_STYLE_CODE_KEYWORD, "int");
    n = rich_text_parse("~~~python\nreturn 2\n~~~\n", tokens, CAPACITY);
    expect(n, RICH_STYLE_CODE_KEYWORD, "return");
    n = rich_text_parse("```c\n\treturn 42", tokens, CAPACITY);
    expect(n, RICH_STYLE_CODE_NUMBER, "42");
    for (size_t cap = 0; cap < 16; cap++) {
        rich_token_t bounded[17];
        memset(bounded, 0xa5, sizeof(bounded));
        rich_token_t sentinel = bounded[cap];
        assert(rich_text_parse("```c\n\n int x = 3; // hi\n```", bounded, cap) <= cap);
        assert(!memcmp(&sentinel, &bounded[cap], sizeof(sentinel)));
    }
}

int main(void) {
    assert(rich_style_is_code(RICH_STYLE_CODE_KEYWORD));
    assert(rich_style_is_code(RICH_STYLE_CODE_STRING));
    assert(rich_style_is_code(RICH_STYLE_CODE_COMMENT));
    assert(rich_style_is_code(RICH_STYLE_CODE_NUMBER));
    assert(!rich_style_is_code(RICH_STYLE_BODY));
    assert(!rich_style_is_code(RICH_STYLE_TABLE));
    test_classes();
    test_whitespace_and_fallback();
    test_multiline_and_inline();
    test_long_lines_and_bounds();
    puts("PASS: rich text highlighting, exact bytes/tabs/blank lines, inline code, bounds");
    return 0;
}
