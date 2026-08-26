#define _POSIX_C_SOURCE 200809L

#include "compositor_parity.h"

#include "md.h"
#include "pixel_tui.h"
#include "tui.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PARITY_NATIVE_WIDTH 1120
#define PARITY_NATIVE_HEIGHT 700
#define PARITY_LEGACY_COLS 120
#define PARITY_LEGACY_ROWS 40
#define PARITY_MAX_LINES 2048

static const pixel_tui_fixture_message_t parity_messages[] = {
    {
        .role = "USER",
        .detail = "turn 7",
        .text = "Make the streaming compositor crisp, preserve the original TUI, "
                "and show enough implementation context to review changes without "
                "opening another pane.",
    },
    {
        .role = "ASSISTANT",
        .detail = "analysis",
        .text = "## Frame plan\n\n"
                "The renderer keeps one logical layout and an exact physical backing "
                "store. Streaming mutations are coalesced before raster work.\n\n"
                "- retain semantic message identity\n"
                "- wrap rich text at measured glyph advances\n"
                "- diff 32 x 32 tiles and merge touching damage\n"
                "- compress only the rectangles that changed\n\n"
                "```c\n"
                "if (damage.count == 0) return FRAME_IDENTICAL;\n"
                "upload_patch(image_id, damage.rects, damage.count);\n"
                "```",
    },
    {
        .role = "TOOL",
        .detail = "make test_pixel",
        .text = "native compositor: 51 passed, 0 failed\n"
                "kitty graphics: 30 passed, 0 failed\n"
                "peak retained compositor bytes: 8647168",
    },
    {
        .role = "ASSISTANT",
        .detail = "implementation",
        .text = "### What changed\n\n"
                "| stage | mechanism | budget |\n"
                "|---|---|---:|\n"
                "| layout and raster | retained canvas pool | measured per frame |\n"
                "| damage | bounded tile merge | at most 8 rectangles |\n"
                "| transport | zlib plus base64 | exact wire bytes |\n"
                "| streaming | pending-frame coalescing | 30 Hz ceiling |\n\n"
                "Full uploads remain the authority after resize, state transition, "
                "large damage, or a failed patch. The cell renderer remains available "
                "through `--tui` and exercises the same provider, tool, and governance "
                "paths.",
    },
    {
        .role = "ASSISTANT",
        .detail = "result",
        .text = "The next optimization target is visible and measurable: reduce the "
                "95th-percentile raster time without lowering transcript capacity, "
                "then compare both renderers against this exact fixture.",
    },
};

typedef struct {
    int columns;
    int rows;
    int transcript_columns;
    int line_capacity;
    int wrapped_lines;
    int visible_lines;
    int visible_messages;
    size_t source_chars;
    size_t visible_chars;
} legacy_density_t;

static bool ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path, 0755) == 0;
}

static bool parity_path(char *dst, size_t cap, const char *directory,
                        const char *name) {
    int wrote = snprintf(dst, cap, "%s/%s", directory, name);
    return wrote > 0 && (size_t)wrote < cap;
}

static void json_string(FILE *out, const char *value) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(value ? value : "");
         *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20) fprintf(out, "\\u%04x", *p);
            else fputc(*p, out);
        }
    }
    fputc('"', out);
}

