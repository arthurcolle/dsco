#include "rtf.h"
#include "md.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ── RTF output helpers ─────────────────────────────────────────────── */

/* Escape one UTF-8 string into RTF: ASCII passes through with \ { } escaped,
 * non-ASCII becomes \uN? signed 16-bit Unicode. */
static void rtf_puts_escaped(FILE *out, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            if (*p == '\\' || *p == '{' || *p == '}') fputc('\\', out);
            if (*p == '\n') { fputs("\\line ", out); p++; continue; }
            fputc(*p, out);
            p++;
        } else {
            unsigned cp; int n;
            if      ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 1; }
            else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 2; }
            else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 3; }
            else { p++; continue; }
            for (int i = 0; i < n && p[1 + i]; i++) cp = (cp << 6) | (p[1 + i] & 0x3F);
            if (cp > 0xFFFF) cp = 0xFFFD;              /* no surrogate pairs in RTF \u */
            fprintf(out, "\\u%d?", (int)(short)cp);    /* signed 16-bit per spec */
            p += n + 1;
        }
    }
}

static void rtf_raw(FILE *out, const char *s) { fputs(s, out); }

/* ── math ───────────────────────────────────────────────────────────── */

/* Convert a LaTeX math span to Unicode (via md.c) and emit it italic. */
static void rtf_math(FILE *out, const char *tex, size_t len, int display) {
    char *uni = md_latex_to_unicode(tex, len);
    if (display) rtf_raw(out, "\\par\\pard\\qc ");
    rtf_raw(out, "\\i ");
    rtf_puts_escaped(out, uni);
    rtf_raw(out, "\\i0 ");
    if (display) rtf_raw(out, "\\par\\pard\\ql ");
    free(uni);
}

/* ── inline span parsing ────────────────────────────────────────────── */

/* Render one line of markdown inline content with RTF groups for styles. */
static void rtf_inline(FILE *out, const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        /* display math $$..$$ */
        if (i + 1 < len && s[i] == '$' && s[i + 1] == '$') {
            const char *e = strstr(s + i + 2, "$$");
            if (e && (size_t)(e - s) < len) { rtf_math(out, s + i + 2, (size_t)(e - (s + i + 2)), 1); i = (size_t)(e - s) + 2; continue; }
        }
        /* inline math $..$ */
        if (s[i] == '$') {
            const char *e = strchr(s + i + 1, '$');
            if (e && (size_t)(e - s) < len && e != s + i + 1) { rtf_math(out, s + i + 1, (size_t)(e - (s + i + 1)), 0); i = (size_t)(e - s) + 1; continue; }
        }
        /* \(..\) and \[..\] */
        if (s[i] == '\\' && i + 1 < len && (s[i + 1] == '(' || s[i + 1] == '[')) {
            char close = (s[i + 1] == '(') ? ')' : ']';
            const char *e = strstr(s + i + 2, (close == ')') ? "\\)" : "\\]");
            if (e && (size_t)(e - s) < len) { rtf_math(out, s + i + 2, (size_t)(e - (s + i + 2)), close == ']'); i = (size_t)(e - s) + 2; continue; }
        }
        /* bold **..** or __..__ */
        if (i + 1 < len && ((s[i] == '*' && s[i + 1] == '*') || (s[i] == '_' && s[i + 1] == '_'))) {
            char m[3] = {s[i], s[i], 0};
            const char *e = strstr(s + i + 2, m);
            if (e && (size_t)(e - s) < len) {
                rtf_raw(out, "\\b ");
                rtf_inline(out, s + i + 2, (size_t)(e - (s + i + 2)));
                rtf_raw(out, "\\b0 ");
                i = (size_t)(e - s) + 2; continue;
            }
        }
        /* italic *..* or _.._ */
        if (s[i] == '*' || s[i] == '_') {
            const char *e = strchr(s + i + 1, s[i]);
            if (e && (size_t)(e - s) < len && e != s + i + 1) {
                rtf_raw(out, "\\i ");
                rtf_inline(out, s + i + 1, (size_t)(e - (s + i + 1)));
                rtf_raw(out, "\\i0 ");
                i = (size_t)(e - s) + 1; continue;
            }
        }
        /* strikethrough ~~..~~ */
        if (i + 1 < len && s[i] == '~' && s[i + 1] == '~') {
            const char *e = strstr(s + i + 2, "~~");
            if (e && (size_t)(e - s) < len) {
                rtf_raw(out, "\\strike ");
                rtf_inline(out, s + i + 2, (size_t)(e - (s + i + 2)));
                rtf_raw(out, "\\strike0 ");
                i = (size_t)(e - s) + 2; continue;
            }
        }
        /* inline code `..` */
        if (s[i] == '`') {
            const char *e = strchr(s + i + 1, '`');
            if (e && (size_t)(e - s) < len) {
                rtf_raw(out, "{\\f1\\fs20 ");
                for (const char *q = s + i + 1; q < e; q++) {
                    if (*q == '\\' || *q == '{' || *q == '}') fputc('\\', out);
                    fputc(*q, out);
                }
                rtf_raw(out, "}");
                i = (size_t)(e - s) + 1; continue;
            }
        }
        /* link [text](url) -> styled text; RTF hyperlink fields are overkill here */
        if (s[i] == '[') {
            const char *mid = strstr(s + i, "](");
            if (mid && (size_t)(mid - s) < len) {
                const char *e = strchr(mid + 2, ')');
                if (e && (size_t)(e - s) < len) {
                    rtf_raw(out, "\\ul ");
                    rtf_inline(out, s + i + 1, (size_t)(mid - (s + i + 1)));
                    rtf_raw(out, "\\ul0 ");
                    i = (size_t)(e - s) + 1; continue;
                }
            }
        }
        /* plain char */
        if (s[i] == '\\' || s[i] == '{' || s[i] == '}') fputc('\\', out);
        fputc(s[i], out);
        i++;
    }
}

