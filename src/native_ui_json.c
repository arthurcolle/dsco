#include "native_ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Declarative scene builder ───────────────────────────────────────────
 * Compiles a JSON UI specification into a retained native_ui scene. This is
 * the generative-UI entry point: an agent (or any producer) emits structure
 * as data, the compositor owns layout, diffing, focus, and rendering. The
 * parser is a bounded recursive-descent walk over exactly the spec shape —
 * no allocation, no reliance on key-scanning helpers that can bleed across
 * sibling objects.
 *
 * Spec (all fields optional except element):
 *   { "element": "stack", "role": "plan", "key": "id", "text": "...",
 *     "value": 0.4, "label": "a11y",
 *     "style": { "fg": "accent", "bg": "surface", "border": "border",
 *                "pad": 8, "gap": 6, "radius": 8, "border_width": 1,
 *                "align": "center", "justify": "start", "columns": 2,
 *                "z": 1, "opacity": 0.9, "type": "title" },
 *     "size": { "w": 200, "h": -1, "min_w": 0, "max_w": -1,
 *               "grow": 1, "shrink": 1 },
 *     "children": [ ... ] }
 */

#define SCENE_JSON_MAX_DEPTH 8

typedef struct {
    const char *at;
    bool failed;
} scene_parser_t;

static void parser_fail(scene_parser_t *p) { p->failed = true; }

static void skip_ws(scene_parser_t *p) {
    while (*p->at == ' ' || *p->at == '\t' || *p->at == '\n' || *p->at == '\r')
        p->at++;
}

static bool consume(scene_parser_t *p, char ch) {
    skip_ws(p);
    if (*p->at != ch) return false;
    p->at++;
    return true;
}

/* Parse a JSON string into buf (escapes resolved, truncating). */
static bool parse_string(scene_parser_t *p, char *buf, size_t cap) {
    skip_ws(p);
    if (*p->at != '"') return false;
    p->at++;
    size_t n = 0;
    while (*p->at && *p->at != '"') {
        char ch = *p->at++;
        if (ch == '\\' && *p->at) {
            char esc = *p->at++;
            switch (esc) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'u': {
                /* Keep the BMP scalar as UTF-8; surrogate pairs collapse to
                 * '?', which is acceptable for spec labels. */
                unsigned code = 0;
                for (int i = 0; i < 4 && *p->at; i++) {
                    char hex = *p->at++;
                    code <<= 4;
                    if (hex >= '0' && hex <= '9') code |= (unsigned)(hex - '0');
                    else if (hex >= 'a' && hex <= 'f') code |= (unsigned)(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') code |= (unsigned)(hex - 'A' + 10);
                    else return false;
                }
                if (code < 0x80) {
                    ch = (char)code;
                } else if (code < 0x800) {
                    if (buf && n + 2 < cap) {
                        buf[n++] = (char)(0xc0 | (code >> 6));
                        buf[n++] = (char)(0x80 | (code & 0x3f));
                    }
                    continue;
                } else {
                    if (buf && n + 3 < cap) {
                        buf[n++] = (char)(0xe0 | (code >> 12));
                        buf[n++] = (char)(0x80 | ((code >> 6) & 0x3f));
                        buf[n++] = (char)(0x80 | (code & 0x3f));
                    }
                    continue;
                }
                break;
            }
            default: ch = esc; break;
            }
        }
        if (buf && n + 1 < cap) buf[n++] = ch;
    }
    if (buf && cap > 0) buf[n] = '\0';
    if (*p->at != '"') return false;
    p->at++;
    return true;
}

static bool parse_number(scene_parser_t *p, double *out) {
    skip_ws(p);
    char *end = NULL;
    double value = strtod(p->at, &end);
    if (end == p->at) return false;
    p->at = end;
    if (out) *out = value;
    return true;
}

