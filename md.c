#include "md.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static int term_width(void) {
    struct winsize w;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    return 80;
}

static int str_starts(const char *s, const char *prefix) {
    int n = 0;
    while (prefix[n] && s[n] == prefix[n]) n++;
    return prefix[n] == '\0' ? n : 0;
}

static bool is_blank_line(const char *line) {
    while (*line) {
        if (*line != ' ' && *line != '\t' && *line != '\r' && *line != '\n')
            return false;
        line++;
    }
    return true;
}

/* Count visible characters (skip ANSI escapes + UTF-8 continuation) */
static int vis_len(const char *s) {
    int len = 0;
    bool in_esc = false;
    for (const char *p = s; *p; p++) {
        if (*p == '\033') { in_esc = true; continue; }
        if (in_esc) {
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) in_esc = false;
            continue;
        }
        if (((unsigned char)*p & 0xC0) == 0x80) continue;
        len++;
    }
    return len;
}

/* Strip leading/trailing whitespace in-place, return new start */
static char *str_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = '\0';
    return s;
}

/* ── LaTeX Symbol Table ───────────────────────────────────────────────── */

typedef struct { const char *tex; const char *utf8; } latex_sym_t;

static const latex_sym_t LATEX_SYMBOLS[] = {
    /* Greek lowercase */
    {"\\alpha",    "\xCE\xB1"}, {"\\beta",    "\xCE\xB2"}, {"\\gamma",   "\xCE\xB3"},
    {"\\delta",    "\xCE\xB4"}, {"\\epsilon", "\xCE\xB5"}, {"\\zeta",    "\xCE\xB6"},
    {"\\eta",      "\xCE\xB7"}, {"\\theta",   "\xCE\xB8"}, {"\\iota",    "\xCE\xB9"},
    {"\\kappa",    "\xCE\xBA"}, {"\\lambda",  "\xCE\xBB"}, {"\\mu",      "\xCE\xBC"},
    {"\\nu",       "\xCE\xBD"}, {"\\xi",      "\xCE\xBE"}, {"\\pi",      "\xCF\x80"},
    {"\\rho",      "\xCF\x81"}, {"\\sigma",   "\xCF\x83"}, {"\\tau",     "\xCF\x84"},
    {"\\upsilon",  "\xCF\x85"}, {"\\phi",     "\xCF\x86"}, {"\\chi",     "\xCF\x87"},
    {"\\psi",      "\xCF\x88"}, {"\\omega",   "\xCF\x89"},
    /* Greek uppercase */
    {"\\Alpha",    "\xCE\x91"}, {"\\Beta",    "\xCE\x92"}, {"\\Gamma",   "\xCE\x93"},
    {"\\Delta",    "\xCE\x94"}, {"\\Epsilon", "\xCE\x95"}, {"\\Zeta",    "\xCE\x96"},
    {"\\Eta",      "\xCE\x97"}, {"\\Theta",   "\xCE\x98"}, {"\\Iota",    "\xCE\x99"},
    {"\\Kappa",    "\xCE\x9A"}, {"\\Lambda",  "\xCE\x9B"}, {"\\Mu",      "\xCE\x9C"},
    {"\\Nu",       "\xCE\x9D"}, {"\\Xi",      "\xCE\x9E"}, {"\\Pi",      "\xCE\xA0"},
    {"\\Rho",      "\xCE\xA1"}, {"\\Sigma",   "\xCE\xA3"}, {"\\Tau",     "\xCE\xA4"},
    {"\\Upsilon",  "\xCE\xA5"}, {"\\Phi",     "\xCE\xA6"}, {"\\Chi",     "\xCE\xA7"},
    {"\\Psi",      "\xCE\xA8"}, {"\\Omega",   "\xCE\xA9"},
    /* Math operators & symbols */
    {"\\cdot",     "\xC2\xB7"},     {"\\times",    "\xC3\x97"},
    {"\\div",      "\xC3\xB7"},     {"\\pm",       "\xC2\xB1"},
    {"\\mp",       "\xE2\x88\x93"}, {"\\leq",      "\xE2\x89\xA4"},
    {"\\geq",      "\xE2\x89\xA5"}, {"\\neq",      "\xE2\x89\xA0"},
    {"\\approx",   "\xE2\x89\x88"}, {"\\equiv",    "\xE2\x89\xA1"},
    {"\\sim",      "\xE2\x88\xBC"}, {"\\propto",   "\xE2\x88\x9D"},
    {"\\infty",    "\xE2\x88\x9E"}, {"\\partial",  "\xE2\x88\x82"},
    {"\\nabla",    "\xE2\x88\x87"}, {"\\forall",   "\xE2\x88\x80"},
    {"\\exists",   "\xE2\x88\x83"}, {"\\in",       "\xE2\x88\x88"},
    {"\\notin",    "\xE2\x88\x89"}, {"\\subset",   "\xE2\x8A\x82"},
    {"\\supset",   "\xE2\x8A\x83"}, {"\\cup",      "\xE2\x88\xAA"},
    {"\\cap",      "\xE2\x88\xA9"}, {"\\emptyset", "\xE2\x88\x85"},
    {"\\int",      "\xE2\x88\xAB"}, {"\\sum",      "\xE2\x88\x91"},
    {"\\prod",     "\xE2\x88\x8F"}, {"\\sqrt",     "\xE2\x88\x9A"},
    {"\\langle",   "\xE2\x9F\xA8"}, {"\\rangle",   "\xE2\x9F\xA9"},
    {"\\leftarrow",  "\xE2\x86\x90"}, {"\\rightarrow", "\xE2\x86\x92"},
    {"\\Leftarrow",  "\xE2\x87\x90"}, {"\\Rightarrow", "\xE2\x87\x92"},
    {"\\leftrightarrow", "\xE2\x86\x94"},
    {"\\to",       "\xE2\x86\x92"}, {"\\gets",     "\xE2\x86\x90"},
    {"\\ldots",    "\xE2\x80\xA6"}, {"\\cdots",    "\xE2\x8B\xAF"},
    {"\\vdots",    "\xE2\x8B\xAE"},
    {"\\neg",      "\xC2\xAC"},     {"\\land",     "\xE2\x88\xA7"},
    {"\\lor",      "\xE2\x88\xA8"},
    {"\\le",       "\xE2\x89\xA4"}, {"\\ge",       "\xE2\x89\xA5"},
    {"\\ne",       "\xE2\x89\xA0"},
    {NULL, NULL}
};

/* Unicode superscript digits */
static const char *SUPER_DIGITS[] = {
    "\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3",
    "\xE2\x81\xB4", "\xE2\x81\xB5", "\xE2\x81\xB6", "\xE2\x81\xB7",
    "\xE2\x81\xB8", "\xE2\x81\xB9"
};

/* Unicode subscript digits */
static const char *SUB_DIGITS[] = {
    "\xE2\x82\x80", "\xE2\x82\x81", "\xE2\x82\x82", "\xE2\x82\x83",
    "\xE2\x82\x84", "\xE2\x82\x85", "\xE2\x82\x86", "\xE2\x82\x87",
    "\xE2\x82\x88", "\xE2\x82\x89"
};