/* ── block parsing ──────────────────────────────────────────────────── */

static int is_blank(const char *l) { while (*l) { if (!isspace((unsigned char)*l)) return 0; l++; } return 1; }

static int heading_level(const char *l) {
    int n = 0;
    while (n < 6 && l[n] == '#') n++;
    return (n > 0 && (l[n] == ' ' || l[n] == '\t')) ? n : 0;
}

int rtf_render_markdown(FILE *out, const char *markdown) {
    if (!out || !markdown) return -1;

    /* header: Times body (f0), Courier code (f1) */
    rtf_raw(out, "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}{\\f1\\fmodern Courier;}}\n");
    rtf_raw(out, "\\paperw12240\\paperh15840\\margl1440\\margr1440\\fs24\n");

    int in_code = 0;
    const char *p = markdown;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        char line[4096];
        if (llen >= sizeof line) llen = sizeof line - 1;
        memcpy(line, p, llen);
        line[llen] = '\0';

        /* fenced code */
        if (strncmp(line, "```", 3) == 0) {
            in_code = !in_code;
            rtf_raw(out, "\\par ");
            p = nl ? nl + 1 : p + llen;
            continue;
        }
        if (in_code) {
            rtf_raw(out, "\\f1\\fs20\\li360 ");
            rtf_puts_escaped(out, line);
            rtf_raw(out, "\\par\\f0\\fs24\\li0 ");
            p = nl ? nl + 1 : p + llen;
            continue;
        }

        if (is_blank(line)) {
            rtf_raw(out, "\\par\n");
        } else if (strncmp(line, "---", 3) == 0 || strncmp(line, "***", 3) == 0) {
            rtf_raw(out, "\\par\\pard\\brdrb\\brdrs\\brdrw10 \\par\\pard ");
        } else if (heading_level(line)) {
            int lvl = heading_level(line);
            int sz = 44 - lvl * 4;              /* h1=40pt .. h6=20pt (half-points) */
            fprintf(out, "\\par\\pard\\b\\fs%d ", sz);
            rtf_inline(out, line + lvl + 1, strlen(line + lvl + 1));
            rtf_raw(out, "\\b0\\fs24\\par\n");
        } else if (line[0] == '>') {
            rtf_raw(out, "\\par\\pard\\li420\\i ");
            rtf_inline(out, line + 1 + (line[1] == ' ' ? 1 : 0), strlen(line) - 1 - (line[1] == ' ' ? 1 : 0));
            rtf_raw(out, "\\i0\\par\\pard ");
        } else if ((line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            rtf_raw(out, "\\par\\pard\\li360\\fi-180 \\'b7\\tab ");
            rtf_inline(out, line + 2, strlen(line + 2));
            rtf_raw(out, "\\par\\pard ");
        } else if (isdigit((unsigned char)line[0]) && strchr(line, '.') && strchr(line, '.') - line < 4) {
            const char *dot = strchr(line, '.');
            rtf_raw(out, "\\par\\pard\\li360\\fi-180 ");
            fwrite(line, 1, (size_t)(dot - line + 1), out);
            rtf_raw(out, "\\tab ");
            rtf_inline(out, dot + 2, strlen(dot + 2));
            rtf_raw(out, "\\par\\pard ");
        } else {
            rtf_raw(out, "\\par\\pard ");
            rtf_inline(out, line, strlen(line));
            rtf_raw(out, "\\par\n");
        }

        p = nl ? nl + 1 : p + llen;
    }

    rtf_raw(out, "}\n");
    return ferror(out) ? -1 : 0;
}

int rtf_render_file(const char *in_path, const char *out_path) {
    FILE *in = fopen(in_path, "r");
    if (!in) return -1;
    if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return -1; }
    long sz = ftell(in);
    if (sz < 0 || fseek(in, 0, SEEK_SET) != 0) { fclose(in); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(in); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, in);
    fclose(in);
    buf[got] = '\0';

    FILE *out = fopen(out_path, "w");
    if (!out) { free(buf); return -1; }
    int rc = rtf_render_markdown(out, buf);
    fclose(out);
    free(buf);
    return rc;
}
