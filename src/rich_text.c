#define _POSIX_C_SOURCE 200809L

#include "rich_text.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *tex;
    const char *utf8;
} math_symbol_t;

/* Long commands precede their prefixes (\rightarrow before \to, etc.). */
static const math_symbol_t MATH_SYMBOLS[] = {
    {"\\leftrightarrow", "↔"},
    {"\\Rightarrow", "⇒"},
    {"\\Leftarrow", "⇐"},
    {"\\rightarrow", "→"},
    {"\\leftarrow", "←"},
    {"\\emptyset", "∅"},
    {"\\varepsilon", "ε"},
    {"\\subseteq", "⊆"},
    {"\\supseteq", "⊇"},
    {"\\partial", "∂"},
    {"\\nabla", "∇"},
    {"\\forall", "∀"},
    {"\\exists", "∃"},
    {"\\notin", "∉"},
    {"\\infty", "∞"},
    {"\\approx", "≈"},
    {"\\equiv", "≡"},
    {"\\propto", "∝"},
    {"\\lambda", "λ"},
    {"\\Lambda", "Λ"},
    {"\\epsilon", "ε"},
    {"\\upsilon", "υ"},
    {"\\Upsilon", "Υ"},
    {"\\rightarrow", "→"},
    {"\\alpha", "α"},
    {"\\beta", "β"},
    {"\\gamma", "γ"},
    {"\\delta", "δ"},
    {"\\theta", "θ"},
    {"\\kappa", "κ"},
    {"\\sigma", "σ"},
    {"\\omega", "ω"},
    {"\\Gamma", "Γ"},
    {"\\Delta", "Δ"},
    {"\\Theta", "Θ"},
    {"\\Sigma", "Σ"},
    {"\\Omega", "Ω"},
    {"\\subset", "⊂"},
    {"\\supset", "⊃"},
    {"\\langle", "⟨"},
    {"\\rangle", "⟩"},
    {"\\times", "×"},
    {"\\cdot", "·"},
    {"\\oplus", "⊕"},
    {"\\otimes", "⊗"},
    {"\\wedge", "∧"},
    {"\\vee", "∨"},
    {"\\land", "∧"},
    {"\\lor", "∨"},
    {"\\cup", "∪"},
    {"\\cap", "∩"},
    {"\\prod", "∏"},
    {"\\sum", "∑"},
    {"\\int", "∫"},
    {"\\oint", "∮"},
    {"\\sqrt", "√"},
    {"\\pm", "±"},
    {"\\mp", "∓"},
    {"\\div", "÷"},
    {"\\neq", "≠"},
    {"\\ne", "≠"},
    {"\\leq", "≤"},
    {"\\geq", "≥"},
    {"\\le", "≤"},
    {"\\ge", "≥"},
    {"\\sim", "∼"},
    {"\\in", "∈"},
    {"\\neg", "¬"},
    {"\\to", "→"},
    {"\\gets", "←"},
    {"\\ldots", "…"},
    {"\\cdots", "⋯"},
    {"\\vdots", "⋮"},
    {"\\mu", "μ"},
    {"\\nu", "ν"},
    {"\\xi", "ξ"},
    {"\\pi", "π"},
    {"\\rho", "ρ"},
    {"\\tau", "τ"},
    {"\\phi", "φ"},
    {"\\chi", "χ"},
    {"\\psi", "ψ"},
    {"\\eta", "η"},
    {"\\zeta", "ζ"},
    {"\\Pi", "Π"},
    {"\\Phi", "Φ"},
    {"\\Psi", "Ψ"},
    /* Frequently emitted AMS/MathJax operators; long names must precede
     * prefixes because conversion is a left-to-right maximal match. */
    {"\\Longleftrightarrow", "⟺"},
    {"\\Longrightarrow", "⟹"},
    {"\\Longleftarrow", "⟸"},
    {"\\mapsto", "↦"},
    {"\\implies", "⟹"},
    {"\\iff", "⟺"},
    {"\\therefore", "∴"},
    {"\\because", "∵"},
    {"\\degree", "°"},
    {"\\prime", "′"},
    {"\\mathbb{R}", "ℝ"},
    {"\\mathbb{Q}", "ℚ"},
    {"\\mathbb{Z}", "ℤ"},
    {"\\mathbb{N}", "ℕ"},
    {"\\Re", "ℜ"},
    {"\\Im", "ℑ"},
    {"\\hbar", "ℏ"},
    {"\\ell", "ℓ"},
    {"\\aleph", "ℵ"},
    {NULL, NULL},
};