/* Replace LaTeX symbols in-place within a buffer */
static void latex_replace_symbols(char *buf, int bufsize) {
    char tmp[MD_LINE_MAX];
    char *out = tmp;
    char *end = tmp + sizeof(tmp) - 16;
    const char *p = buf;

    while (*p && out < end) {
        if (*p == '\\') {
            bool found = false;
            for (int i = 0; LATEX_SYMBOLS[i].tex; i++) {
                int n = str_starts(p, LATEX_SYMBOLS[i].tex);
                if (n > 0) {
                    /* Ensure it's a complete token: next char must not be alpha */
                    if (!isalpha((unsigned char)p[n])) {
                        int slen = (int)strlen(LATEX_SYMBOLS[i].utf8);
                        if (out + slen < end) {
                            memcpy(out, LATEX_SYMBOLS[i].utf8, slen);
                            out += slen;
                        }
                        p += n;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) *out++ = *p++;
        } else if (*p == '^' && p[1] == '{') {
            /* ^{...} superscript */
            p += 2;
            while (*p && *p != '}' && out < end) {
                if (*p >= '0' && *p <= '9') {
                    const char *s = SUPER_DIGITS[*p - '0'];
                    int slen = (int)strlen(s);
                    if (out + slen < end) { memcpy(out, s, slen); out += slen; }
                } else if (*p == 'n') {
                    /* superscript n */
                    const char *s = "\xE2\x81\xBF";
                    int slen = (int)strlen(s);
                    if (out + slen < end) { memcpy(out, s, slen); out += slen; }
                } else {
                    *out++ = *p;
                }
                p++;
            }
            if (*p == '}') p++;
        } else if (*p == '^' && p[1] >= '0' && p[1] <= '9') {
            /* ^N single digit superscript */
            p++;
            const char *s = SUPER_DIGITS[*p - '0'];
            int slen = (int)strlen(s);
            if (out + slen < end) { memcpy(out, s, slen); out += slen; }
            p++;
        } else if (*p == '_' && p[1] == '{') {
            /* _{...} subscript */
            p += 2;
            while (*p && *p != '}' && out < end) {
                if (*p >= '0' && *p <= '9') {
                    const char *s = SUB_DIGITS[*p - '0'];
                    int slen = (int)strlen(s);
                    if (out + slen < end) { memcpy(out, s, slen); out += slen; }
                } else {
                    *out++ = *p;
                }
                p++;
            }
            if (*p == '}') p++;
        } else if (*p == '_' && p[1] >= '0' && p[1] <= '9') {
            p++;
            const char *s = SUB_DIGITS[*p - '0'];
            int slen = (int)strlen(s);
            if (out + slen < end) { memcpy(out, s, slen); out += slen; }
            p++;
        } else if (str_starts(p, "\\frac{") == 6) {
            /* \frac{a}{b} → a/b */
            p += 6;
            while (*p && *p != '}' && out < end) *out++ = *p++;
            if (*p == '}') p++;
            if (*p == '{') p++;
            if (out < end) *out++ = '/';
            while (*p && *p != '}' && out < end) *out++ = *p++;
            if (*p == '}') p++;
        } else if (str_starts(p, "\\text{") == 6 || str_starts(p, "\\mathrm{") == 8 ||
                   str_starts(p, "\\mathbf{") == 8 || str_starts(p, "\\mathit{") == 8 ||
                   str_starts(p, "\\textbf{") == 7 || str_starts(p, "\\textit{") == 7) {
            /* \text{...} → just the content */
            while (*p && *p != '{') p++;
            if (*p == '{') p++;
            while (*p && *p != '}' && out < end) *out++ = *p++;
            if (*p == '}') p++;
        } else if (*p == '{' || *p == '}') {
            /* Skip bare braces in math mode */
            p++;
        } else if (str_starts(p, "\\left") == 5 || str_starts(p, "\\right") == 6 ||
                   str_starts(p, "\\bigl") == 5 || str_starts(p, "\\bigr") == 5 ||
                   str_starts(p, "\\Bigl") == 5 || str_starts(p, "\\Bigr") == 5) {
            /* Skip sizing commands, output the delimiter */
            while (*p && *p != '(' && *p != ')' && *p != '[' && *p != ']' &&
                   *p != '|' && *p != '{' && *p != '}' && *p != '.' && !isspace((unsigned char)*p))
                p++;
            if (*p == '.') p++; /* \right. → nothing */
            else if (*p && out < end) *out++ = *p++;
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    int len = (int)(out - tmp);
    if (len < bufsize) {
        memcpy(buf, tmp, len + 1);
    }
}

/* ── Inline Markdown Rendering ────────────────────────────────────────── */

/* Render inline markdown formatting to ANSI.
 * Handles: **bold**, *italic*, `code`, ~~strike~~, [links](url), $latex$
 * Also handles basic HTML inline: <b>, <i>, <code>, <em>, <strong>, <a>, <s>
 */
/* ── Emoji shortcode table ────────────────────────────────────────────── */

typedef struct { const char *code; const char *emoji; } emoji_entry_t;

static const emoji_entry_t EMOJI_TABLE[] = {
    {":rocket:",        "\xF0\x9F\x9A\x80"}, {":star:",          "\xE2\xAD\x90"},
    {":fire:",          "\xF0\x9F\x94\xA5"}, {":heart:",         "\xE2\x9D\xA4\xEF\xB8\x8F"},
    {":smile:",         "\xF0\x9F\x98\x84"}, {":laughing:",      "\xF0\x9F\x98\x86"},
    {":wink:",          "\xF0\x9F\x98\x89"}, {":grin:",          "\xF0\x9F\x98\x81"},
    {":cry:",           "\xF0\x9F\x98\xA2"}, {":thinking:",      "\xF0\x9F\xA4\x94"},
    {":thumbsup:",      "\xF0\x9F\x91\x8D"}, {":thumbsdown:",    "\xF0\x9F\x91\x8E"},
    {":clap:",          "\xF0\x9F\x91\x8F"}, {":wave:",          "\xF0\x9F\x91\x8B"},
    {":pray:",          "\xF0\x9F\x99\x8F"}, {":100:",           "\xF0\x9F\x92\xAF"},
    {":tada:",          "\xF0\x9F\x8E\x89"}, {":sparkles:",      "\xE2\x9C\xA8"},
    {":zap:",           "\xE2\x9A\xA1"},     {":boom:",          "\xF0\x9F\x92\xA5"},
    {":bulb:",          "\xF0\x9F\x92\xA1"}, {":memo:",          "\xF0\x9F\x93\x9D"},
    {":wrench:",        "\xF0\x9F\x94\xA7"}, {":hammer:",        "\xF0\x9F\x94\xA8"},
    {":gear:",          "\xE2\x9A\x99\xEF\xB8\x8F"}, {":link:",  "\xF0\x9F\x94\x97"},
    {":lock:",          "\xF0\x9F\x94\x92"}, {":key:",           "\xF0\x9F\x94\x91"},
    {":warning:",       "\xE2\x9A\xA0\xEF\xB8\x8F"}, {":x:",    "\xE2\x9D\x8C"},
    {":white_check_mark:", "\xE2\x9C\x85"}, {":heavy_check_mark:", "\xE2\x9C\x94\xEF\xB8\x8F"},
    {":arrow_right:",   "\xE2\x9E\xA1\xEF\xB8\x8F"}, {":arrow_left:", "\xE2\xAC\x85\xEF\xB8\x8F"},
    {":point_right:",   "\xF0\x9F\x91\x89"}, {":point_left:",    "\xF0\x9F\x91\x88"},
    {":eyes:",          "\xF0\x9F\x91\x80"}, {":brain:",         "\xF0\x9F\xA7\xA0"},
    {":computer:",      "\xF0\x9F\x92\xBB"}, {":package:",       "\xF0\x9F\x93\xA6"},
    {":bug:",           "\xF0\x9F\x90\x9B"}, {":chart_with_upwards_trend:", "\xF0\x9F\x93\x88"},
    {":globe_with_meridians:", "\xF0\x9F\x8C\x90"},
    {":shield:",        "\xF0\x9F\x9B\xA1\xEF\xB8\x8F"},
    {":books:",         "\xF0\x9F\x93\x9A"}, {":pencil:",        "\xE2\x9C\x8F\xEF\xB8\x8F"},
    {":mag:",           "\xF0\x9F\x94\x8D"}, {":clipboard:",     "\xF0\x9F\x93\x8B"},
    {":check:",         "\xE2\x9C\x94\xEF\xB8\x8F"}, {":cross:","\xE2\x9C\x96\xEF\xB8\x8F"},
    {":plus:",          "\xE2\x9E\x95"},     {":minus:",         "\xE2\x9E\x96"},
    {":question:",      "\xE2\x9D\x93"},     {":exclamation:",   "\xE2\x9D\x97"},
    {":info:",          "\xE2\x84\xB9\xEF\xB8\x8F"},
    {":coffee:",        "\xE2\x98\x95"},     {":beer:",          "\xF0\x9F\x8D\xBA"},
    {":pizza:",         "\xF0\x9F\x8D\x95"}, {":trophy:",        "\xF0\x9F\x8F\x86"},
    {":muscle:",        "\xF0\x9F\x92\xAA"}, {":ok_hand:",       "\xF0\x9F\x91\x8C"},
    {":raised_hands:",  "\xF0\x9F\x99\x8C"}, {":v:",             "\xE2\x9C\x8C\xEF\xB8\x8F"},
    {":sunglasses:",    "\xF0\x9F\x98\x8E"}, {":sweat_smile:",   "\xF0\x9F\x98\x85"},
    {":joy:",           "\xF0\x9F\x98\x82"}, {":sob:",           "\xF0\x9F\x98\xAD"},
    {":angry:",         "\xF0\x9F\x98\xA0"}, {":scream:",        "\xF0\x9F\x98\xB1"},
    {":skull:",         "\xF0\x9F\x92\x80"}, {":ghost:",         "\xF0\x9F\x91\xBB"},
    {":alien:",         "\xF0\x9F\x91\xBD"}, {":robot:",         "\xF0\x9F\xA4\x96"},
    {":earth_americas:","\xF0\x9F\x8C\x8E"}, {":sun:",           "\xE2\x98\x80\xEF\xB8\x8F"},
    {":moon:",          "\xF0\x9F\x8C\x99"}, {":rainbow:",       "\xF0\x9F\x8C\x88"},
    {":cloud:",         "\xE2\x98\x81\xEF\xB8\x8F"}, {":umbrella:","\xE2\x98\x82\xEF\xB8\x8F"},
    {":snowflake:",     "\xE2\x9D\x84\xEF\xB8\x8F"}, {":art:",   "\xF0\x9F\x8E\xA8"},
    {":musical_note:",  "\xF0\x9F\x8E\xB5"}, {":movie_camera:",  "\xF0\x9F\x8E\xA5"},
    {":phone:",         "\xF0\x9F\x93\xB1"}, {":email:",         "\xF0\x9F\x93\xA7"},
    {":calendar:",      "\xF0\x9F\x93\x85"}, {":clock:",         "\xF0\x9F\x95\x90"},
    {":hourglass:",     "\xE2\x8C\x9B"},     {":battery:",       "\xF0\x9F\x94\x8B"},
    {"+1:",             "\xF0\x9F\x91\x8D"}, {"-1:",             "\xF0\x9F\x91\x8E"},
    {NULL, NULL}
};

static void render_inline(FILE *out, const char *text) {
    const char *p = text;
    while (*p) {
        /* Escaped character — CommonMark allows escaping any ASCII punctuation */
        if (*p == '\\' && p[1] && strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", p[1])) {
            fputc(p[1], out);
            p += 2;
            continue;
        }

        /* Hard line break: backslash at end of line */
        if (*p == '\\' && (p[1] == '\n' || p[1] == '\0')) {
            fputc('\n', out);
            p += (*p == '\\' && p[1] == '\n') ? 2 : 1;
            continue;
        }

        /* Inline code: multi-backtick code spans `` `code` `` */
        if (*p == '`') {
            /* Count opening backticks */
            int ticks = 0;
            const char *start = p;
            while (p[ticks] == '`') ticks++;
            /* Find matching closing backticks of same count */
            const char *end = start + ticks;
            while (*end) {
                int closing = 0;
                while (end[closing] == '`') closing++;
                if (closing == ticks) {
                    /* Strip one leading/trailing space if both present */
                    const char *cs = start + ticks;
                    int clen = (int)(end - cs);
                    if (clen >= 2 && cs[0] == ' ' && cs[clen-1] == ' ') {
                        cs++; clen -= 2;
                    }
                    fprintf(out, "%s%s", TUI_BG256(236), TUI_BCYAN);
                    fwrite(cs, 1, clen, out);
                    fprintf(out, "%s", TUI_RESET);
                    p = end + ticks;
                    continue;
                }
                end += closing > 0 ? closing : 1;
            }
            /* No match — output literal backticks */
            for (int i = 0; i < ticks; i++) fputc('`', out);
            p = start + ticks;
            continue;
        }

        /* Bold + Italic: ***...*** or ___...___ */
        if ((p[0] == '*' && p[1] == '*' && p[2] == '*') ||
            (p[0] == '_' && p[1] == '_' && p[2] == '_')) {
            char marker = p[0];
            const char *end = p + 3;
            while (*end) {
                if (end[0] == marker && end[1] == marker && end[2] == marker) break;
                end++;
            }
            if (*end) {
                fprintf(out, "%s%s", TUI_BOLD, TUI_ITALIC);
                /* Recurse on inner content */
                char inner[MD_LINE_MAX];
                int n = (int)(end - p - 3);
                if (n >= (int)sizeof(inner)) n = (int)sizeof(inner) - 1;
                memcpy(inner, p + 3, n);
                inner[n] = '\0';
                render_inline(out, inner);
                fprintf(out, "%s", TUI_RESET);
                p = end + 3;
                continue;
            }
        }

        /* Bold: **...** or __...__ */
        if ((p[0] == '*' && p[1] == '*' && p[2] != '*') ||
            (p[0] == '_' && p[1] == '_' && p[2] != '_')) {
            char marker = p[0];
            const char *end = p + 2;
            while (*end) {
                if (end[0] == marker && end[1] == marker) break;
                end++;
            }
            if (*end) {
                fprintf(out, "%s", TUI_BOLD);
                char inner[MD_LINE_MAX];
                int n = (int)(end - p - 2);
                if (n >= (int)sizeof(inner)) n = (int)sizeof(inner) - 1;
                memcpy(inner, p + 2, n);
                inner[n] = '\0';
                render_inline(out, inner);
                fprintf(out, "%s", TUI_RESET);
                p = end + 2;
                continue;
            }
        }

        /* Italic: *...* or _..._ (single) */
        if ((*p == '*' || *p == '_') && p[1] != *p && p[1] != ' ') {
            char marker = *p;
            const char *end = p + 1;
            while (*end && *end != '\n') {
                if (*end == marker && (end == p + 1 || *(end-1) != ' ')) break;
                end++;
            }
            if (*end == marker && end > p + 1) {
                fprintf(out, "%s", TUI_ITALIC);
                char inner[MD_LINE_MAX];
                int n = (int)(end - p - 1);
                if (n >= (int)sizeof(inner)) n = (int)sizeof(inner) - 1;
                memcpy(inner, p + 1, n);
                inner[n] = '\0';
                render_inline(out, inner);
                fprintf(out, "%s", TUI_RESET);
                p = end + 1;
                continue;
            }
        }

        /* Strikethrough: ~~...~~ */
        if (p[0] == '~' && p[1] == '~') {
            const char *end = strstr(p + 2, "~~");
            if (end) {
                fprintf(out, "%s", TUI_STRIKE);
                fwrite(p + 2, 1, end - p - 2, out);
                fprintf(out, "%s", TUI_RESET);
                p = end + 2;
                continue;
            }
        }

        /* Link: [text](url) */
        if (*p == '[') {
            const char *close = strchr(p + 1, ']');
            if (close && close[1] == '(') {
                const char *url_end = strchr(close + 2, ')');
                if (url_end) {
                    /* Render link text underlined + blue */
                    fprintf(out, "%s%s", TUI_UNDERLINE, TUI_BBLUE);
                    fwrite(p + 1, 1, close - p - 1, out);
                    fprintf(out, "%s", TUI_RESET);
                    /* Show URL dimmed in parens */
                    fprintf(out, "%s (", TUI_DIM);
                    fwrite(close + 2, 1, url_end - close - 2, out);
                    fprintf(out, ")%s", TUI_RESET);
                    p = url_end + 1;
                    continue;
                }
            }
        }

        /* Image: ![alt](url) - show alt text as description */
        if (*p == '!' && p[1] == '[') {
            const char *close = strchr(p + 2, ']');
            if (close && close[1] == '(') {
                const char *url_end = strchr(close + 2, ')');
                if (url_end) {
                    fprintf(out, "%s[image: ", TUI_DIM);
                    fwrite(p + 2, 1, close - p - 2, out);
                    fprintf(out, "]%s", TUI_RESET);
                    p = url_end + 1;
                    continue;
                }
            }
        }

        /* Inline LaTeX: $...$ */
        if (*p == '$' && p[1] != '$') {
            const char *end = strchr(p + 1, '$');
            if (end && end > p + 1) {
                char latex[MD_LINE_MAX];
                int n = (int)(end - p - 1);
                if (n >= (int)sizeof(latex)) n = (int)sizeof(latex) - 1;
                memcpy(latex, p + 1, n);
                latex[n] = '\0';
                latex_replace_symbols(latex, sizeof(latex));
                fprintf(out, "%s%s%s", TUI_ITALIC, latex, TUI_RESET);
                p = end + 1;
                continue;
            }
        }

        /* Display LaTeX: $$...$$ */
        if (p[0] == '$' && p[1] == '$') {
            const char *end = strstr(p + 2, "$$");
            if (end) {
                char latex[MD_LINE_MAX];
                int n = (int)(end - p - 2);
                if (n >= (int)sizeof(latex)) n = (int)sizeof(latex) - 1;
                memcpy(latex, p + 2, n);
                latex[n] = '\0';
                latex_replace_symbols(latex, sizeof(latex));
                fprintf(out, "\n  %s%s%s%s\n", TUI_BOLD, TUI_BMAGENTA, latex, TUI_RESET);
                p = end + 2;
                continue;
            }
        }

        /* HTML inline tags & autolinks */
        if (*p == '<') {
            /* Autolinks FIRST: <https://...> or <email@...> */
            if (str_starts(p + 1, "http://") || str_starts(p + 1, "https://") ||
                str_starts(p + 1, "ftp://") || str_starts(p + 1, "mailto:")) {
                const char *gt = strchr(p + 1, '>');
                if (gt) {
                    fprintf(out, "%s%s", TUI_UNDERLINE, TUI_BBLUE);
                    fwrite(p + 1, 1, gt - p - 1, out);
                    fprintf(out, "%s", TUI_RESET);
                    p = gt + 1;
                    continue;
                }
            }
            /* Email autolink: <user@domain> (no spaces, has @) */
            {
                const char *at_pos = NULL;
                const char *scan = p + 1;
                bool valid_email = true;
                while (*scan && *scan != '>' && *scan != ' ' && *scan != '<') {
                    if (*scan == '@') at_pos = scan;
                    scan++;
                }
                if (at_pos && *scan == '>' && valid_email) {
                    fprintf(out, "%s%s", TUI_UNDERLINE, TUI_BBLUE);
                    fwrite(p + 1, 1, scan - p - 1, out);
                    fprintf(out, "%s", TUI_RESET);
                    p = scan + 1;
                    continue;
                }
            }
            /* <b> / <strong> */
            if (str_starts(p, "<b>") || str_starts(p, "<strong>")) {
                int skip = (p[1] == 'b') ? 3 : 8;
                const char *close = (p[1] == 'b') ? strstr(p, "</b>") : strstr(p, "</strong>");
                if (close) {
                    fprintf(out, "%s", TUI_BOLD);
                    char inner[MD_LINE_MAX];
                    int n = (int)(close - p - skip);
                    if (n >= (int)sizeof(inner)) n = (int)sizeof(inner) - 1;
                    memcpy(inner, p + skip, n);
                    inner[n] = '\0';
                    render_inline(out, inner);
                    fprintf(out, "%s", TUI_RESET);
                    p = close + ((p[1] == 'b') ? 4 : 9);
                    continue;
                }
            }
            /* <i> / <em> */
            if (str_starts(p, "<i>") || str_starts(p, "<em>")) {
                int skip = (p[1] == 'i') ? 3 : 4;
                const char *close = (p[1] == 'i') ? strstr(p, "</i>") : strstr(p, "</em>");
                if (close) {
                    fprintf(out, "%s", TUI_ITALIC);
                    char inner[MD_LINE_MAX];
                    int n = (int)(close - p - skip);
                    if (n >= (int)sizeof(inner)) n = (int)sizeof(inner) - 1;
                    memcpy(inner, p + skip, n);
                    inner[n] = '\0';
                    render_inline(out, inner);
                    fprintf(out, "%s", TUI_RESET);
                    p = close + ((p[1] == 'i') ? 4 : 5);
                    continue;
                }
            }
            /* <code> */
            if (str_starts(p, "<code>")) {
                const char *close = strstr(p + 6, "</code>");
                if (close) {
                    fprintf(out, "%s%s", TUI_BG256(236), TUI_BCYAN);
                    fwrite(p + 6, 1, close - p - 6, out);
                    fprintf(out, "%s", TUI_RESET);
                    p = close + 7;
                    continue;
                }
            }
            /* <s> / <del> / <strike> */
            if (str_starts(p, "<s>") || str_starts(p, "<del>") || str_starts(p, "<strike>")) {
                int skip;
                const char *close;
                if (p[1] == 's' && p[2] == '>') { skip = 3; close = strstr(p, "</s>"); }
                else if (p[1] == 'd') { skip = 5; close = strstr(p, "</del>"); }
                else { skip = 8; close = strstr(p, "</strike>"); }
                if (close) {
                    fprintf(out, "%s", TUI_STRIKE);
                    fwrite(p + skip, 1, close - p - skip, out);
                    fprintf(out, "%s", TUI_RESET);
                    p = close + skip + 1; /* +1 for / */
                    /* Advance past the closing tag */
                    while (*p && *p != '>') p++;
                    if (*p == '>') p++;
                    continue;
                }
            }
            /* <u> */
            if (str_starts(p, "<u>")) {
                const char *close = strstr(p + 3, "</u>");
                if (close) {
                    fprintf(out, "%s", TUI_UNDERLINE);
                    fwrite(p + 3, 1, close - p - 3, out);
                    fprintf(out, "%s", TUI_RESET);
                    p = close + 4;
                    continue;
                }
            }
            /* <a href="url">text</a> */
            if (str_starts(p, "<a ")) {
                const char *href = strstr(p, "href=\"");
                const char *close_tag = strchr(p, '>');
                const char *end_a = strstr(p, "</a>");
                if (href && close_tag && end_a && href < close_tag) {
                    const char *url_start = href + 6;
                    const char *url_end = strchr(url_start, '"');
                    if (url_end && url_end < close_tag) {
                        fprintf(out, "%s%s", TUI_UNDERLINE, TUI_BBLUE);
                        fwrite(close_tag + 1, 1, end_a - close_tag - 1, out);
                        fprintf(out, "%s", TUI_RESET);
                        fprintf(out, "%s (", TUI_DIM);
                        fwrite(url_start, 1, url_end - url_start, out);
                        fprintf(out, ")%s", TUI_RESET);
                        p = end_a + 4;
                        continue;
                    }
                }
            }
            /* <br> / <br/> / <br /> */
            if (str_starts(p, "<br") && (p[3] == '>' || p[3] == '/' || p[3] == ' ')) {
                fputc('\n', out);
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                continue;
            }
            /* <sup> / <sub> */
            if (str_starts(p, "<sup>")) {
                const char *close = strstr(p + 5, "</sup>");
                if (close) {
                    /* Try to render as unicode superscript for digits */
                    for (const char *c = p + 5; c < close; c++) {
                        if (*c >= '0' && *c <= '9')
                            fputs(SUPER_DIGITS[*c - '0'], out);
                        else
                            fputc(*c, out);
                    }
                    p = close + 6;
                    continue;
                }
            }
            if (str_starts(p, "<sub>")) {
                const char *close = strstr(p + 5, "</sub>");
                if (close) {
                    for (const char *c = p + 5; c < close; c++) {
                        if (*c >= '0' && *c <= '9')
                            fputs(SUB_DIGITS[*c - '0'], out);
                        else
                            fputc(*c, out);
                    }
                    p = close + 6;
                    continue;
                }
            }
            /* <mark> / <kbd> */
            if (str_starts(p, "<mark>")) {
                const char *close = strstr(p + 6, "</mark>");
                if (close) {
                    fprintf(out, "%s%s", TUI_BG_YELLOW, TUI_BLACK);
                    char inner[MD_LINE_MAX];
                    int n = (int)(close - p - 6);
                    if (n >= (int)sizeof(inner)) n = (int)sizeof(inner) - 1;
                    memcpy(inner, p + 6, n);
                    inner[n] = '\0';
                    render_inline(out, inner);
                    fprintf(out, "%s", TUI_RESET);
                    p = close + 7;
                    continue;
                }
            }
            if (str_starts(p, "<kbd>")) {
                const char *close = strstr(p + 5, "</kbd>");
                if (close) {
                    fprintf(out, "%s%s ", TUI_BG256(236), TUI_BWHITE);
                    fwrite(p + 5, 1, close - p - 5, out);
                    fprintf(out, " %s", TUI_RESET);
                    p = close + 6;
                    continue;
                }
            }
            /* <h1>-<h6> inline (in HTML blocks) */
            if (p[1] == 'h' && p[2] >= '1' && p[2] <= '6' && p[3] == '>') {
                int level = p[2] - '0';
                char close_tag[8];
                snprintf(close_tag, sizeof(close_tag), "</h%c>", p[2]);
                const char *close = strstr(p + 4, close_tag);
                if (close) {
                    const char *hcolors[] = {
                        TUI_BWHITE, TUI_BCYAN, TUI_BBLUE, TUI_BMAGENTA, TUI_DIM, TUI_DIM
                    };
                    fprintf(out, "%s%s", TUI_BOLD, hcolors[level-1]);
                    if (level == 1) fprintf(out, "%s", TUI_UNDERLINE);
                    char inner[MD_LINE_MAX];
                    int n = (int)(close - p - 4);
                    if (n >= (int)sizeof(inner)) n = (int)sizeof(inner) - 1;
                    memcpy(inner, p + 4, n);
                    inner[n] = '\0';
                    render_inline(out, inner);
                    fprintf(out, "%s", TUI_RESET);
                    p = close + 5;
                    continue;
                }
            }
            /* <hr> / <hr/> */
            if (str_starts(p, "<hr") && (p[3] == '>' || p[3] == '/' || p[3] == ' ')) {
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                fprintf(out, "\n  %s", TUI_DIM);
                for (int i = 0; i < 56; i++) fprintf(out, "─");
                fprintf(out, "%s\n", TUI_RESET);
                continue;
            }
            /* <p> → newline */
            if (str_starts(p, "<p>") || str_starts(p, "<p ")) {
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                fprintf(out, "\n");
                continue;
            }
            if (str_starts(p, "</p>")) { p += 4; fprintf(out, "\n"); continue; }
            /* <li> in HTML */
            if (str_starts(p, "<li>")) {
                p += 4;
                fprintf(out, "  %s•%s ", TUI_BCYAN, TUI_RESET);
                continue;
            }
            if (str_starts(p, "</li>")) { p += 5; fprintf(out, "\n"); continue; }
            /* <img src="..." alt="..."> */
            if (str_starts(p, "<img ")) {
                const char *alt = strstr(p, "alt=\"");
                const char *gt = strchr(p, '>');
                if (alt && gt) {
                    const char *astart = alt + 5;
                    const char *aend = strchr(astart, '"');
                    if (aend && aend < gt) {
                        fprintf(out, "%s[image: ", TUI_DIM);
                        fwrite(astart, 1, aend - astart, out);
                        fprintf(out, "]%s", TUI_RESET);
                    }
                }
                if (gt) p = gt + 1; else p++;
                continue;
            }
            /* Skip other HTML tags */
            if (p[1] == '/' || isalpha((unsigned char)p[1])) {
                const char *gt = strchr(p + 1, '>');
                if (gt && (gt - p) < 64) {
                    p = gt + 1;
                    continue;
                }
            }
        }

        /* ==highlight== (GFM extension) */
        if (p[0] == '=' && p[1] == '=') {
            const char *end = strstr(p + 2, "==");
            if (end && end > p + 2) {
                fprintf(out, "%s%s", TUI_BG_YELLOW, TUI_BLACK);
                char inner[MD_LINE_MAX];
                int n = (int)(end - p - 2);
                if (n >= (int)sizeof(inner)) n = (int)sizeof(inner) - 1;
                memcpy(inner, p + 2, n);
                inner[n] = '\0';
                render_inline(out, inner);
                fprintf(out, "%s", TUI_RESET);
                p = end + 2;
                continue;
            }
        }

        /* Footnote reference: [^id] */
        if (p[0] == '[' && p[1] == '^') {
            const char *close = strchr(p + 2, ']');
            if (close && (close - p) < 64) {
                /* Check it's NOT a definition (no trailing :) */
                if (close[1] != ':') {
                    fprintf(out, "%s%s[", TUI_BOLD, TUI_BCYAN);
                    fwrite(p + 1, 1, close - p - 1, out);
                    fprintf(out, "]%s", TUI_RESET);
                    p = close + 1;
                    continue;
                }
            }
        }

        /* Emoji shortcodes :name: */
        if (*p == ':' && (isalpha((unsigned char)p[1]) || isdigit((unsigned char)p[1]))) {
            const char *end = strchr(p + 1, ':');
            if (end && (end - p) < 40) {
                /* Check all chars between are valid shortcode chars */
                bool valid = true;
                for (const char *c = p + 1; c < end; c++) {
                    if (!isalnum((unsigned char)*c) && *c != '_' && *c != '+' && *c != '-') {
                        valid = false; break;
                    }
                }
                if (valid) {
                    char code[48];
                    int clen = (int)(end - p + 1);
                    if (clen < (int)sizeof(code)) {
                        memcpy(code, p, clen);
                        code[clen] = '\0';
                        bool found = false;
                        for (int i = 0; EMOJI_TABLE[i].code; i++) {
                            if (strcmp(code, EMOJI_TABLE[i].code) == 0) {
                                fputs(EMOJI_TABLE[i].emoji, out);
                                p = end + 1;
                                found = true;
                                break;
                            }
                        }
                        if (found) continue;
                    }
                }
            }
        }

        /* GFM bare URL autolink */
        if ((str_starts(p, "https://") || str_starts(p, "http://")) &&
            (p == text || *(p-1) == ' ' || *(p-1) == '(' || *(p-1) == '\t')) {
            const char *end = p;
            while (*end && *end != ' ' && *end != '\t' && *end != '\n' &&
                   *end != ')' && *end != '>' && *end != '"' && *end != '\'')
                end++;
            /* Strip trailing punctuation */
            while (end > p && (*(end-1) == '.' || *(end-1) == ',' || *(end-1) == ';' ||
                               *(end-1) == '!' || *(end-1) == '?'))
                end--;
            fprintf(out, "%s%s", TUI_UNDERLINE, TUI_BBLUE);
            fwrite(p, 1, end - p, out);
            fprintf(out, "%s", TUI_RESET);
            p = end;
            continue;
        }

        /* &entity; HTML entities — named, decimal, hex */
        if (*p == '&') {
            /* Named entities */
            if (str_starts(p, "&amp;"))  { fputc('&', out); p += 5; continue; }
            if (str_starts(p, "&lt;"))   { fputc('<', out); p += 4; continue; }
            if (str_starts(p, "&gt;"))   { fputc('>', out); p += 4; continue; }
            if (str_starts(p, "&quot;")) { fputc('"', out); p += 6; continue; }
            if (str_starts(p, "&apos;")) { fputc('\'', out); p += 6; continue; }
            if (str_starts(p, "&nbsp;")) { fputc(' ', out); p += 6; continue; }
            if (str_starts(p, "&mdash;"))  { fputs("\xE2\x80\x94", out); p += 7; continue; }
            if (str_starts(p, "&ndash;"))  { fputs("\xE2\x80\x93", out); p += 7; continue; }
            if (str_starts(p, "&hellip;")) { fputs("\xE2\x80\xA6", out); p += 8; continue; }
            if (str_starts(p, "&laquo;"))  { fputs("\xC2\xAB", out); p += 7; continue; }
            if (str_starts(p, "&raquo;"))  { fputs("\xC2\xBB", out); p += 7; continue; }
            if (str_starts(p, "&lsquo;"))  { fputs("\xE2\x80\x98", out); p += 7; continue; }
            if (str_starts(p, "&rsquo;"))  { fputs("\xE2\x80\x99", out); p += 7; continue; }
            if (str_starts(p, "&ldquo;"))  { fputs("\xE2\x80\x9C", out); p += 7; continue; }
            if (str_starts(p, "&rdquo;"))  { fputs("\xE2\x80\x9D", out); p += 7; continue; }
            if (str_starts(p, "&bull;"))   { fputs("\xE2\x80\xA2", out); p += 6; continue; }
            if (str_starts(p, "&trade;"))  { fputs("\xE2\x84\xA2", out); p += 7; continue; }
            if (str_starts(p, "&copy;"))   { fputs("\xC2\xA9", out); p += 6; continue; }
            if (str_starts(p, "&reg;"))    { fputs("\xC2\xAE", out); p += 5; continue; }
            if (str_starts(p, "&deg;"))    { fputs("\xC2\xB0", out); p += 5; continue; }
            if (str_starts(p, "&micro;"))  { fputs("\xC2\xB5", out); p += 7; continue; }
            if (str_starts(p, "&para;"))   { fputs("\xC2\xB6", out); p += 6; continue; }
            if (str_starts(p, "&sect;"))   { fputs("\xC2\xA7", out); p += 6; continue; }
            if (str_starts(p, "&euro;"))   { fputs("\xE2\x82\xAC", out); p += 6; continue; }
            if (str_starts(p, "&pound;"))  { fputs("\xC2\xA3", out); p += 7; continue; }
            if (str_starts(p, "&yen;"))    { fputs("\xC2\xA5", out); p += 5; continue; }
            if (str_starts(p, "&cent;"))   { fputs("\xC2\xA2", out); p += 6; continue; }
            if (str_starts(p, "&times;"))  { fputs("\xC3\x97", out); p += 7; continue; }
            if (str_starts(p, "&divide;")) { fputs("\xC3\xB7", out); p += 8; continue; }
            if (str_starts(p, "&plusmn;")) { fputs("\xC2\xB1", out); p += 8; continue; }
            if (str_starts(p, "&frac12;")) { fputs("\xC2\xBD", out); p += 8; continue; }
            if (str_starts(p, "&frac14;")) { fputs("\xC2\xBC", out); p += 8; continue; }
            if (str_starts(p, "&frac34;")) { fputs("\xC2\xBE", out); p += 8; continue; }
            if (str_starts(p, "&larr;"))   { fputs("\xE2\x86\x90", out); p += 6; continue; }
            if (str_starts(p, "&rarr;"))   { fputs("\xE2\x86\x92", out); p += 6; continue; }
            if (str_starts(p, "&uarr;"))   { fputs("\xE2\x86\x91", out); p += 6; continue; }
            if (str_starts(p, "&darr;"))   { fputs("\xE2\x86\x93", out); p += 6; continue; }
            if (str_starts(p, "&harr;"))   { fputs("\xE2\x86\x94", out); p += 6; continue; }
            if (str_starts(p, "&infin;"))  { fputs("\xE2\x88\x9E", out); p += 7; continue; }

            /* Decimal numeric: &#123; */
            if (p[1] == '#' && isdigit((unsigned char)p[2])) {
                unsigned int codepoint = 0;
                const char *d = p + 2;
                while (isdigit((unsigned char)*d)) {
                    codepoint = codepoint * 10 + (*d - '0');
                    d++;
                }
                if (*d == ';' && codepoint > 0 && codepoint <= 0x10FFFF) {
                    /* Encode as UTF-8 */
                    char utf8[5];
                    int ulen = 0;
                    if (codepoint < 0x80) {
                        utf8[ulen++] = (char)codepoint;
                    } else if (codepoint < 0x800) {
                        utf8[ulen++] = (char)(0xC0 | (codepoint >> 6));
                        utf8[ulen++] = (char)(0x80 | (codepoint & 0x3F));
                    } else if (codepoint < 0x10000) {
                        utf8[ulen++] = (char)(0xE0 | (codepoint >> 12));
                        utf8[ulen++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        utf8[ulen++] = (char)(0x80 | (codepoint & 0x3F));
                    } else {
                        utf8[ulen++] = (char)(0xF0 | (codepoint >> 18));
                        utf8[ulen++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        utf8[ulen++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        utf8[ulen++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                    fwrite(utf8, 1, ulen, out);
                    p = d + 1;
                    continue;
                }
            }
            /* Hex numeric: &#x1F600; */
            if (p[1] == '#' && (p[2] == 'x' || p[2] == 'X')) {
                unsigned int codepoint = 0;
                const char *d = p + 3;
                while (isxdigit((unsigned char)*d)) {
                    codepoint <<= 4;
                    if (*d >= '0' && *d <= '9') codepoint |= *d - '0';
                    else if (*d >= 'a' && *d <= 'f') codepoint |= *d - 'a' + 10;
                    else codepoint |= *d - 'A' + 10;
                    d++;
                }
                if (*d == ';' && codepoint > 0 && codepoint <= 0x10FFFF) {
                    char utf8[5];
                    int ulen = 0;
                    if (codepoint < 0x80) {
                        utf8[ulen++] = (char)codepoint;
                    } else if (codepoint < 0x800) {
                        utf8[ulen++] = (char)(0xC0 | (codepoint >> 6));
                        utf8[ulen++] = (char)(0x80 | (codepoint & 0x3F));
                    } else if (codepoint < 0x10000) {
                        utf8[ulen++] = (char)(0xE0 | (codepoint >> 12));
                        utf8[ulen++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        utf8[ulen++] = (char)(0x80 | (codepoint & 0x3F));
                    } else {
                        utf8[ulen++] = (char)(0xF0 | (codepoint >> 18));
                        utf8[ulen++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        utf8[ulen++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        utf8[ulen++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                    fwrite(utf8, 1, ulen, out);
                    p = d + 1;
                    continue;
                }
            }
        }

        /* Hard line break: two trailing spaces (already trimmed, so check original) */
        /* This is handled at line level instead */

        fputc(*p, out);
        p++;
        ; /* end of inline loop */
    }
}

/* ── Table Rendering ──────────────────────────────────────────────────── */

static bool is_table_separator(const char *line) {
    line = str_trim((char *)line);
    if (*line != '|') return false;
    for (const char *p = line; *p; p++) {
        if (*p != '|' && *p != '-' && *p != ':' && *p != ' ')
            return false;
    }
    return true;
}

static int parse_table_row(const char *line, char cells[][256], int max_cols) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '|') p++;

    int col = 0;
    while (*p && col < max_cols) {
        const char *start = p;
        while (*p && *p != '|') p++;

        /* Trim cell content */
        int len = (int)(p - start);
        while (len > 0 && (start[len-1] == ' ' || start[len-1] == '\t')) len--;
        while (len > 0 && (*start == ' ' || *start == '\t')) { start++; len--; }
        if (len >= 256) len = 255;
        memcpy(cells[col], start, len);
        cells[col][len] = '\0';
        col++;

        if (*p == '|') p++;
    }
    return col;
}

/* Compute the visible width of markdown text after rendering (strip formatting markers).
 * This renders to /dev/null-like buffer, counting only visible chars. */
static int md_rendered_width(const char *text) {
    /* Render to a temporary buffer, then measure visible length */
    char buf[MD_LINE_MAX * 2];
    FILE *tmp = fmemopen(buf, sizeof(buf) - 1, "w");
    if (!tmp) return vis_len(text); /* fallback */
    render_inline(tmp, text);
    fflush(tmp);
    long pos = ftell(tmp);
    fclose(tmp);
    if (pos >= 0 && pos < (long)sizeof(buf)) buf[pos] = '\0';
    else buf[0] = '\0';
    return vis_len(buf);
}

static void render_table(FILE *out, md_renderer_t *r) {
    if (r->table_rows == 0) return;

    /* Calculate column widths based on rendered visible length */
    int col_w[MD_TABLE_MAXCOL] = {0};
    for (int row = 0; row < r->table_rows; row++) {
        for (int c = 0; c < r->table_cols; c++) {
            if (r->table_cells[row][c]) {
                int w = md_rendered_width(r->table_cells[row][c]);
                if (w > col_w[c]) col_w[c] = w;
            }
        }
    }

    /* Ensure minimum column widths */
    for (int c = 0; c < r->table_cols; c++) {
        if (col_w[c] < 3) col_w[c] = 3;
    }

    /* Top border */
    fprintf(out, "  %s╭", TUI_DIM);
    for (int c = 0; c < r->table_cols; c++) {
        for (int i = 0; i < col_w[c] + 2; i++) fprintf(out, "─");
        fprintf(out, "%s", c < r->table_cols - 1 ? "┬" : "╮");
    }
    fprintf(out, "%s\n", TUI_RESET);

    for (int row = 0; row < r->table_rows; row++) {
        /* Render row */
        fprintf(out, "  %s│%s", TUI_DIM, TUI_RESET);
        for (int c = 0; c < r->table_cols; c++) {
            const char *cell = (r->table_cells[row][c] && r->table_cells[row][c][0])
                               ? r->table_cells[row][c] : "";
            int cw = md_rendered_width(cell);
            int pad = col_w[c] - cw;

            fprintf(out, " ");
            if (row == 0) {
                /* Header row: bold + colored */
                fprintf(out, "%s%s", TUI_BOLD, TUI_BCYAN);
                render_inline(out, cell);
                fprintf(out, "%s", TUI_RESET);
            } else {
                render_inline(out, cell);
            }
            for (int i = 0; i < pad; i++) fputc(' ', out);
            fprintf(out, " %s│%s", TUI_DIM, TUI_RESET);
        }
        fprintf(out, "\n");

        /* After header row, draw separator */
        if (row == 0) {
            fprintf(out, "  %s├", TUI_DIM);
            for (int c = 0; c < r->table_cols; c++) {
                for (int i = 0; i < col_w[c] + 2; i++) fprintf(out, "─");
                fprintf(out, "%s", c < r->table_cols - 1 ? "┼" : "┤");
            }
            fprintf(out, "%s\n", TUI_RESET);
        }
    }

    /* Bottom border */
    fprintf(out, "  %s╰", TUI_DIM);
    for (int c = 0; c < r->table_cols; c++) {
        for (int i = 0; i < col_w[c] + 2; i++) fprintf(out, "─");
        fprintf(out, "%s", c < r->table_cols - 1 ? "┴" : "╯");
    }
    fprintf(out, "%s\n", TUI_RESET);
}

static void table_free(md_renderer_t *r) {
    for (int row = 0; row < r->table_rows; row++) {
        for (int c = 0; c < r->table_cols; c++) {
            free(r->table_cells[row][c]);
            r->table_cells[row][c] = NULL;
        }
    }
    r->table_rows = 0;
    r->table_cols = 0;
    r->table_has_sep = false;
}

/* ── Code Block Rendering ─────────────────────────────────────────────── */

/* Simple syntax highlighting for common languages */
static void render_code_line(FILE *out, const char *line, const char *lang) {
    /* Basic keyword highlighting */
    bool is_c = (lang && (strcmp(lang, "c") == 0 || strcmp(lang, "cpp") == 0 ||
                          strcmp(lang, "h") == 0 || strcmp(lang, "cc") == 0));
    bool is_py = (lang && (strcmp(lang, "python") == 0 || strcmp(lang, "py") == 0));
    bool is_js = (lang && (strcmp(lang, "javascript") == 0 || strcmp(lang, "js") == 0 ||
                           strcmp(lang, "typescript") == 0 || strcmp(lang, "ts") == 0 ||
                           strcmp(lang, "jsx") == 0 || strcmp(lang, "tsx") == 0));
    bool is_sh = (lang && (strcmp(lang, "bash") == 0 || strcmp(lang, "sh") == 0 ||
                           strcmp(lang, "shell") == 0 || strcmp(lang, "zsh") == 0));
    bool is_rust = (lang && strcmp(lang, "rust") == 0);
    bool is_go = (lang && (strcmp(lang, "go") == 0 || strcmp(lang, "golang") == 0));
    bool is_java = (lang && (strcmp(lang, "java") == 0 || strcmp(lang, "kotlin") == 0));
    bool is_rb = (lang && (strcmp(lang, "ruby") == 0 || strcmp(lang, "rb") == 0));
    bool is_sql = (lang && (strcmp(lang, "sql") == 0));
    bool is_html = (lang && (strcmp(lang, "html") == 0 || strcmp(lang, "xml") == 0));
    bool is_css = (lang && (strcmp(lang, "css") == 0 || strcmp(lang, "scss") == 0));
    bool is_json = (lang && (strcmp(lang, "json") == 0));
    bool is_yaml = (lang && (strcmp(lang, "yaml") == 0 || strcmp(lang, "yml") == 0));
    bool is_md = (lang && (strcmp(lang, "markdown") == 0 || strcmp(lang, "md") == 0));
    bool highlight = is_c || is_py || is_js || is_sh || is_rust || is_go ||
                     is_java || is_rb || is_sql || is_html || is_css ||
                     is_json || is_yaml || is_md;

    if (!highlight) {
        fputs(line, out);
        return;
    }

    const char *p = line;
    while (*p) {
        /* Comments */
        if ((is_c || is_js || is_rust || is_go || is_java || is_css) && p[0] == '/' && p[1] == '/') {
            fprintf(out, "%s%s%s", TUI_DIM, p, TUI_RESET);
            return;
        }
        if (is_c && p[0] == '/' && p[1] == '*') {
            fprintf(out, "%s%s%s", TUI_DIM, p, TUI_RESET);
            return;
        }
        if ((is_py || is_sh || is_rb || is_yaml) && *p == '#') {
            fprintf(out, "%s%s%s", TUI_DIM, p, TUI_RESET);
            return;
        }
        if (is_sql && *p == '-' && p[1] == '-') {
            fprintf(out, "%s%s%s", TUI_DIM, p, TUI_RESET);
            return;
        }
        if (is_html && p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            fprintf(out, "%s%s%s", TUI_DIM, p, TUI_RESET);
            return;
        }

        /* Strings */
        if (*p == '"' || *p == '\'' || (is_py && *p == '\'' )) {
            char quote = *p;
            fprintf(out, "%s%c", TUI_GREEN, quote);
            p++;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    fprintf(out, "%s\\%c%s", TUI_BYELLOW, p[1], TUI_GREEN);
                    p += 2;
                } else {
                    fputc(*p, out);
                    p++;
                }
            }
            if (*p == quote) { fputc(quote, out); p++; }
            fprintf(out, "%s", TUI_RESET);
            continue;
        }

        /* Numbers */
        if (isdigit((unsigned char)*p) &&
            (p == line || !isalpha((unsigned char)*(p-1)))) {
            fprintf(out, "%s", TUI_BYELLOW);
            while (isdigit((unsigned char)*p) || *p == '.' || *p == 'x' || *p == 'X' ||
                   (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
            { fputc(*p, out); p++; }
            if (*p == 'f' || *p == 'l' || *p == 'L' || *p == 'u' || *p == 'U')
            { fputc(*p, out); p++; }
            fprintf(out, "%s", TUI_RESET);
            continue;
        }

        /* C preprocessor */
        if (is_c && *p == '#' && (p == line || *(p-1) == ' ' || *(p-1) == '\t')) {
            fprintf(out, "%s", TUI_BMAGENTA);
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') { fputc(*p, out); p++; }
            fprintf(out, "%s", TUI_RESET);
            continue;
        }

        /* HTML/XML tags */
        if (is_html && *p == '<') {
            fprintf(out, "%s", TUI_BBLUE);
            while (*p && *p != '>') { fputc(*p, out); p++; }
            if (*p == '>') { fputc(*p, out); p++; }
            fprintf(out, "%s", TUI_RESET);
            continue;
        }

        /* CSS properties */
        if (is_css && *p == ':' && p > line) {
            fputc(*p, out);
            p++;
            /* Value part in a different color */
            fprintf(out, "%s", TUI_BCYAN);
            while (*p && *p != ';' && *p != '}') { fputc(*p, out); p++; }
            fprintf(out, "%s", TUI_RESET);
            continue;
        }

        /* JSON keys */
        if (is_json && *p == '"') {
            /* Check if this is a key (followed by :) */
            const char *end = strchr(p + 1, '"');
            if (end) {
                const char *after = end + 1;
                while (*after == ' ' || *after == '\t') after++;
                if (*after == ':') {
                    fprintf(out, "%s", TUI_BCYAN);
                    while (p <= end) { fputc(*p, out); p++; }
                    fprintf(out, "%s", TUI_RESET);
                    continue;
                }
            }
            /* It's a value string */
            fprintf(out, "%s\"", TUI_GREEN);
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    fprintf(out, "%s\\%c%s", TUI_BYELLOW, p[1], TUI_GREEN);
                    p += 2;
                } else {
                    fputc(*p, out);
                    p++;
                }
            }
            if (*p == '"') { fputc('"', out); p++; }
            fprintf(out, "%s", TUI_RESET);
            continue;
        }

        /* YAML keys */
        if (is_yaml && (p == line || *(p-1) == '\n') ) {
            const char *colon = strchr(p, ':');
            if (colon && colon > p) {
                fprintf(out, "%s", TUI_BCYAN);
                while (p <= colon) { fputc(*p, out); p++; }
                fprintf(out, "%s", TUI_RESET);
                continue;
            }
        }

        /* Keywords */
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            int wlen = (int)(p - start);
            char word[64];
            if (wlen < 64) {
                memcpy(word, start, wlen);
                word[wlen] = '\0';
            } else {
                memcpy(word, start, 63);
                word[63] = '\0';
            }

            bool is_kw = false;

            if (is_c) {
                const char *kws[] = {"if","else","for","while","do","switch","case","break",
                    "continue","return","goto","struct","union","enum","typedef",
                    "const","static","extern","volatile","inline","sizeof","void",
                    "int","char","long","short","unsigned","signed","float","double",
                    "bool","true","false","NULL","auto","register","restrict",
                    "#include","#define","#ifdef","#ifndef","#endif","#if","#else","#elif",
                    "class","public","private","protected","virtual","override","template",
                    "namespace","using","new","delete","try","catch","throw",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(word, kws[i]) == 0) { is_kw = true; break; }
                }
            }
            if (is_py) {
                const char *kws[] = {"def","class","if","elif","else","for","while","try",
                    "except","finally","with","as","import","from","return","yield",
                    "raise","pass","break","continue","and","or","not","in","is",
                    "lambda","None","True","False","global","nonlocal","assert","del",
                    "async","await","self","print",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(word, kws[i]) == 0) { is_kw = true; break; }
                }
            }
            if (is_js) {
                const char *kws[] = {"function","const","let","var","if","else","for","while",
                    "do","switch","case","break","continue","return","class","extends",
                    "new","delete","typeof","instanceof","this","super","import","export",
                    "from","default","try","catch","finally","throw","async","await",
                    "yield","true","false","null","undefined","void","of","in",
                    "interface","type","enum","implements","abstract","private","public",
                    "protected","readonly","static","declare","module","namespace",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(word, kws[i]) == 0) { is_kw = true; break; }
                }
            }
            if (is_sh) {
                const char *kws[] = {"if","then","else","elif","fi","for","while","do","done",
                    "case","esac","in","function","return","exit","local","export",
                    "source","echo","printf","read","set","unset","shift","eval","exec",
                    "true","false","test",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(word, kws[i]) == 0) { is_kw = true; break; }
                }
            }
            if (is_rust) {
                const char *kws[] = {"fn","let","mut","const","if","else","for","while","loop",
                    "match","return","break","continue","struct","enum","impl","trait",
                    "pub","use","mod","crate","self","super","Self","type","where",
                    "async","await","move","ref","as","in","true","false","None","Some",
                    "Ok","Err","Box","Vec","String","Option","Result","unsafe","dyn",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(word, kws[i]) == 0) { is_kw = true; break; }
                }
            }
            if (is_go) {
                const char *kws[] = {"func","var","const","if","else","for","range","switch",
                    "case","break","continue","return","struct","interface","type","map",
                    "chan","go","defer","select","package","import","true","false","nil",
                    "append","make","new","len","cap","delete","copy","close","panic",
                    "recover","print","println","error","string","int","bool","byte",
                    "float64","float32",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(word, kws[i]) == 0) { is_kw = true; break; }
                }
            }
            if (is_java) {
                const char *kws[] = {"class","interface","enum","extends","implements","import",
                    "package","public","private","protected","static","final","abstract",
                    "void","int","long","double","float","boolean","char","byte","short",
                    "if","else","for","while","do","switch","case","break","continue",
                    "return","new","this","super","try","catch","finally","throw","throws",
                    "null","true","false","instanceof","synchronized","volatile","transient",
                    "var","val","fun","when","is","in","object","companion","data",
                    "sealed","override","open","lateinit","suspend","coroutine",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(word, kws[i]) == 0) { is_kw = true; break; }
                }
            }
            if (is_rb) {
                const char *kws[] = {"def","class","module","if","elsif","else","unless","for",
                    "while","until","do","end","begin","rescue","ensure","raise","return",
                    "yield","block_given?","require","include","extend","attr_accessor",
                    "attr_reader","attr_writer","self","nil","true","false","and","or",
                    "not","in","then","puts","print","p",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(word, kws[i]) == 0) { is_kw = true; break; }
                }
            }
            if (is_sql) {
                /* Case-insensitive for SQL */
                char upper[64];
                for (int i = 0; i < wlen && i < 63; i++)
                    upper[i] = toupper((unsigned char)word[i]);
                upper[wlen < 63 ? wlen : 63] = '\0';
                const char *kws[] = {"SELECT","FROM","WHERE","INSERT","UPDATE","DELETE",
                    "CREATE","DROP","ALTER","TABLE","INDEX","VIEW","INTO","VALUES",
                    "SET","JOIN","LEFT","RIGHT","INNER","OUTER","ON","AND","OR","NOT",
                    "IN","IS","NULL","AS","ORDER","BY","GROUP","HAVING","LIMIT","OFFSET",
                    "DISTINCT","COUNT","SUM","AVG","MIN","MAX","BETWEEN","LIKE","EXISTS",
                    "UNION","ALL","CASE","WHEN","THEN","ELSE","END","BEGIN","COMMIT",
                    "ROLLBACK","TRANSACTION","PRIMARY","KEY","FOREIGN","REFERENCES",
                    "CASCADE","CONSTRAINT","DEFAULT","CHECK","UNIQUE","IF","WITH",NULL};
                for (int i = 0; kws[i]; i++) {
                    if (strcmp(upper, kws[i]) == 0) { is_kw = true; break; }
                }
            }

            if (is_kw) {
                fprintf(out, "%s%s%s", TUI_BMAGENTA, word, TUI_RESET);
            } else {
                /* Check if it looks like a type (starts with uppercase in most langs) */
                if (isupper((unsigned char)word[0]) && !is_sql && !is_sh) {
                    fprintf(out, "%s%s%s", TUI_BYELLOW, word, TUI_RESET);
                } else {
                    fputs(word, out);
                }
            }
            continue;
        }

        /* Operators */
        if (strchr("=!<>+-*/&|^~%?:", *p)) {
            fprintf(out, "%s%c%s", TUI_CYAN, *p, TUI_RESET);
            p++;
            continue;
        }

        fputc(*p, out);
        p++;
    }
}

static void render_code_block(FILE *out, const char *code, const char *lang, int tw) {
    if (tw <= 0) tw = term_width();
    int box_w = tw - 4;
    if (box_w < 20) box_w = 20;

    /* Language label */
    fprintf(out, "  %s╭─", TUI_DIM);
    if (lang && lang[0]) {
        fprintf(out, " %s%s%s%s ", TUI_RESET, TUI_BCYAN, lang, TUI_DIM);
        int lw = (int)strlen(lang) + 2;
        for (int i = 0; i < box_w - 3 - lw; i++) fprintf(out, "─");
    } else {
        for (int i = 0; i < box_w - 2; i++) fprintf(out, "─");
    }
    fprintf(out, "╮%s\n", TUI_RESET);

    /* Code lines */
    int lineno = 1;
    const char *p = code;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);

        char line[MD_LINE_MAX];
        if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';

        /* Line with gutter */
        fprintf(out, "  %s│%s %s%3d%s %s│%s ",
                TUI_DIM, TUI_RESET,
                TUI_FG256(240), lineno, TUI_RESET,
                TUI_DIM, TUI_RESET);

        render_code_line(out, line, lang);
        fprintf(out, "\n");

        lineno++;
        p = nl ? nl + 1 : p + len;
        if (!nl) break;
    }

    /* Bottom border */
    fprintf(out, "  %s╰", TUI_DIM);
    for (int i = 0; i < box_w; i++) fprintf(out, "─");
    fprintf(out, "╯%s\n", TUI_RESET);
}

