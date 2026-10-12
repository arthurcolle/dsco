#ifndef DSCO_RTF_H
#define DSCO_RTF_H

/* Minimal RTF export for Markdown documents with LaTeX math.
 *
 * Complements, not replaces, the terminal pipeline: md.c renders MD+LaTeX to
 * ANSI for the TTY; this renders the same dialect to a portable .rtf document
 * for Word / TextEdit / Pages. Math goes through md.c's own LaTeX->Unicode
 * converter, so output matches what the terminal shows.
 *
 * Supported: headings (#..######), **bold**, *italic*, `code`, ~~strike~~,
 * [links](url), unordered/ordered lists, > quotes, --- rules, fenced code
 * blocks, and inline/display math ($..$, \(..\), $$..$$, \[..\]).
 * No malloc, no globals, no dependencies beyond md.h. */

#include <stdio.h>

/* Render markdown to RTF, written to out. Returns 0 on success. */
int rtf_render_markdown(FILE *out, const char *markdown);

/* Convenience: read in_path (markdown), write out_path (rtf). 0 on success. */
int rtf_render_file(const char *in_path, const char *out_path);

#endif /* DSCO_RTF_H */