static const char *SUPER_DIGITS[] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
static const char *SUB_DIGITS[] = {"₀", "₁", "₂", "₃", "₄", "₅", "₆", "₇", "₈", "₉"};

static void out_bytes(char **dst, char *end, const char *src, size_t n) {
    if (!dst || !*dst || !src || *dst >= end)
        return;
    size_t room = (size_t)(end - *dst);
    if (n > room)
        n = room;
    memcpy(*dst, src, n);
    *dst += n;
}

static void out_str(char **dst, char *end, const char *src) {
    out_bytes(dst, end, src, src ? strlen(src) : 0);
}

static const char *brace_end(const char *p) {
    if (!p || *p != '{')
        return NULL;
    int depth = 1;
    for (p++; *p; p++) {
        if (*p == '{')
            depth++;
        else if (*p == '}' && --depth == 0)
            return p;
    }
    return NULL;
}

static void convert_math_range(const char *src, size_t len, char **dst, char *end) {
    const char *p = src, *limit = src + len;
    while (p < limit && *p && *dst < end) {
        if (!strncmp(p, "\\frac{", 6)) {
            const char *num_open = p + 5;
            const char *num_end = brace_end(num_open);
            const char *den_open = num_end && num_end[1] == '{' ? num_end + 1 : NULL;
            const char *den_end = den_open ? brace_end(den_open) : NULL;
            if (num_end && den_end && den_end < limit) {
                out_str(dst, end, "(");
                convert_math_range(num_open + 1, (size_t)(num_end - num_open - 1), dst, end);
                out_str(dst, end, ")⁄(");
                convert_math_range(den_open + 1, (size_t)(den_end - den_open - 1), dst, end);
                out_str(dst, end, ")");
                p = den_end + 1;
                continue;
            }
        }
        if (!strncmp(p, "\\sqrt{", 6)) {
            const char *open = p + 5, *close = brace_end(open);
            if (close && close < limit) {
                out_str(dst, end, "√(");
                convert_math_range(open + 1, (size_t)(close - open - 1), dst, end);
                out_str(dst, end, ")");
                p = close + 1;
                continue;
            }
        }
        const char *group_cmds[] = {"\\text{",   "\\mathrm{",       "\\mathbf{",
                                    "\\mathit{", "\\operatorname{", "\\boxed{"};
        bool grouped = false;
        for (size_t i = 0; i < sizeof(group_cmds) / sizeof(group_cmds[0]); i++) {
            size_t n = strlen(group_cmds[i]);
            if (strncmp(p, group_cmds[i], n))
                continue;
            const char *open = p + n - 1, *close = brace_end(open);
            if (!close || close >= limit)
                break;
            if (!strcmp(group_cmds[i], "\\boxed{"))
                out_str(dst, end, "⟨ ");
            convert_math_range(open + 1, (size_t)(close - open - 1), dst, end);
            if (!strcmp(group_cmds[i], "\\boxed{"))
                out_str(dst, end, " ⟩");
            p = close + 1;
            grouped = true;
            break;
        }
        if (grouped)
            continue;
        if (!strncmp(p, "\\left", 5)) {
            p += 5;
            continue;
        }
        if (!strncmp(p, "\\right", 6)) {
            p += 6;
            continue;
        }
        if (!strncmp(p, "\\begin{", 7) || !strncmp(p, "\\end{", 5)) {
            const char *open = strchr(p, '{'), *close = open ? brace_end(open) : NULL;
            if (close && close < limit) {
                p = close + 1;
                continue;
            }
        }
        if (*p == '^' || *p == '_') {
            bool super = *p++ == '^';
            const char *q = p, *q_end = p + 1;
            if (*q == '{') {
                const char *close = brace_end(q);
                if (close && close < limit) {
                    q++;
                    q_end = close;
                    p = close + 1;
                }
            } else {
                if (*q == '\\') {
                    bool symbol = false;
                    for (const math_symbol_t *s = MATH_SYMBOLS; s->tex; s++) {
                        size_t n = strlen(s->tex);
                        if ((size_t)(limit - q) >= n && !strncmp(q, s->tex, n)) {
                            out_str(dst, end, s->utf8);
                            p = q + n;
                            symbol = true;
                            break;
                        }
                    }
                    if (symbol)
                        continue;
                }
                p++;
            }
            for (; q < q_end; q++) {
                if (*q == '\\') {
                    bool symbol = false;
                    for (const math_symbol_t *s = MATH_SYMBOLS; s->tex; s++) {
                        size_t n = strlen(s->tex);
                        if ((size_t)(q_end - q) >= n && !strncmp(q, s->tex, n)) {
                            out_str(dst, end, s->utf8);
                            q += n - 1;
                            symbol = true;
                            break;
                        }
                    }
                    if (symbol)
                        continue;
                }
                if (*q == '^' || *q == '_')
                    continue;
                if (isdigit((unsigned char)*q))
                    out_str(dst, end, super ? SUPER_DIGITS[*q - '0'] : SUB_DIGITS[*q - '0']);
                else {
                    const char *mapped = NULL;
                    if (super && *q == 'n')
                        mapped = "ⁿ";
                    else if (super && *q == 'i')
                        mapped = "ⁱ";
                    else if (super && *q == 'x')
                        mapped = "ˣ";
                    else if (super && *q == 'y')
                        mapped = "ʸ";
                    else if (super && *q == '+')
                        mapped = "⁺";
                    else if (super && *q == '-')
                        mapped = "⁻";
                    else if (!super && *q == 'i')
                        mapped = "ᵢ";
                    else if (!super && *q == 'n')
                        mapped = "ₙ";
                    else if (!super && *q == 'x')
                        mapped = "ₓ";
                    else if (!super && *q == '+')
                        mapped = "₊";
                    else if (!super && *q == '-')
                        mapped = "₋";
                    if (mapped)
                        out_str(dst, end, mapped);
                    else
                        out_bytes(dst, end, q, 1);
                }
            }
            continue;
        }
        if (*p == '\\') {
            if (p + 1 < limit && strchr(",;:! ", p[1])) {
                if (p[1] != '!')
                    out_str(dst, end, " ");
                p += 2;
                continue;
            }
            if (p + 1 < limit && p[1] == '\\') {
                /* LaTex row break: preserve geometry in the display card. */
                out_bytes(dst, end, "\n", 1);
                p += 2;
                continue;
            }
            bool matched = false;
            for (const math_symbol_t *s = MATH_SYMBOLS; s->tex; s++) {
                size_t n = strlen(s->tex);
                if ((size_t)(limit - p) >= n && !strncmp(p, s->tex, n)) {
                    out_str(dst, end, s->utf8);
                    p += n;
                    matched = true;
                    break;
                }
            }
            if (matched)
                continue;
            const char *functions[] = {"sin", "cos", "tan", "log", "ln",  "exp", "lim",
                                       "max", "min", "det", "dim", "arg", "mod"};
            for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
                size_t n = strlen(functions[i]);
                if ((size_t)(limit - p) > n && p[0] == '\\' && !strncmp(p + 1, functions[i], n)) {
                    out_bytes(dst, end, functions[i], n);
                    p += n + 1;
                    matched = true;
                    break;
                }
            }
            if (matched)
                continue;
            p++; /* Unknown command: keep its readable name, drop the slash. */
            continue;
        }
        if (*p == '{' || *p == '}') {
            p++;
            continue;
        }
        if (*p == '&') {
            out_str(dst, end, "  ");
            p++;
            continue;
        }
        out_bytes(dst, end, p++, 1);
    }
}