static bool write_fixture_json(const char *path) {
    FILE *out = fopen(path, "w");
    if (!out) return false;
    fputs("{\n  \"schema\": \"dsco.compositor_parity_fixture.v1\",\n", out);
    fputs("  \"model\": \"openai-codex/gpt-5.6-luna\",\n", out);
    fputs("  \"state\": \"responding\",\n  \"messages\": [\n", out);
    size_t count = sizeof(parity_messages) / sizeof(parity_messages[0]);
    for (size_t i = 0; i < count; i++) {
        fputs("    {\"role\": ", out);
        json_string(out, parity_messages[i].role);
        fputs(", \"detail\": ", out);
        json_string(out, parity_messages[i].detail);
        fputs(", \"text\": ", out);
        json_string(out, parity_messages[i].text);
        fputs(i + 1 < count ? "},\n" : "}\n", out);
    }
    fputs("  ],\n  \"input\": \"Measure P95 and keep both renderers first-class.\"\n}\n", out);
    bool ok = !ferror(out);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static bool write_legacy_ansi(const char *path) {
    FILE *out = fopen(path, "w");
    if (!out) return false;
    fprintf(out, "%s%sDSCO / AGENT WORKSPACE%s  %sopenai-codex/gpt-5.6-luna%s  "
                 "%sRESPONDING%s\n",
            TUI_BOLD, TUI_BCYAN, TUI_RESET, TUI_DIM, TUI_RESET,
            TUI_BGREEN, TUI_RESET);
    for (int i = 0; i < PARITY_LEGACY_COLS; i++) fputs("─", out);
    fputc('\n', out);
    size_t count = sizeof(parity_messages) / sizeof(parity_messages[0]);
    for (size_t i = 0; i < count; i++) {
        const pixel_tui_fixture_message_t *message = &parity_messages[i];
        const char *color = !strcmp(message->role, "USER") ? TUI_BCYAN :
                            !strcmp(message->role, "TOOL") ? TUI_BYELLOW :
                            TUI_BMAGENTA;
        fprintf(out, "%s%s%-9s%s %s%s%s\n", TUI_BOLD, color, message->role,
                TUI_RESET, TUI_DIM, message->detail, TUI_RESET);
        if (!strcmp(message->role, "ASSISTANT")) {
            md_renderer_t renderer;
            md_init(&renderer, out);
            renderer.term_width = PARITY_LEGACY_COLS - 2;
            md_feed_str(&renderer, message->text);
            md_flush(&renderer);
            md_reset(&renderer);
        } else {
            fprintf(out, "%s\n", message->text);
        }
        fputc('\n', out);
    }
    for (int i = 0; i < PARITY_LEGACY_COLS; i++) fputs("─", out);
    fprintf(out, "\n%s>%s Measure P95 and keep both renderers first-class.\n",
            TUI_BCYAN, TUI_RESET);
    bool ok = !ferror(out);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static bool write_plain_capture(const char *ansi_path, const char *plain_path) {
    FILE *in = fopen(ansi_path, "rb");
    FILE *out = fopen(plain_path, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return false;
    }
    enum { COPY, ESCAPE, CSI, STRING, STRING_ESC } state = COPY;
    int ch;
    while ((ch = fgetc(in)) != EOF) {
        unsigned char c = (unsigned char)ch;
        if (state == COPY) {
            if (c == 0x1b) state = ESCAPE;
            else fputc(c, out);
        } else if (state == ESCAPE) {
            if (c == '[') state = CSI;
            else if (c == ']' || c == '_' || c == 'P' || c == '^') state = STRING;
            else state = COPY;
        } else if (state == CSI) {
            if (c >= 0x40 && c <= 0x7e) state = COPY;
        } else if (state == STRING) {
            if (c == 0x07) state = COPY;
            else if (c == 0x1b) state = STRING_ESC;
        } else if (state == STRING_ESC) {
            state = c == '\\' ? COPY : STRING;
        }
    }
    bool ok = !ferror(in) && !ferror(out);
    if (fclose(in) != 0) ok = false;
    if (fclose(out) != 0) ok = false;
    return ok;
}

static legacy_density_t legacy_density(void) {
    legacy_density_t density = {
        .columns = PARITY_LEGACY_COLS,
        .rows = PARITY_LEGACY_ROWS,
        .transcript_columns = PARITY_LEGACY_COLS - 12,
        .line_capacity = PARITY_LEGACY_ROWS - 6,
    };
    int line_chars[PARITY_MAX_LINES] = {0};
    int line_message[PARITY_MAX_LINES] = {0};
    int line_count = 0;
    size_t message_count = sizeof(parity_messages) / sizeof(parity_messages[0]);
    for (size_t mi = 0; mi < message_count && line_count < PARITY_MAX_LINES; mi++) {
        const unsigned char *p = (const unsigned char *)parity_messages[mi].text;
        int used = (int)strlen(parity_messages[mi].detail);
        density.source_chars += (size_t)used;
        if (used > 0 && *p) {
            used += 5; /* native detail separator; legacy role header is equivalent */
            density.source_chars += 5;
        }
        line_message[line_count] = (int)mi + 1;
        while (*p && line_count < PARITY_MAX_LINES) {
            if (*p == '\n' || used >= density.transcript_columns) {
                line_chars[line_count] = used;
                line_count++;
                used = 0;
                if (line_count < PARITY_MAX_LINES)
                    line_message[line_count] = (int)mi + 1;
                if (*p == '\n') p++;
                continue;
            }
            if ((*p & 0xc0U) != 0x80U) {
                used++;
                density.source_chars++;
            }
            p++;
        }
        if (line_count < PARITY_MAX_LINES) {
            line_chars[line_count] = used;
            line_count++;
        }
    }
    density.wrapped_lines = line_count;
    density.visible_lines = line_count < density.line_capacity
                                ? line_count : density.line_capacity;
    int first = line_count - density.visible_lines;
    int prior_message = 0;
    for (int i = first; i < line_count; i++) {
        if (line_chars[i] > 0) density.visible_chars += (size_t)line_chars[i];
        if (line_message[i] != prior_message) {
            density.visible_messages++;
            prior_message = line_message[i];
        }
    }
    return density;
}

static bool write_metrics(const char *path,
                          const pixel_tui_density_metrics_t *native,
                          const legacy_density_t *legacy) {
    FILE *out = fopen(path, "w");
    if (!out) return false;
    double native_coverage = native->source_chars
        ? (double)native->visible_chars / (double)native->source_chars : 0.0;
    double legacy_coverage = legacy->source_chars
        ? (double)legacy->visible_chars / (double)legacy->source_chars : 0.0;
    double native_kpx = native->transcript_width > 0 && native->transcript_height > 0
        ? (double)native->visible_chars * 1000.0 /
          ((double)native->transcript_width * (double)native->transcript_height)
        : 0.0;
    double legacy_kcells = legacy->transcript_columns > 0 && legacy->line_capacity > 0
        ? (double)legacy->visible_chars * 1000.0 /
          ((double)legacy->transcript_columns * (double)legacy->line_capacity)
        : 0.0;
    int rc = fprintf(out,
        "{\n"
        "  \"schema\": \"dsco.compositor_parity_metrics.v1\",\n"
        "  \"source_chars\": %zu,\n"
        "  \"native\": {\"logical_width\": %d, \"logical_height\": %d, "
        "\"transcript_width\": %d, \"transcript_height\": %d, "
        "\"wrap_columns\": %d, \"line_capacity\": %d, "
        "\"wrapped_lines\": %d, \"visible_lines\": %d, "
        "\"visible_messages\": %d, \"visible_chars\": %zu, "
        "\"source_coverage\": %.6f, \"visible_chars_per_kpx\": %.6f},\n"
        "  \"established\": {\"columns\": %d, \"rows\": %d, "
        "\"transcript_columns\": %d, \"line_capacity\": %d, "
        "\"wrapped_lines\": %d, \"visible_lines\": %d, "
        "\"visible_messages\": %d, \"visible_chars\": %zu, "
        "\"source_coverage\": %.6f, \"visible_chars_per_kcells\": %.6f},\n"
        "  \"parity\": {\"message_count\": %zu, "
        "\"native_to_established_visible_char_ratio\": %.6f}\n"
        "}\n",
        native->source_chars,
        native->logical_width, native->logical_height,
        native->transcript_width, native->transcript_height,
        native->wrap_columns, native->line_capacity, native->wrapped_lines,
        native->visible_lines, native->visible_messages, native->visible_chars,
        native_coverage, native_kpx,
        legacy->columns, legacy->rows, legacy->transcript_columns,
        legacy->line_capacity, legacy->wrapped_lines, legacy->visible_lines,
        legacy->visible_messages, legacy->visible_chars, legacy_coverage,
        legacy_kcells, sizeof(parity_messages) / sizeof(parity_messages[0]),
        legacy->visible_chars
            ? (double)native->visible_chars / (double)legacy->visible_chars : 0.0);
    bool ok = rc >= 0 && !ferror(out);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static bool write_readme(const char *path) {
    FILE *out = fopen(path, "w");
    if (!out) return false;
    fputs("# DSCO dual-compositor parity corpus\n\n"
          "This directory is generated by the real established Markdown renderer "
          "and the real native session renderer from one semantic fixture.\n\n"
          "- `legacy.ansi`: ANSI/cell renderer capture.\n"
          "- `legacy.txt`: escape-free review copy of that capture.\n"
          "- `native.ppm`: headless native compositor screenshot.\n"
          "- `fixture.json`: shared messages, state, model, and composer input.\n"
          "- `metrics.json`: viewport capacity, visible text, and coverage.\n\n"
          "Regenerate with `dsco --compositor-parity <directory>`. Density values "
          "measure transcript text, excluding masthead and composer chrome. The two "
          "renderers intentionally keep their own layout idioms; parity is evaluated "
          "at the shared behavior and information boundary.\n", out);
    bool ok = !ferror(out);
    if (fclose(out) != 0) ok = false;
    return ok;
}

int compositor_parity_write(const char *directory, FILE *summary) {
    if (!directory || !*directory || !ensure_directory(directory)) {
        if (summary)
            fprintf(summary, "compositor parity: cannot create %s: %s\n",
                    directory ? directory : "(null)", strerror(errno));
        return 1;
    }
    char native_path[4096], ansi_path[4096], plain_path[4096];
    char fixture_path[4096], metrics_path[4096], readme_path[4096];
    if (!parity_path(native_path, sizeof(native_path), directory, "native.ppm") ||
        !parity_path(ansi_path, sizeof(ansi_path), directory, "legacy.ansi") ||
        !parity_path(plain_path, sizeof(plain_path), directory, "legacy.txt") ||
        !parity_path(fixture_path, sizeof(fixture_path), directory, "fixture.json") ||
        !parity_path(metrics_path, sizeof(metrics_path), directory, "metrics.json") ||
        !parity_path(readme_path, sizeof(readme_path), directory, "README.md")) {
        if (summary) fprintf(summary, "compositor parity: output path too long\n");
        return 1;
    }
    pixel_tui_fixture_t fixture = {
        .model = "openai-codex/gpt-5.6-luna",
        .slot_name = "native subscription",
        .state = PIXEL_TUI_RESPONDING,
        .messages = parity_messages,
        .message_count = (int)(sizeof(parity_messages) / sizeof(parity_messages[0])),
        .input = "Measure P95 and keep both renderers first-class.",
        .input_cursor = strlen("Measure P95 and keep both renderers first-class."),
        .input_active = false,
        .turn = 7,
        .input_tokens = 8421,
        .output_tokens = 1337,
        .tools_used = 4,
        .cost_usd = 0.0842,
        .context_percent = 38.0,
    };
    pixel_tui_density_metrics_t native = {0};
    bool ok = pixel_tui_write_fixture_ppm(native_path, PARITY_NATIVE_WIDTH,
                                          PARITY_NATIVE_HEIGHT, &fixture, &native);
    ok = write_legacy_ansi(ansi_path) && ok;
    ok = write_plain_capture(ansi_path, plain_path) && ok;
    ok = write_fixture_json(fixture_path) && ok;
    legacy_density_t legacy = legacy_density();
    ok = write_metrics(metrics_path, &native, &legacy) && ok;
    ok = write_readme(readme_path) && ok;
    if (summary) {
        fprintf(summary,
                "compositor parity: %s (%zu native chars / %zu established chars visible)\n",
                ok ? directory : "failed", native.visible_chars,
                legacy.visible_chars);
    }
    return ok ? 0 : 1;
}