/* Skip any JSON value (bounded nesting). */
static void skip_value(scene_parser_t *p, int depth) {
    skip_ws(p);
    if (depth > SCENE_JSON_MAX_DEPTH + 4) {
        parser_fail(p);
        return;
    }
    if (*p->at == '"') {
        (void)parse_string(p, NULL, 0);
        return;
    }
    if (*p->at == '{' || *p->at == '[') {
        char open = *p->at, close = open == '{' ? '}' : ']';
        p->at++;
        skip_ws(p);
        if (*p->at == close) {
            p->at++;
            return;
        }
        for (;;) {
            if (open == '{') {
                if (!parse_string(p, NULL, 0) || !consume(p, ':')) {
                    parser_fail(p);
                    return;
                }
            }
            skip_value(p, depth + 1);
            if (p->failed) return;
            skip_ws(p);
            if (*p->at == ',') {
                p->at++;
                continue;
            }
            if (*p->at == close) {
                p->at++;
                return;
            }
            parser_fail(p);
            return;
        }
    }
    if (!strncmp(p->at, "true", 4)) { p->at += 4; return; }
    if (!strncmp(p->at, "false", 5)) { p->at += 5; return; }
    if (!strncmp(p->at, "null", 4)) { p->at += 4; return; }
    double ignored;
    if (!parse_number(p, &ignored)) parser_fail(p);
}

/* ── vocabulary ──────────────────────────────────────────────────────── */

static native_ui_element_t element_from_name(const char *name) {
    static const struct { const char *name; native_ui_element_t element; } map[] = {
        {"root", NATIVE_UI_ELEMENT_ROOT}, {"surface", NATIVE_UI_ELEMENT_SURFACE},
        {"stack", NATIVE_UI_ELEMENT_STACK}, {"row", NATIVE_UI_ELEMENT_ROW},
        {"grid", NATIVE_UI_ELEMENT_GRID}, {"overlay", NATIVE_UI_ELEMENT_OVERLAY},
        {"scroll", NATIVE_UI_ELEMENT_SCROLL}, {"text", NATIVE_UI_ELEMENT_TEXT},
        {"icon", NATIVE_UI_ELEMENT_ICON}, {"rule", NATIVE_UI_ELEMENT_RULE},
        {"badge", NATIVE_UI_ELEMENT_BADGE}, {"meter", NATIVE_UI_ELEMENT_METER},
        {"sparkline", NATIVE_UI_ELEMENT_SPARKLINE}, {"input", NATIVE_UI_ELEMENT_INPUT},
        {"image", NATIVE_UI_ELEMENT_IMAGE}, {"custom", NATIVE_UI_ELEMENT_CUSTOM},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(name, map[i].name)) return map[i].element;
    return NATIVE_UI_ELEMENT_SURFACE;
}

static native_ui_role_t role_from_name(const char *name) {
    for (int role = NATIVE_UI_ROLE_NONE; role <= NATIVE_UI_ROLE_SCRIM; role++)
        if (!strcmp(name, native_ui_role_name((native_ui_role_t)role)))
            return (native_ui_role_t)role;
    return NATIVE_UI_ROLE_NONE;
}

static native_ui_color_token_t color_from_name(const char *name) {
    static const struct { const char *name; native_ui_color_token_t token; } map[] = {
        {"clear", NATIVE_UI_COLOR_CLEAR}, {"canvas", NATIVE_UI_COLOR_CANVAS},
        {"surface", NATIVE_UI_COLOR_SURFACE},
        {"surface-raised", NATIVE_UI_COLOR_SURFACE_RAISED},
        {"text", NATIVE_UI_COLOR_TEXT}, {"muted", NATIVE_UI_COLOR_TEXT_MUTED},
        {"accent", NATIVE_UI_COLOR_ACCENT}, {"success", NATIVE_UI_COLOR_SUCCESS},
        {"warning", NATIVE_UI_COLOR_WARNING}, {"danger", NATIVE_UI_COLOR_DANGER},
        {"border", NATIVE_UI_COLOR_BORDER}, {"focus", NATIVE_UI_COLOR_FOCUS},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(name, map[i].name)) return map[i].token;
    return NATIVE_UI_COLOR_TEXT;
}