size_t rich_text_latex_to_unicode(const char *latex, char *out, size_t out_size) {
    if (!out || out_size < 1)
        return 0;
    out[0] = '\0';
    if (!latex)
        return 0;
    char *dst = out, *end = out + out_size - 1;
    convert_math_range(latex, strlen(latex), &dst, end);
    *dst = '\0';
    return (size_t)(dst - out);
}

static void emit_token(rich_token_t *tokens, size_t cap, size_t *count, rich_token_type_t type,
                       rich_style_t style, const char *text, size_t len, int level, int indent,
                       bool block_start) {
    if (!tokens || !count || *count >= cap)
        return;
    if (type == RICH_TOKEN_TEXT && (!text || len == 0))
        return;
    while (type == RICH_TOKEN_TEXT && len > 0 && *count < cap) {
        size_t take = len;
        if (take >= RICH_TOKEN_TEXT_MAX)
            take = RICH_TOKEN_TEXT_MAX - 1;
        while (take > 0 && ((unsigned char)text[take] & 0xc0) == 0x80)
            take--;
        if (take == 0)
            take = len < RICH_TOKEN_TEXT_MAX ? len : RICH_TOKEN_TEXT_MAX - 1;
        rich_token_t *t = &tokens[(*count)++];
        memset(t, 0, sizeof(*t));
        t->type = type;
        t->style = style;
        t->level = (uint8_t)level;
        t->indent = (uint8_t)indent;
        t->block_start = block_start;
        memcpy(t->text, text, take);
        t->text[take] = '\0';
        text += take;
        len -= take;
        block_start = false;
    }
    if (type != RICH_TOKEN_TEXT && *count < cap) {
        rich_token_t *t = &tokens[(*count)++];
        memset(t, 0, sizeof(*t));
        t->type = type;
        t->style = style;
        t->level = (uint8_t)level;
        t->indent = (uint8_t)indent;
        t->block_start = block_start;
    }
}