/* ── Line-Level Rendering ─────────────────────────────────────────────── */

static void render_line(md_renderer_t *r, char *line) {
    FILE *out = r->out;
    int tw = r->term_width;
    if (tw <= 0) tw = term_width();

    /* Strip trailing whitespace but preserve the string */
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                       line[len-1] == ' ' || line[len-1] == '\t'))
        line[--len] = '\0';

    /* ── Code fence start ── */
    if ((line[0] == '`' && line[1] == '`' && line[2] == '`') ||
        (line[0] == '~' && line[1] == '~' && line[2] == '~')) {
        if (r->state == MD_STATE_NORMAL) {
            r->state = MD_STATE_CODE_BLOCK;
            r->code_fence[0] = line[0];
            r->code_fence[1] = line[1];
            r->code_fence[2] = line[2];
            r->code_fence[3] = '\0';
            r->code_fence_len = 3;
            /* Parse language */
            const char *lang = line + 3;
            while (*lang == ' ') lang++;
            int llen = (int)strlen(lang);
            if (llen >= (int)sizeof(r->code_lang)) llen = (int)sizeof(r->code_lang) - 1;
            memcpy(r->code_lang, lang, llen);
            r->code_lang[llen] = '\0';
            /* Trim trailing whitespace from lang */
            char *le = r->code_lang + strlen(r->code_lang);
            while (le > r->code_lang && (*(le-1) == ' ' || *(le-1) == '\t' || *(le-1) == '\n'))
                *--le = '\0';
            r->code_len = 0;
            r->code_buf[0] = '\0';
            return;
        }
        if (r->state == MD_STATE_CODE_BLOCK) {
            /* End of code block */
            render_code_block(out, r->code_buf, r->code_lang, tw);
            r->state = MD_STATE_NORMAL;
            r->code_len = 0;
            return;
        }
    }

    /* Inside code block: accumulate */
    if (r->state == MD_STATE_CODE_BLOCK) {
        int ll = (int)strlen(line);
        if (r->code_len + ll + 2 < MD_BUF_MAX) {
            memcpy(r->code_buf + r->code_len, line, ll);
            r->code_len += ll;
            r->code_buf[r->code_len++] = '\n';
            r->code_buf[r->code_len] = '\0';
        }
        return;
    }

    /* ── Table detection ── */
    if (line[0] == '|' || (strchr(line, '|') && r->state == MD_STATE_TABLE)) {
        if (is_table_separator(line)) {
            r->table_has_sep = true;
            return;
        }

        /* Parse this row */
        char cells[MD_TABLE_MAXCOL][256];
        int ncols = parse_table_row(line, cells, MD_TABLE_MAXCOL);

        if (ncols > 0 && r->table_rows < MD_TABLE_MAXROW) {
            if (r->state != MD_STATE_TABLE) {
                r->state = MD_STATE_TABLE;
                r->table_cols = ncols;
                r->table_rows = 0;
            }
            if (ncols > r->table_cols) r->table_cols = ncols;

            for (int c = 0; c < ncols; c++) {
                r->table_cells[r->table_rows][c] = strdup(cells[c]);
            }
            for (int c = ncols; c < r->table_cols; c++) {
                r->table_cells[r->table_rows][c] = strdup("");
            }
            r->table_rows++;
        }
        return;
    }

    /* Flush pending table if we're no longer in one */
    if (r->state == MD_STATE_TABLE) {
        render_table(out, r);
        table_free(r);
        r->state = MD_STATE_NORMAL;
    }

    /* ── Multi-line LaTeX display block ── */
    if (r->state == MD_STATE_LATEX_BLOCK) {
        if (str_starts(line, "$$")) {
            char latex[MD_BUF_MAX];
            int n = r->latex_len < (int)sizeof(latex) - 1 ? r->latex_len : (int)sizeof(latex) - 1;
            memcpy(latex, r->latex_buf, n);
            latex[n] = '\0';
            latex_replace_symbols(latex, sizeof(latex));
            fprintf(out, "\n");
            const char *lp = latex;
            while (*lp) {
                const char *nl = strchr(lp, '\n');
                int ll = nl ? (int)(nl - lp) : (int)strlen(lp);
                fprintf(out, "  %s%s", TUI_BOLD, TUI_BMAGENTA);
                fwrite(lp, 1, ll, out);
                fprintf(out, "%s\n", TUI_RESET);
                lp = nl ? nl + 1 : lp + ll;
                if (!nl) break;
            }
            fprintf(out, "\n");
            r->state = MD_STATE_NORMAL;
            r->latex_len = 0;
            return;
        }
        int ll = (int)strlen(line);
        if (r->latex_len + ll + 2 < MD_BUF_MAX) {
            memcpy(r->latex_buf + r->latex_len, line, ll);
            r->latex_len += ll;
            r->latex_buf[r->latex_len++] = '\n';
            r->latex_buf[r->latex_len] = '\0';
        }
        return;
    }

    /* ── HTML block accumulation ── */
    if (r->state == MD_STATE_HTML_BLOCK) {
        char close_tag[40];
        snprintf(close_tag, sizeof(close_tag), "</%s>", r->html_tag);
        bool has_close = (strstr(line, close_tag) != NULL);
        int ll = (int)strlen(line);
        if (r->html_len + ll + 2 < MD_BUF_MAX) {
            memcpy(r->html_buf + r->html_len, line, ll);
            r->html_len += ll;
            r->html_buf[r->html_len++] = '\n';
            r->html_buf[r->html_len] = '\0';
        }
        if (has_close) {
            render_inline(out, r->html_buf);
            fprintf(out, "\n");
            r->state = MD_STATE_NORMAL;
            r->html_len = 0;
        }
        return;
    }

    /* ── Blank line ── */
    if (is_blank_line(line)) {
        /* Flush any deferred setext base line as normal paragraph */
        if (r->prev_line_valid) {
            render_inline(out, r->prev_line);
            fprintf(out, "\n");
            r->prev_line_valid = false;
        }
        if (!r->last_was_blank) {
            fprintf(out, "\n");
            r->last_was_blank = true;
        }
        r->in_list = false;
        r->list_depth = 0;
        r->blockquote_depth = 0;
        return;
    }
    r->last_was_blank = false;

    /* ── Setext heading detection ── */
    if (r->prev_line_valid && len >= 1) {
        bool is_setext1 = true, is_setext2 = true;
        for (int i = 0; i < len; i++) {
            if (line[i] != '=' && line[i] != ' ') is_setext1 = false;
            if (line[i] != '-' && line[i] != ' ') is_setext2 = false;
        }
        if ((is_setext1 && strchr(line, '=')) || (is_setext2 && strchr(line, '-') && len >= 2)) {
            int level = is_setext1 ? 1 : 2;
            if (level == 1) {
                fprintf(out, "\n%s%s%s ", TUI_BOLD, TUI_BWHITE, TUI_UNDERLINE);
                render_inline(out, r->prev_line);
                fprintf(out, "%s\n\n", TUI_RESET);
            } else {
                fprintf(out, "\n%s%s ", TUI_BOLD, TUI_BCYAN);
                render_inline(out, r->prev_line);
                fprintf(out, "%s\n", TUI_RESET);
            }
            r->prev_line_valid = false;
            return;
        }
        render_inline(out, r->prev_line);
        fprintf(out, "\n");
        r->prev_line_valid = false;
    }

    /* ── Reference link definition: [id]: url "title" ── */
    if (line[0] == '[' && r->ref_count < MD_REF_MAX) {
        const char *close = strchr(line + 1, ']');
        if (close && close[1] == ':' && close[2] != ':') {
            const char *url = close + 2;
            while (*url == ' ' || *url == '\t') url++;
            if (*url) {
                md_ref_link_t *ref = &r->refs[r->ref_count];
                int id_len = (int)(close - line - 1);
                if (id_len >= (int)sizeof(ref->id)) id_len = (int)sizeof(ref->id) - 1;
                memcpy(ref->id, line + 1, id_len);
                ref->id[id_len] = '\0';
                const char *ue = url;
                if (*url == '<') { url++; ue = strchr(url, '>'); if (!ue) ue = url + strlen(url); }
                else { while (*ue && *ue != ' ' && *ue != '\t') ue++; }
                int url_len = (int)(ue - url);
                if (url_len >= (int)sizeof(ref->url)) url_len = (int)sizeof(ref->url) - 1;
                memcpy(ref->url, url, url_len);
                ref->url[url_len] = '\0';
                ref->title[0] = '\0';
                r->ref_count++;
                return;
            }
        }
    }

    /* ── Footnote definition: [^id]: text ── */
    if (line[0] == '[' && line[1] == '^' && r->footnote_count < MD_FOOTNOTE_MAX) {
        const char *close = strchr(line + 2, ']');
        if (close && close[1] == ':') {
            md_footnote_t *fn = &r->footnotes[r->footnote_count];
            int id_len = (int)(close - line - 2);
            if (id_len >= (int)sizeof(fn->id)) id_len = (int)sizeof(fn->id) - 1;
            memcpy(fn->id, line + 2, id_len);
            fn->id[id_len] = '\0';
            const char *text = close + 2;
            while (*text == ' ' || *text == '\t') text++;
            strncpy(fn->text, text, sizeof(fn->text) - 1);
            fn->text[sizeof(fn->text)-1] = '\0';
            r->footnote_count++;
            fprintf(out, "  %s%s[^%s]%s ", TUI_DIM, TUI_BCYAN, fn->id, TUI_RESET);
            render_inline(out, fn->text);
            fprintf(out, "\n");
            return;
        }
    }

    /* ── LaTeX display block: $$ ── */
    if (str_starts(line, "$$")) {
        char *end_latex = strstr(line + 2, "$$");
        if (end_latex && end_latex > line + 2) {
            /* Single-line $$ ... $$ */
            *end_latex = '\0';
            char latex[MD_LINE_MAX];
            strncpy(latex, line + 2, sizeof(latex) - 1);
            latex[sizeof(latex)-1] = '\0';
            latex_replace_symbols(latex, sizeof(latex));
            fprintf(out, "\n  %s%s%s%s\n\n", TUI_BOLD, TUI_BMAGENTA, latex, TUI_RESET);
            return;
        }
        /* Multi-line start */
        r->state = MD_STATE_LATEX_BLOCK;
        r->latex_len = 0;
        r->latex_buf[0] = '\0';
        const char *after = line + 2;
        while (*after == ' ') after++;
        if (*after) {
            int al = (int)strlen(after);
            memcpy(r->latex_buf, after, al);
            r->latex_len = al;
            r->latex_buf[r->latex_len++] = '\n';
            r->latex_buf[r->latex_len] = '\0';
        }
        return;
    }

    /* ── Headers ── */
    if (line[0] == '#') {
        int level = 0;
        while (line[level] == '#' && level < 6) level++;
        if (line[level] == ' ' || line[level] == '\0') {
            const char *text = line + level;
            while (*text == ' ') text++;

            switch (level) {
            case 1:
                fprintf(out, "\n%s%s%s ", TUI_BOLD, TUI_BWHITE, TUI_UNDERLINE);
                render_inline(out, text);
                fprintf(out, "%s\n\n", TUI_RESET);
                break;
            case 2:
                fprintf(out, "\n%s%s ", TUI_BOLD, TUI_BCYAN);
                render_inline(out, text);
                fprintf(out, "%s\n", TUI_RESET);
                break;
            case 3:
                fprintf(out, "\n%s%s ", TUI_BOLD, TUI_BBLUE);
                render_inline(out, text);
                fprintf(out, "%s\n", TUI_RESET);
                break;
            case 4:
                fprintf(out, "%s%s ", TUI_BOLD, TUI_BMAGENTA);
                render_inline(out, text);
                fprintf(out, "%s\n", TUI_RESET);
                break;
            default:
                fprintf(out, "%s%s ", TUI_BOLD, TUI_DIM);
                render_inline(out, text);
                fprintf(out, "%s\n", TUI_RESET);
                break;
            }
            return;
        }
    }

    /* ── Horizontal rule ── */
    if ((str_starts(line, "---") || str_starts(line, "***") || str_starts(line, "___")) &&
        len >= 3) {
        bool is_rule = true;
        char ch = line[0];
        for (int i = 0; i < len; i++) {
            if (line[i] != ch && line[i] != ' ') { is_rule = false; break; }
        }
        if (is_rule) {
            int rw = tw - 4;
            if (rw > 60) rw = 60;
            fprintf(out, "  %s", TUI_DIM);
            for (int i = 0; i < rw; i++) fprintf(out, "─");
            fprintf(out, "%s\n", TUI_RESET);
            return;
        }
    }

    /* ── Blockquotes (nested, with GFM alerts) ── */
    if (line[0] == '>') {
        int depth = 0;
        const char *text = line;
        while (*text == '>') { depth++; text++; if (*text == ' ') text++; }
        r->blockquote_depth = depth;

        /* GFM alerts */
        if (str_starts(text, "[!NOTE]") || str_starts(text, "[!note]")) {
            fprintf(out, "  %s┃%s %s%s\xE2\x84\xB9  Note%s\n", TUI_BBLUE, TUI_RESET, TUI_BOLD, TUI_BBLUE, TUI_RESET);
            return;
        }
        if (str_starts(text, "[!WARNING]") || str_starts(text, "[!warning]")) {
            fprintf(out, "  %s┃%s %s%s\xE2\x9A\xA0  Warning%s\n", TUI_BYELLOW, TUI_RESET, TUI_BOLD, TUI_BYELLOW, TUI_RESET);
            return;
        }
        if (str_starts(text, "[!TIP]") || str_starts(text, "[!tip]")) {
            fprintf(out, "  %s┃%s %s%s\xF0\x9F\x92\xA1 Tip%s\n", TUI_BGREEN, TUI_RESET, TUI_BOLD, TUI_BGREEN, TUI_RESET);
            return;
        }
        if (str_starts(text, "[!IMPORTANT]") || str_starts(text, "[!important]")) {
            fprintf(out, "  %s┃%s %s%s\xE2\x9D\x97 Important%s\n", TUI_BMAGENTA, TUI_RESET, TUI_BOLD, TUI_BMAGENTA, TUI_RESET);
            return;
        }
        if (str_starts(text, "[!CAUTION]") || str_starts(text, "[!caution]")) {
            fprintf(out, "  %s┃%s %s%s\xF0\x9F\x94\xB4 Caution%s\n", TUI_BRED, TUI_RESET, TUI_BOLD, TUI_BRED, TUI_RESET);
            return;
        }

        /* Nested blockquote rendering */
        const char *bq_colors[] = { TUI_BBLUE, TUI_BCYAN, TUI_BMAGENTA, TUI_BYELLOW, TUI_BGREEN };
        for (int d = 0; d < depth; d++)
            fprintf(out, "  %s┃%s", bq_colors[d % 5], TUI_RESET);
        fprintf(out, " %s", TUI_ITALIC);
        render_inline(out, text);
        fprintf(out, "%s\n", TUI_RESET);
        return;
    }

    /* ── HTML block elements ── */
    if (line[0] == '<') {
        if (str_starts(line, "<details")) {
            fprintf(out, "  %s\xE2\x96\xB6%s ", TUI_BCYAN, TUI_RESET);
            const char *sum = strstr(line, "<summary>");
            if (sum) {
                const char *sc = strstr(sum + 9, "</summary>");
                if (sc) { fprintf(out, "%s", TUI_BOLD); fwrite(sum + 9, 1, sc - sum - 9, out); fprintf(out, "%s", TUI_RESET); }
            }
            fprintf(out, "\n");
            return;
        }
        if (str_starts(line, "</details>")) { fprintf(out, "  %s\xE2\x97\x80%s\n", TUI_DIM, TUI_RESET); return; }
        if (str_starts(line, "<summary>")) {
            const char *sc = strstr(line + 9, "</summary>");
            fprintf(out, "  %s\xE2\x96\xB6 %s%s", TUI_BCYAN, TUI_BOLD, TUI_RESET);
            if (sc) { fprintf(out, "%s", TUI_BOLD); fwrite(line + 9, 1, sc - line - 9, out); fprintf(out, "%s", TUI_RESET); }
            fprintf(out, "\n");
            return;
        }
        if (str_starts(line, "<pre")) {
            r->state = MD_STATE_HTML_BLOCK;
            strncpy(r->html_tag, "pre", sizeof(r->html_tag));
            r->html_len = 0;
            r->html_buf[0] = '\0';
            if (strstr(line, "</pre>")) {
                render_inline(out, line);
                fprintf(out, "\n");
                r->state = MD_STATE_NORMAL;
            }
            return;
        }
        const char *block_tags[] = {"div","section","article","nav","table","blockquote",NULL};
        for (int i = 0; block_tags[i]; i++) {
            char ot[32];
            snprintf(ot, sizeof(ot), "<%s", block_tags[i]);
            if (str_starts(line, ot) && (line[strlen(ot)] == '>' || line[strlen(ot)] == ' ')) {
                r->state = MD_STATE_HTML_BLOCK;
                strncpy(r->html_tag, block_tags[i], sizeof(r->html_tag) - 1);
                r->html_len = 0;
                char ct[40];
                snprintf(ct, sizeof(ct), "</%s>", block_tags[i]);
                if (strstr(line, ct)) {
                    render_inline(out, line);
                    fprintf(out, "\n");
                    r->state = MD_STATE_NORMAL;
                } else {
                    int ll = len;
                    memcpy(r->html_buf, line, ll);
                    r->html_len = ll;
                    r->html_buf[r->html_len++] = '\n';
                    r->html_buf[r->html_len] = '\0';
                }
                return;
            }
        }
    }

    /* ── Unordered list (with nesting) ── */
    {
        int indent = 0;
        const char *lp = line;
        while (*lp == ' ' || *lp == '\t') { indent += (*lp == '\t') ? 4 : 1; lp++; }
        char marker = *lp;
        if ((marker == '-' || marker == '*' || marker == '+') && lp[1] == ' ') {
            int depth = indent / 2;
            if (depth >= MD_LIST_MAXDEPTH) depth = MD_LIST_MAXDEPTH - 1;
            const char *text = lp + 2;

            if (text[0] == '[' && (text[1] == ' ' || text[1] == 'x' || text[1] == 'X') && text[2] == ']') {
                bool checked = (text[1] == 'x' || text[1] == 'X');
                for (int i = 0; i < depth; i++) fprintf(out, "  ");
                fprintf(out, "  %s%s%s ", checked ? TUI_GREEN : TUI_DIM, checked ? "\xE2\x98\x91" : "\xE2\x98\x90", TUI_RESET);
                render_inline(out, text + 4);
            } else {
                const char *bullets[] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xB8", "\xE2\x96\xB9", "\xE2\x80\xA3"};
                const char *bcolors[] = {TUI_BCYAN, TUI_DIM, TUI_BBLUE, TUI_DIM, TUI_BMAGENTA};
                for (int i = 0; i < depth; i++) fprintf(out, "  ");
                fprintf(out, "  %s%s%s ", bcolors[depth % 5], bullets[depth % 5], TUI_RESET);
                render_inline(out, text);
            }
            fprintf(out, "\n");
            r->in_list = true;
            r->list_depth = depth + 1;
            r->list_stack[depth] = MD_LIST_UNORDERED;
            return;
        }
    }

    /* ── Ordered list (with nesting) ── */
    {
        int indent = 0;
        const char *lp = line;
        while (*lp == ' ' || *lp == '\t') { indent += (*lp == '\t') ? 4 : 1; lp++; }
        if (isdigit((unsigned char)*lp)) {
            const char *p = lp;
            while (isdigit((unsigned char)*p)) p++;
            if ((*p == '.' || *p == ')') && p[1] == ' ') {
                int depth = indent / 3;
                if (depth >= MD_LIST_MAXDEPTH) depth = MD_LIST_MAXDEPTH - 1;
                int num = atoi(lp);
                for (int i = 0; i < depth; i++) fprintf(out, "   ");
                fprintf(out, "  %s%s%d.%s ", TUI_BOLD, TUI_BYELLOW, num, TUI_RESET);
                render_inline(out, p + 2);
                fprintf(out, "\n");
                r->in_list = true;
                r->list_depth = depth + 1;
                r->list_stack[depth] = MD_LIST_ORDERED;
                r->list_num[depth] = num;
                return;
            }
        }
    }

    /* ── Indented code (4 spaces or tab, not in a list) ── */
    if (!r->in_list && (str_starts(line, "    ") || line[0] == '\t')) {
        const char *code = (line[0] == '\t') ? line + 1 : line + 4;
        fprintf(out, "  %s%s %s %s\n", TUI_BG256(236), TUI_FG256(252), code, TUI_RESET);
        return;
    }

    /* ── List continuation: indented text under a list item ── */
    if (r->in_list && (line[0] == ' ' || line[0] == '\t')) {
        int indent = 0;
        while (line[indent] == ' ' || line[indent] == '\t') indent++;
        if (indent >= 2) {
            for (int i = 0; i < (indent / 2); i++) fprintf(out, "  ");
            fprintf(out, "  ");
            render_inline(out, line + indent);
            fprintf(out, "\n");
            return;
        }
    }

    /* ── Definition list: term\n: definition ── */
    if (line[0] == ':' && line[1] == ' ') {
        fprintf(out, "  %s%s\xE2\x96\xB8%s ", TUI_BOLD, TUI_BCYAN, TUI_RESET);
        render_inline(out, line + 2);
        fprintf(out, "\n");
        return;
    }

    /* ── Possible setext base: defer plain text lines ── */
    if (len > 0 && line[0] != ' ' && line[0] != '\t' &&
        r->state == MD_STATE_NORMAL && !r->in_list) {
        bool could_be_setext = true;
        if (line[0] == '#' || line[0] == '>' || line[0] == '|' || line[0] == '`' ||
            line[0] == '~' || line[0] == '-' || line[0] == '*' || line[0] == '+' ||
            line[0] == '[' || line[0] == '<' || line[0] == '$' || line[0] == ':')
            could_be_setext = false;
        if (could_be_setext) {
            strncpy(r->prev_line, line, MD_LINE_MAX - 1);
            r->prev_line[MD_LINE_MAX - 1] = '\0';
            r->prev_line_valid = true;
            return;
        }
    }

    /* ── Normal paragraph ── */
    render_inline(out, line);
    fprintf(out, "\n");
}