static native_ui_type_token_t type_from_name(const char *name) {
    if (!strcmp(name, "label")) return NATIVE_UI_TYPE_LABEL;
    if (!strcmp(name, "title")) return NATIVE_UI_TYPE_TITLE;
    if (!strcmp(name, "code")) return NATIVE_UI_TYPE_CODE;
    if (!strcmp(name, "math")) return NATIVE_UI_TYPE_MATH;
    if (!strcmp(name, "metric")) return NATIVE_UI_TYPE_METRIC;
    return NATIVE_UI_TYPE_BODY;
}

static native_ui_align_t align_from_name(const char *name) {
    if (!strcmp(name, "center")) return NATIVE_UI_ALIGN_CENTER;
    if (!strcmp(name, "end")) return NATIVE_UI_ALIGN_END;
    if (!strcmp(name, "stretch")) return NATIVE_UI_ALIGN_STRETCH;
    return NATIVE_UI_ALIGN_START;
}

static uint64_t fnv1a(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* ── node builder ────────────────────────────────────────────────────── */

static void apply_style_field(native_ui_node_t *node, scene_parser_t *p,
                              const char *field) {
    char name[48];
    double number = 0.0;
    if (!strcmp(field, "fg") || !strcmp(field, "bg") ||
        !strcmp(field, "border") || !strcmp(field, "type") ||
        !strcmp(field, "align") || !strcmp(field, "justify")) {
        if (!parse_string(p, name, sizeof(name))) {
            parser_fail(p);
            return;
        }
        if (!strcmp(field, "fg")) node->style.foreground = color_from_name(name);
        else if (!strcmp(field, "bg")) node->style.background = color_from_name(name);
        else if (!strcmp(field, "border")) node->style.border = color_from_name(name);
        else if (!strcmp(field, "type")) node->style.type = type_from_name(name);
        else if (!strcmp(field, "align")) node->style.align = align_from_name(name);
        else node->style.justify = align_from_name(name);
        return;
    }
    if (!strcmp(field, "pad")) {
        skip_ws(p);
        if (*p->at == '[') {
            p->at++;
            double edges[4] = {0, 0, 0, 0};
            for (int i = 0; i < 4; i++) {
                if (!parse_number(p, &edges[i])) {
                    parser_fail(p);
                    return;
                }
                skip_ws(p);
                if (*p->at == ',') p->at++;
            }
            if (!consume(p, ']')) {
                parser_fail(p);
                return;
            }
            node->style.padding = (native_ui_insets_t){
                (int)edges[0], (int)edges[1], (int)edges[2], (int)edges[3]};
            return;
        }
        if (!parse_number(p, &number)) {
            parser_fail(p);
            return;
        }
        int pad = (int)number;
        node->style.padding = (native_ui_insets_t){pad, pad, pad, pad};
        return;
    }
    if (!parse_number(p, &number)) {
        parser_fail(p);
        return;
    }
    if (!strcmp(field, "gap")) node->style.gap = (int)number;
    else if (!strcmp(field, "radius")) node->style.radius = (uint8_t)(number < 0 ? 0 : number);
    else if (!strcmp(field, "border_width")) node->style.border_width = (uint8_t)(number < 0 ? 0 : number);
    else if (!strcmp(field, "columns")) node->style.grid_columns = (int)number;
    else if (!strcmp(field, "z")) node->style.z_index = (int)number;
    else if (!strcmp(field, "opacity"))
        node->style.opacity = number <= 1.0
                                  ? (uint8_t)(number * 255.0 + 0.5)
                                  : (uint8_t)(number > 255.0 ? 255.0 : number);
}

static void apply_size_field(native_ui_node_t *node, scene_parser_t *p,
                             const char *field) {
    double number = 0.0;
    if (!parse_number(p, &number)) {
        parser_fail(p);
        return;
    }
    int value = (int)number;
    if (!strcmp(field, "w")) node->constraints.preferred_width = value;
    else if (!strcmp(field, "h")) node->constraints.preferred_height = value;
    else if (!strcmp(field, "min_w")) node->constraints.min_width = value;
    else if (!strcmp(field, "min_h")) node->constraints.min_height = value;
    else if (!strcmp(field, "max_w")) node->constraints.max_width = value;
    else if (!strcmp(field, "max_h")) node->constraints.max_height = value;
    else if (!strcmp(field, "grow")) node->constraints.grow = (uint16_t)(value < 0 ? 0 : value);
    else if (!strcmp(field, "shrink")) node->constraints.shrink = (uint16_t)(value < 0 ? 0 : value);
}

static void parse_flat_object(native_ui_node_t *node, scene_parser_t *p,
                              void (*apply)(native_ui_node_t *,
                                            scene_parser_t *, const char *)) {
    if (!consume(p, '{')) {
        parser_fail(p);
        return;
    }
    skip_ws(p);
    if (*p->at == '}') {
        p->at++;
        return;
    }
    for (;;) {
        char field[32];
        if (!parse_string(p, field, sizeof(field)) || !consume(p, ':')) {
            parser_fail(p);
            return;
        }
        apply(node, p, field);
        if (p->failed) return;
        skip_ws(p);
        if (*p->at == ',') {
            p->at++;
            continue;
        }
        if (*p->at == '}') {
            p->at++;
            return;
        }
        parser_fail(p);
        return;
    }
}

static bool parse_node(native_ui_scene_t *scene, scene_parser_t *p,
                       int parent, uint64_t path_key, int depth);

static bool parse_children(native_ui_scene_t *scene, scene_parser_t *p,
                           int parent, uint64_t path_key, int depth) {
    if (!consume(p, '[')) {
        parser_fail(p);
        return false;
    }
    skip_ws(p);
    if (*p->at == ']') {
        p->at++;
        return true;
    }
    for (int ordinal = 0;; ordinal++) {
        uint64_t child_key = fnv1a(path_key, &ordinal, sizeof(ordinal));
        if (!parse_node(scene, p, parent, child_key, depth + 1)) return false;
        skip_ws(p);
        if (*p->at == ',') {
            p->at++;
            continue;
        }
        if (*p->at == ']') {
            p->at++;
            return true;
        }
        parser_fail(p);
        return false;
    }
}

static bool parse_node(native_ui_scene_t *scene, scene_parser_t *p,
                       int parent, uint64_t path_key, int depth) {
    if (depth > SCENE_JSON_MAX_DEPTH) {
        parser_fail(p);
        return false;
    }
    if (!consume(p, '{')) {
        parser_fail(p);
        return false;
    }
    /* The node must exist before children parse, so key/element/role fields
     * appearing after "children" still apply: create first, configure as
     * fields stream in. Explicit keys therefore re-key the node in place. */
    uint64_t key = path_key ? path_key : UINT64_C(0x9e3779b97f4a7c15);
    int index = native_ui_scene_add(scene, parent, key,
                                    NATIVE_UI_ELEMENT_SURFACE,
                                    NATIVE_UI_ROLE_NONE);
    if (index < 0) {
        parser_fail(p);
        return false;
    }
    skip_ws(p);
    if (*p->at == '}') {
        p->at++;
        return true;
    }
    for (;;) {
        char field[32];
        if (!parse_string(p, field, sizeof(field)) || !consume(p, ':')) {
            parser_fail(p);
            return false;
        }
        native_ui_node_t *node = native_ui_scene_node(scene, index);
        if (!strcmp(field, "element")) {
            char name[32];
            if (!parse_string(p, name, sizeof(name))) {
                parser_fail(p);
                return false;
            }
            node->element = element_from_name(name);
            node->style = (native_ui_style_t){0};
            /* Re-derive flow/typography defaults for the real element. */
            native_ui_scene_t probe;
            native_ui_scene_init(&probe, 0, 0);
            int probe_index = native_ui_scene_add(&probe, 0, 2, node->element,
                                                  node->role);
            if (probe_index > 0) node->style = probe.nodes[probe_index].style;
        } else if (!strcmp(field, "role")) {
            char name[32];
            if (!parse_string(p, name, sizeof(name))) {
                parser_fail(p);
                return false;
            }
            node->role = role_from_name(name);
        } else if (!strcmp(field, "key")) {
            char name[64];
            if (!parse_string(p, name, sizeof(name))) {
                parser_fail(p);
                return false;
            }
            uint64_t explicit_key =
                fnv1a(UINT64_C(1469598103934665603), name, strlen(name));
            if (explicit_key == 0) explicit_key = 1;
            bool taken = false;
            for (int i = 0; i < scene->count; i++)
                if (i != index && scene->nodes[i].key == explicit_key)
                    taken = true;
            if (!taken) node->key = explicit_key;
        } else if (!strcmp(field, "text")) {
            char text[NATIVE_UI_TEXT_CAP];
            if (!parse_string(p, text, sizeof(text))) {
                parser_fail(p);
                return false;
            }
            native_ui_node_set_text(node, text);
        } else if (!strcmp(field, "label")) {
            char label[NATIVE_UI_LABEL_CAP];
            if (!parse_string(p, label, sizeof(label))) {
                parser_fail(p);
                return false;
            }
            native_ui_node_set_accessibility_label(node, label);
        } else if (!strcmp(field, "value")) {
            double value = 0.0;
            if (!parse_number(p, &value)) {
                parser_fail(p);
                return false;
            }
            node->value = (float)value;
        } else if (!strcmp(field, "focusable")) {
            skip_ws(p);
            if (!strncmp(p->at, "true", 4)) {
                node->state |= NATIVE_UI_STATE_FOCUSABLE;
                p->at += 4;
            } else if (!strncmp(p->at, "false", 5)) {
                node->state &= ~(uint32_t)NATIVE_UI_STATE_FOCUSABLE;
                p->at += 5;
            } else {
                parser_fail(p);
                return false;
            }
        } else if (!strcmp(field, "style")) {
            parse_flat_object(node, p, apply_style_field);
            if (p->failed) return false;
        } else if (!strcmp(field, "size")) {
            parse_flat_object(node, p, apply_size_field);
            if (p->failed) return false;
        } else if (!strcmp(field, "children")) {
            /* A flowless container (surface/scroll/custom) would never lay
             * out its children; declared children imply a column unless the
             * spec chose a flow. This keeps "card with contents" the obvious
             * one-liner it should be. */
            if (node->style.flow == NATIVE_UI_FLOW_NONE)
                node->style.flow = NATIVE_UI_FLOW_COLUMN;
            uint64_t base = native_ui_scene_node(scene, index)->key;
            if (!parse_children(scene, p, index, base, depth)) return false;
        } else {
            skip_value(p, depth);
            if (p->failed) return false;
        }
        skip_ws(p);
        if (*p->at == ',') {
            p->at++;
            continue;
        }
        if (*p->at == '}') {
            p->at++;
            return true;
        }
        parser_fail(p);
        return false;
    }
}

int native_ui_scene_from_json(native_ui_scene_t *scene, const char *json,
                              int width, int height) {
    if (!scene || !json) return -1;
    native_ui_scene_init(scene, width, height);
    scene_parser_t parser = {.at = json, .failed = false};
    skip_ws(&parser);
    if (*parser.at != '{') return -1;
    uint64_t top_key = fnv1a(UINT64_C(1469598103934665603), "scene", 5);
    if (!parse_node(scene, &parser, scene->root, top_key, 0) || parser.failed)
        return -1;
    skip_ws(&parser);
    if (*parser.at != '\0') return -1;
    /* The generated panel fills the viewport unless it sized itself. */
    native_ui_node_t *top = native_ui_scene_node(scene, 1);
    if (top && top->constraints.grow == 0 &&
        top->constraints.preferred_height < 0)
        top->constraints.grow = 1;
    native_ui_layout(scene);
    return scene->count;
}