static void emit_cstr(rich_token_t *tokens, size_t cap, size_t *count, rich_style_t style,
                      const char *text, int level, int indent, bool block_start) {
    emit_token(tokens, cap, count, RICH_TOKEN_TEXT, style, text, text ? strlen(text) : 0, level,
               indent, block_start);
}

/* Emphasis follows CommonMark-style flanking so identifiers survive:
 * `_` never opens or closes inside a word (s_slash_commands stays literal),
 * `*`/`_` need a non-space run start and a non-space run end. */
static const char *emphasis_close(const char *open, char mark) {
    const char *scan = open + 1;
    while ((scan = strchr(scan, mark)) != NULL) {
        char before = scan[-1];
        char after = scan[1];
        bool closes = before != ' ' && before != '\t' && before != mark;
        if (mark == '_' && (isalnum((unsigned char)after) || after == '_'))
            closes = false;
        if (closes && scan > open + 1)
            return scan;
        scan++;
    }
    return NULL;
}

/* True only when `*`/`_` at p begins a real emphasis run. Shared by the marker
 * branch and the literal scanner so a non-opening marker (tool_response) is
 * absorbed into surrounding text as ONE token — the wrapper spaces adjacent
 * tokens, so splitting on every `_` would render "tool _ response". */
static bool emphasis_opens(const char *line, const char *p) {
    char mark = *p;
    if ((mark != '*' && mark != '_') || !p[1])
        return false;
    char prev = p == line ? '\0' : p[-1];
    bool opens = p[1] != ' ' && p[1] != '\t' && p[1] != mark;
    if (mark == '_' && (isalnum((unsigned char)prev) || prev == '_'))
        opens = false;
    return opens && emphasis_close(p, mark) != NULL;
}

