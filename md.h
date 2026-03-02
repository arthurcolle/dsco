#ifndef DSCO_MD_H
#define DSCO_MD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* ── Streaming Markdown Renderer ─────────────────────────────────────────
 * Full CommonMark + GFM + LaTeX + HTML → ANSI terminal rendering.
 * Designed for streaming: feed arbitrary chunks, get rendered output.
 *
 * Supported features:
 *   CommonMark:
 *     ATX headers (#..######), setext headers (=== / ---), thematic breaks
 *     Paragraphs, hard line breaks (trailing \\, trailing 2+ spaces)
 *     Indented code blocks (4 spaces / tab)
 *     Fenced code blocks (``` / ~~~) with language + syntax highlighting
 *     Blockquotes (> nested >> nested), lazy continuation
 *     Ordered lists (1. / 1)), unordered lists (- * +)
 *     Nested lists via indentation (up to 10 levels)
 *     List continuation paragraphs
 *     Inline: **bold**, *italic*, ***bold-italic***, `code`, ~~strike~~
 *     Multi-backtick code spans (`` `code` ``)
 *     Links [text](url "title"), reference links [text][id], autolinks
 *     Images ![alt](url), footnotes [^id], abbreviations
 *     Backslash escapes for punctuation
 *     HTML entities (&amp; &lt; &#123; &#x1F600;)
 *
 *   GFM extensions:
 *     Tables with alignment (| :--- | :---: | ---: |)
 *     Task lists (- [ ] / - [x])
 *     Strikethrough (~~text~~)
 *     Autolinks (bare URLs, emails)
 *     Emoji shortcodes (:rocket: :smile: etc.)
 *     Alerts/callouts (> [!NOTE], > [!WARNING], etc.)
 *
 *   LaTeX:
 *     Inline $...$ and display $$...$$
 *     Multi-line display blocks
 *     Greek letters, math operators, relations, arrows, sets
 *     Superscript ^{n}, subscript _{n}, \frac{a}{b}
 *     \sqrt, \text, \mathrm, sizing commands, environments
 *
 *   HTML:
 *     Block: <div>, <pre>, <table>, <details>/<summary>, <blockquote>
 *     Inline: <b>, <strong>, <i>, <em>, <code>, <u>, <s>, <del>, <mark>
 *     <a href>, <sup>, <sub>, <br>, <hr>, <img>
 *     <h1>-<h6>, <p>, <ul>/<ol>/<li>, <kbd>
 *     Entity decoding: named (&mdash;), decimal (&#123;), hex (&#x1F600;)
 * ────────────────────────────────────────────────────────────────────────── */

#define MD_LINE_MAX      4096
#define MD_BUF_MAX       65536
#define MD_TABLE_MAXCOL  16
#define MD_TABLE_MAXROW  128
#define MD_LIST_MAXDEPTH 10
#define MD_REF_MAX       64
#define MD_FOOTNOTE_MAX  32

typedef enum {
    MD_STATE_NORMAL,
    MD_STATE_CODE_BLOCK,
    MD_STATE_TABLE,
    MD_STATE_HTML_BLOCK,
    MD_STATE_LATEX_BLOCK,     /* multi-line $$...$$ */
} md_state_t;

/* Table column alignment */
typedef enum {
    MD_ALIGN_LEFT,
    MD_ALIGN_CENTER,
    MD_ALIGN_RIGHT,
} md_align_t;

/* List item type */
typedef enum {
    MD_LIST_NONE,
    MD_LIST_UNORDERED,
    MD_LIST_ORDERED,
} md_list_type_t;

/* Reference link storage */
typedef struct {
    char id[128];
    char url[512];
    char title[256];
} md_ref_link_t;

/* Footnote storage */
typedef struct {
    char id[64];
    char text[512];
} md_footnote_t;

typedef struct {
    /* Line accumulator for streaming */
    char line_buf[MD_LINE_MAX];
    int  line_len;

    /* Block-level state */
    md_state_t state;

    /* Code block state */
    char code_lang[64];
    char code_buf[MD_BUF_MAX];
    int  code_len;
    char code_fence[8];   /* ``` or ~~~ */
    int  code_fence_len;

    /* Table state */
    char *table_cells[MD_TABLE_MAXROW][MD_TABLE_MAXCOL];
    int  table_cols;
    int  table_rows;
    bool table_has_sep;
    md_align_t table_align[MD_TABLE_MAXCOL];

    /* HTML block accumulator */
    char html_buf[MD_BUF_MAX];
    int  html_len;
    char html_tag[32];

    /* LaTeX display block accumulator */
    char latex_buf[MD_BUF_MAX];
    int  latex_len;

    /* Reference links */
    md_ref_link_t refs[MD_REF_MAX];
    int ref_count;

    /* Footnotes */
    md_footnote_t footnotes[MD_FOOTNOTE_MAX];
    int footnote_count;
    int footnote_render_idx;

    /* Output target */
    FILE *out;
    int   term_width;

    /* List nesting stack */
    md_list_type_t list_stack[MD_LIST_MAXDEPTH];
    int  list_num[MD_LIST_MAXDEPTH];    /* current number for ordered */
    int  list_depth;                     /* current nesting depth */

    /* Tracking */
    bool in_list;
    bool last_was_blank;
    char prev_line[MD_LINE_MAX]; /* for setext heading detection */
    bool prev_line_valid;
    int  blockquote_depth;       /* nested > tracking */
} md_renderer_t;

/* Initialize renderer. out=stdout typically. */
void md_init(md_renderer_t *r, FILE *out);

/* Feed a streaming chunk of text (any size, may be partial lines). */
void md_feed(md_renderer_t *r, const char *text, size_t len);

/* Feed a null-terminated string. */
void md_feed_str(md_renderer_t *r, const char *text);

/* Flush any remaining buffered content (call at end of stream). */
void md_flush(md_renderer_t *r);

/* Reset renderer state for a new message. */
void md_reset(md_renderer_t *r);

#endif