/* ── Public API ───────────────────────────────────────────────────────── */

void md_init(md_renderer_t *r, FILE *out) {
    memset(r, 0, sizeof(*r));
    r->out = out;
    r->term_width = term_width();
}

void md_feed(md_renderer_t *r, const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ch = text[i];

        if (ch == '\n') {
            r->line_buf[r->line_len] = '\0';
            render_line(r, r->line_buf);
            r->line_len = 0;
            fflush(r->out);
        } else {
            if (r->line_len < MD_LINE_MAX - 1) {
                r->line_buf[r->line_len++] = ch;
            }
        }
    }
}

void md_feed_str(md_renderer_t *r, const char *text) {
    md_feed(r, text, strlen(text));
}

void md_flush(md_renderer_t *r) {
    /* Flush any partial line */
    if (r->line_len > 0) {
        r->line_buf[r->line_len] = '\0';
        render_line(r, r->line_buf);
        r->line_len = 0;
    }

    /* Flush deferred setext line */
    if (r->prev_line_valid) {
        render_inline(r->out, r->prev_line);
        fprintf(r->out, "\n");
        r->prev_line_valid = false;
    }

    /* Flush pending table */
    if (r->state == MD_STATE_TABLE) {
        render_table(r->out, r);
        table_free(r);
        r->state = MD_STATE_NORMAL;
    }

    /* Flush pending code block (unclosed) */
    if (r->state == MD_STATE_CODE_BLOCK && r->code_len > 0) {
        render_code_block(r->out, r->code_buf, r->code_lang, r->term_width);
        r->code_len = 0;
        r->state = MD_STATE_NORMAL;
    }

    /* Flush pending LaTeX block (unclosed) */
    if (r->state == MD_STATE_LATEX_BLOCK && r->latex_len > 0) {
        char latex[MD_BUF_MAX];
        int n = r->latex_len < (int)sizeof(latex) - 1 ? r->latex_len : (int)sizeof(latex) - 1;
        memcpy(latex, r->latex_buf, n);
        latex[n] = '\0';
        latex_replace_symbols(latex, sizeof(latex));
        fprintf(r->out, "  %s%s%s%s\n", TUI_BOLD, TUI_BMAGENTA, latex, TUI_RESET);
        r->latex_len = 0;
        r->state = MD_STATE_NORMAL;
    }

    /* Flush pending HTML block (unclosed) */
    if (r->state == MD_STATE_HTML_BLOCK && r->html_len > 0) {
        render_inline(r->out, r->html_buf);
        fprintf(r->out, "\n");
        r->html_len = 0;
        r->state = MD_STATE_NORMAL;
    }

    fflush(r->out);
}

void md_reset(md_renderer_t *r) {
    /* Free table cells */
    table_free(r);

    FILE *out = r->out;
    memset(r, 0, sizeof(*r));
    r->out = out;
    r->term_width = term_width();
}