static void parse_inline(const char *line, rich_style_t base, int level, int indent,
                         bool block_start, rich_token_t *tokens, size_t cap, size_t *count) {
    const char *p = line;
    bool first = block_start;
    while (*p && *count < cap) {
        const char *close = NULL;
        if (!strncmp(p, "**", 2) && (close = strstr(p + 2, "**"))) {
            emit_token(tokens, cap, count, RICH_TOKEN_TEXT, RICH_STYLE_STRONG, p + 2,
                       (size_t)(close - p - 2), level, indent, first);
            p = close + 2;
            first = false;
            continue;
        }
        if (!strncmp(p, "~~", 2) && (close = strstr(p + 2, "~~"))) {
            emit_token(tokens, cap, count, RICH_TOKEN_TEXT, RICH_STYLE_STRIKE, p + 2,
                       (size_t)(close - p - 2), level, indent, first);
            p = close + 2;
            first = false;
            continue;
        }
        if (*p == '`' && (close = strchr(p + 1, '`'))) {
            emit_token(tokens, cap, count, RICH_TOKEN_TEXT, RICH_STYLE_CODE, p + 1,
                       (size_t)(close - p - 1), level, indent, first);
            p = close + 1;
            first = false;
            continue;
        }
        if (*p == '$' && p[1] != '$' && (close = strchr(p + 1, '$'))) {
            char math[RICH_TOKEN_TEXT_MAX];
            size_t n = (size_t)(close - p - 1);
            char raw[RICH_TOKEN_TEXT_MAX];
            if (n >= sizeof(raw))
                n = sizeof(raw) - 1;
            memcpy(raw, p + 1, n);
            raw[n] = '\0';
            rich_text_latex_to_unicode(raw, math, sizeof(math));
            emit_cstr(tokens, cap, count, RICH_STYLE_MATH, math, level, indent, first);
            p = close + 1;
            first = false;
            continue;
        }
        if (!strncmp(p, "\\(", 2) && (close = strstr(p + 2, "\\)"))) {
            char math[RICH_TOKEN_TEXT_MAX], raw[RICH_TOKEN_TEXT_MAX];
            size_t n = (size_t)(close - p - 2);
            if (n >= sizeof(raw))
                n = sizeof(raw) - 1;
            memcpy(raw, p + 2, n);
            raw[n] = '\0';
            rich_text_latex_to_unicode(raw, math, sizeof(math));
            emit_cstr(tokens, cap, count, RICH_STYLE_MATH, math, level, indent, first);
            p = close + 2;
            first = false;
            continue;
        }
        if (*p == '[') {
            const char *label_end = strchr(p + 1, ']');
            if (label_end && label_end[1] == '(' && (close = strchr(label_end + 2, ')'))) {
                emit_token(tokens, cap, count, RICH_TOKEN_TEXT, RICH_STYLE_LINK, p + 1,
                           (size_t)(label_end - p - 1), level, indent, first);
                emit_cstr(tokens, cap, count, RICH_STYLE_MUTED, " ↗", level, indent, false);
                p = close + 1;
                first = false;
                continue;
            }
        }
        if ((*p == '*' || *p == '_') && emphasis_opens(line, p)) {
            char mark = *p;
            close = emphasis_close(p, mark);
            emit_token(tokens, cap, count, RICH_TOKEN_TEXT, RICH_STYLE_EMPHASIS, p + 1,
                       (size_t)(close - p - 1), level, indent, first);
            p = close + 1;
            first = false;
            continue;
        }
        if (*p == '<') {
            close = strchr(p + 1, '>');
            if (close) {
                p = close + 1;
                continue;
            }
        }
        if (*p == '\\' && strchr("\\`*{}[]()#+-.!_$", p[1])) {
            emit_token(tokens, cap, count, RICH_TOKEN_TEXT, base, p + 1, 1, level, indent, first);
            p += 2;
            first = false;
            continue;
        }
        const char *start = p;
        while (*p && strncmp(p, "**", 2) && strncmp(p, "~~", 2) && *p != '`' && *p != '$' &&
               strncmp(p, "\\(", 2) && *p != '[' && *p != '*' && *p != '_' && *p != '<')
            p++;
        if (p == start)
            p++;
        emit_token(tokens, cap, count, RICH_TOKEN_TEXT, base, start, (size_t)(p - start), level,
                   indent, first);
        first = false;
    }
}

