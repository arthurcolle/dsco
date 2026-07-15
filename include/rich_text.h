#ifndef DSCO_RICH_TEXT_H
#define DSCO_RICH_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A compact, allocation-free semantic stream for native transcript surfaces.
 * ANSI terminals continue to use md.c; pixel renderers consume these tokens so
 * Markdown structure and math survive instead of being flattened to raw text. */
typedef enum {
    RICH_TOKEN_TEXT = 0,
    RICH_TOKEN_BREAK,
    RICH_TOKEN_RULE,
} rich_token_type_t;

typedef enum {
    RICH_STYLE_BODY = 0,
    RICH_STYLE_STRONG,
    RICH_STYLE_EMPHASIS,
    RICH_STYLE_STRIKE,
    RICH_STYLE_CODE,
    RICH_STYLE_LINK,
    RICH_STYLE_MATH,
    RICH_STYLE_MATH_DISPLAY,
    RICH_STYLE_HEADING,
    RICH_STYLE_QUOTE,
    RICH_STYLE_LIST_MARKER,
    RICH_STYLE_MUTED,
} rich_style_t;

#define RICH_TOKEN_TEXT_MAX 384

typedef struct {
    rich_token_type_t type;
    rich_style_t style;
    uint8_t level;
    uint8_t indent;
    bool block_start;
    char text[RICH_TOKEN_TEXT_MAX];
} rich_token_t;

/* Parse CommonMark/GFM presentation primitives and MathJax-style LaTeX
 * delimiters ($, $$, \(...\), \[...\]) into a bounded semantic token stream. */
size_t rich_text_parse(const char *markdown, rich_token_t *tokens, size_t capacity);

/* Convert the terminal-relevant LaTeX subset to typographic Unicode. The
 * conversion is deliberately deterministic and dependency-free. */
size_t rich_text_latex_to_unicode(const char *latex, char *out, size_t out_size);

#endif /* DSCO_RICH_TEXT_H */