static bool thematic_rule(const char *line) {
    char mark = 0;
    int count = 0;
    for (const char *p = line; *p; p++) {
        if (isspace((unsigned char)*p))
            continue;
        if (!mark && (*p == '-' || *p == '*' || *p == '_'))
            mark = *p;
        if (*p != mark)
            return false;
        count++;
    }
    return count >= 3;
}

static bool table_separator(const char *line) {
    bool dash = false;
    for (const char *p = line; *p; p++) {
        if (*p == '-')
            dash = true;
        else if (*p != '|' && *p != ':' && !isspace((unsigned char)*p))
            return false;
    }
    return dash && strchr(line, '|');
}

size_t rich_text_parse(const char *markdown, rich_token_t *tokens, size_t capacity) {
    if (!markdown || !tokens || capacity == 0)
        return 0;
    size_t count = 0;
    bool code = false, display_math = false;
    const char *p = markdown;
    while (*p && count < capacity) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= 4096)
            n = 4095;
        char line[4096];
        memcpy(line, p, n);
        line[n] = '\0';
        if (n > 0 && line[n - 1] == '\r')
            line[--n] = '\0';
        const char *s = line;
        int leading = 0;
        while (*s == ' ' || *s == '\t') {
            leading += *s == '\t' ? 4 : 1;
            s++;
        }
        int indent = leading / 2;

        if (!strncmp(s, "```", 3) || !strncmp(s, "~~~", 3)) {
            code = !code;
            if (code && s[3]) {
                char label[96];
                snprintf(label, sizeof(label), "  %s", s + 3);
                emit_cstr(tokens, capacity, &count, RICH_STYLE_MUTED, label, 0, indent, true);
                emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_CODE, NULL, 0, 0,
                           indent, false);
            }
            p = nl ? nl + 1 : p + n;
            continue;
        }
        if (code) {
            emit_cstr(tokens, capacity, &count, RICH_STYLE_CODE, s, 0, indent, true);
            emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_CODE, NULL, 0, 0,
                       indent, false);
            p = nl ? nl + 1 : p + n;
            continue;
        }

        /* Block delimiters must be consumed, never painted as literal $$.
         * The parser is deliberately line-oriented for streaming transcript
         * layout; each source row becomes one centered display row. */
        if (display_math && ((!strcmp(s, "$$")) || !strcmp(s, "\\]"))) {
            display_math = false;
            p = nl ? nl + 1 : p + n;
            continue;
        }
        if (!strncmp(s, "$$", 2) || !strncmp(s, "\\[", 2)) {
            bool dollars = s[0] == '$';
            const char *open = s + 2;
            const char *close = strstr(open, dollars ? "$$" : "\\]");
            if (close) {
                char raw[3072], math[3072];
                size_t m = (size_t)(close - open);
                if (m >= sizeof(raw))
                    m = sizeof(raw) - 1;
                memcpy(raw, open, m);
                raw[m] = '\0';
                rich_text_latex_to_unicode(raw, math, sizeof(math));
                emit_cstr(tokens, capacity, &count, RICH_STYLE_MATH_DISPLAY, math, 0, indent, true);
                emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_MATH_DISPLAY,
                           NULL, 0, 0, indent, false);
            } else {
                display_math = true;
                /* An opener may carry its first equation row. */
                if (*open) {
                    char math[3072];
                    rich_text_latex_to_unicode(open, math, sizeof(math));
                    emit_cstr(tokens, capacity, &count, RICH_STYLE_MATH_DISPLAY, math, 0, indent,
                              true);
                    emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_MATH_DISPLAY,
                               NULL, 0, 0, indent, false);
                }
            }
            p = nl ? nl + 1 : p + n;
            continue;
        }
        if (display_math) {
            char math[3072];
            rich_text_latex_to_unicode(s, math, sizeof(math));
            emit_cstr(tokens, capacity, &count, RICH_STYLE_MATH_DISPLAY, math, 0, indent, true);
            emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_MATH_DISPLAY, NULL, 0,
                       0, indent, false);
            p = nl ? nl + 1 : p + n;
            continue;
        }

        if (!*s) {
            emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_BODY, NULL, 0, 0, 0,
                       true);
        } else if (thematic_rule(s)) {
            emit_token(tokens, capacity, &count, RICH_TOKEN_RULE, RICH_STYLE_MUTED, NULL, 0, 0,
                       indent, true);
            emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_BODY, NULL, 0, 0,
                       indent, false);
        } else if (table_separator(s)) {
            emit_token(tokens, capacity, &count, RICH_TOKEN_RULE, RICH_STYLE_MUTED, NULL, 0, 0,
                       indent, true);
            emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_BODY, NULL, 0, 0,
                       indent, false);
        } else {
            int heading = 0;
            while (s[heading] == '#' && heading < 6)
                heading++;
            if (heading && isspace((unsigned char)s[heading])) {
                s += heading;
                while (isspace((unsigned char)*s))
                    s++;
                parse_inline(s, RICH_STYLE_HEADING, heading, indent, true, tokens, capacity,
                             &count);
            } else if (*s == '>') {
                s++;
                while (*s == ' ')
                    s++;
                parse_inline(s, RICH_STYLE_QUOTE, 0, indent + 1, true, tokens, capacity, &count);
            } else {
                bool list = false;
                char marker[32] = "";
                if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') {
                    s += 2;
                    list = true;
                    if (!strncmp(s, "[x] ", 4) || !strncmp(s, "[X] ", 4)) {
                        snprintf(marker, sizeof(marker), "");
                        s += 4;
                    } else if (!strncmp(s, "[ ] ", 4)) {
                        snprintf(marker, sizeof(marker), "");
                        s += 4;
                    } else
                        snprintf(marker, sizeof(marker), "▸");
                } else if (isdigit((unsigned char)*s)) {
                    const char *q = s;
                    while (isdigit((unsigned char)*q))
                        q++;
                    if ((*q == '.' || *q == ')') && q[1] == ' ') {
                        size_t m = (size_t)(q - s + 1);
                        if (m >= sizeof(marker))
                            m = sizeof(marker) - 1;
                        memcpy(marker, s, m);
                        marker[m] = '\0';
                        s = q + 2;
                        list = true;
                    }
                }
                if (list) {
                    emit_cstr(tokens, capacity, &count, RICH_STYLE_LIST_MARKER, marker, 0, indent,
                              true);
                    emit_cstr(tokens, capacity, &count, RICH_STYLE_BODY, "  ", 0, indent, false);
                }
                if (strchr(s, '|')) {
                    char table[4096];
                    size_t ti = 0;
                    for (const char *q = s; *q && ti + 4 < sizeof(table); q++) {
                        if (*q == '|') {
                            memcpy(table + ti, "│", 3);
                            ti += 3;
                        } else
                            table[ti++] = *q;
                    }
                    table[ti] = '\0';
                    parse_inline(table, RICH_STYLE_CODE, 0, indent, !list, tokens, capacity,
                                 &count);
                } else {
                    parse_inline(s, RICH_STYLE_BODY, 0, indent, !list, tokens, capacity, &count);
                }
            }
            emit_token(tokens, capacity, &count, RICH_TOKEN_BREAK, RICH_STYLE_BODY, NULL, 0, 0,
                       indent, false);
        }
        p = nl ? nl + 1 : p + n;
    }
    return count;
}
