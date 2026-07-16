#include "px_theme.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Registry order is the /theme cycle order: the default first, then darks by
 * temperament, lights last so a cycling user never lands on a light surface
 * by surprise before seeing every dark option. */
static const px_theme_t k_themes[] = {
    {
        .name = "quiet-console",
        .display_name = "Quiet Console",
        .description = "Neutral graphite surfaces; color reserved for state",
        .light = false,
        .shadow_opacity = 0.22f,
        .bg_top = {12, 14, 16},
        .bg_bottom = {8, 10, 12},
        .panel = {17, 20, 23},
        .panel_alt = {21, 24, 28},
        .text = {205, 211, 216},
        .dim = {100, 108, 116},
        .accent = {105, 139, 153},
        .accent_alt = {111, 108, 122},
        .success = {108, 142, 122},
        .warning = {174, 142, 94},
        .danger = {166, 96, 99},
        .brand = {206, 32, 38},
        .chart = {{105, 139, 153},
                  {111, 108, 122},
                  {108, 142, 122},
                  {174, 142, 94},
                  {166, 96, 99},
                  {150, 150, 158}},
    },
    {
        .name = "distributed-crimson",
        .display_name = "Distributed Crimson",
        .description = "Brand blacks with the wordmark crimson as accent",
        .light = false,
        .shadow_opacity = 0.30f,
        .bg_top = {13, 9, 10},
        .bg_bottom = {8, 5, 6},
        .panel = {22, 15, 16},
        .panel_alt = {28, 18, 20},
        .text = {218, 206, 208},
        .dim = {122, 100, 103},
        .accent = {226, 68, 74},
        .accent_alt = {168, 84, 110},
        .success = {122, 156, 126},
        .warning = {206, 152, 88},
        .danger = {240, 84, 90},
        .brand = {232, 30, 36},
        .chart = {{226, 68, 74},
                  {168, 84, 110},
                  {206, 152, 88},
                  {122, 156, 126},
                  {150, 120, 170},
                  {170, 150, 152}},
    },
    {
        .name = "synthwave",
        .display_name = "Synthwave",
        .description = "Deep violet dusk, magenta and cyan neon",
        .light = false,
        .shadow_opacity = 0.48f,
        .bg_top = {24, 16, 38},
        .bg_bottom = {15, 10, 27},
        .panel = {34, 24, 52},
        .panel_alt = {42, 30, 63},
        .text = {236, 226, 250},
        .dim = {134, 118, 164},
        .accent = {255, 110, 199},
        .accent_alt = {96, 219, 255},
        .success = {118, 245, 178},
        .warning = {255, 199, 95},
        .danger = {255, 92, 122},
        .brand = {255, 62, 172},
        .chart = {{255, 110, 199},
                  {96, 219, 255},
                  {186, 128, 255},
                  {118, 245, 178},
                  {255, 199, 95},
                  {255, 140, 120}},
    },
    {
        .name = "matrix",
        .display_name = "Matrix",
        .description = "Green phosphor terminal, rain-dark surfaces",
        .light = false,
        .high_contrast = true,
        .shadow_opacity = 0.08f,
        .bg_top = {4, 11, 7},
        .bg_bottom = {2, 7, 4},
        .panel = {8, 19, 12},
        .panel_alt = {11, 24, 15},
        .text = {172, 232, 184},
        .dim = {74, 124, 88},
        .accent = {68, 255, 126},
        .accent_alt = {44, 186, 104},
        .success = {96, 240, 142},
        .warning = {214, 220, 96},
        .danger = {255, 112, 92},
        .brand = {44, 250, 112},
        .chart = {{68, 255, 126},
                  {44, 186, 104},
                  {160, 240, 120},
                  {214, 220, 96},
                  {96, 200, 170},
                  {140, 170, 148}},
    },
    {
        .name = "amber-crt",
        .display_name = "Amber CRT",
        .description = "Warm amber phosphor on smoked glass",
        .light = false,
        .shadow_opacity = 0.10f,
        .bg_top = {15, 11, 5},
        .bg_bottom = {10, 7, 3},
        .panel = {25, 18, 9},
        .panel_alt = {31, 22, 11},
        .text = {236, 192, 122},
        .dim = {142, 112, 72},
        .accent = {255, 176, 48},
        .accent_alt = {228, 138, 60},
        .success = {182, 208, 110},
        .warning = {255, 204, 84},
        .danger = {255, 112, 82},
        .brand = {255, 168, 40},
        .chart = {{255, 176, 48},
                  {228, 138, 60},
                  {236, 214, 128},
                  {182, 208, 110},
                  {216, 128, 96},
                  {180, 156, 120}},
    },
    {
        .name = "nord",
        .display_name = "Nord",
        .description = "Arctic blue-greys, frost accents",
        .light = false,
        .shadow_opacity = 0.28f,
        .bg_top = {36, 41, 51},
        .bg_bottom = {30, 34, 43},
        .panel = {46, 52, 64},
        .panel_alt = {59, 66, 82},
        .text = {216, 222, 233},
        .dim = {128, 140, 156},
        .accent = {136, 192, 208},
        .accent_alt = {180, 142, 173},
        .success = {163, 190, 140},
        .warning = {235, 203, 139},
        .danger = {191, 97, 106},
        .brand = {136, 192, 208},
        .chart = {{136, 192, 208},
                  {129, 161, 193},
                  {180, 142, 173},
                  {163, 190, 140},
                  {235, 203, 139},
                  {208, 135, 112}},
    },
    {
        .name = "solarized-light",
        .display_name = "Solarized Light",
        .description = "Classic warm paper with solar accents",
        .light = true,
        .shadow_opacity = 0.15f,
        .bg_top = {253, 246, 227},
        .bg_bottom = {245, 237, 214},
        .panel = {238, 232, 213},
        .panel_alt = {230, 223, 201},
        .text = {73, 89, 97},
        .dim = {136, 152, 152},
        .accent = {38, 139, 210},
        .accent_alt = {108, 113, 196},
        .success = {133, 153, 0},
        .warning = {181, 137, 0},
        .danger = {220, 50, 47},
        .brand = {203, 75, 22},
        .chart = {{38, 139, 210},
                  {108, 113, 196},
                  {133, 153, 0},
                  {181, 137, 0},
                  {211, 54, 130},
                  {42, 161, 152}},
    },
    {
        .name = "paper",
        .display_name = "Paper",
        .description = "Bright gallery white, restrained ink accents",
        .light = true,
        .shadow_opacity = 0.16f,
        .bg_top = {250, 250, 248},
        .bg_bottom = {242, 242, 238},
        .panel = {255, 255, 255},
        .panel_alt = {246, 246, 243},
        .text = {40, 44, 48},
        .dim = {128, 134, 140},
        .accent = {66, 108, 214},
        .accent_alt = {146, 88, 196},
        .success = {46, 138, 88},
        .warning = {186, 128, 38},
        .danger = {198, 58, 58},
        .brand = {182, 38, 48},
        .chart = {{66, 108, 214},
                  {146, 88, 196},
                  {46, 138, 88},
                  {186, 128, 38},
                  {198, 58, 58},
                  {96, 104, 112}},
    },
    {
        .name = "native-glass",
        .display_name = "Native Glass",
        .description = "Soft depth, cool signal, contextual surfaces",
        .light = false,
        .shadow_opacity = 0.42f,
        .bg_top = {6, 12, 17}, .bg_bottom = {4, 9, 13},
        .panel = {12, 26, 35}, .panel_alt = {21, 43, 55},
        .text = {222, 237, 243}, .dim = {119, 153, 167},
        .accent = {79, 188, 218}, .accent_alt = {153, 108, 245},
        .success = {92, 204, 158}, .warning = {239, 181, 84},
        .danger = {241, 112, 125}, .brand = {79, 188, 218},
        .chart = {{79,188,218},{153,108,245},{92,204,158},
                  {239,181,84},{241,112,125},{124,173,190}},
    },
    {
        .name = "industrial",
        .display_name = "Industrial",
        .description = "Hard grid, blunt hierarchy, safety amber",
        .light = false,
        .shadow_opacity = 0.12f,
        .bg_top = {13,12,10}, .bg_bottom = {9,8,7},
        .panel = {27,26,23}, .panel_alt = {39,37,32},
        .text = {239,235,222}, .dim = {158,153,139},
        .accent = {255,177,35}, .accent_alt = {232,113,53},
        .success = {117,196,119}, .warning = {255,177,35},
        .danger = {233,94,76}, .brand = {255,177,35},
        .chart = {{255,177,35},{232,113,53},{117,196,119},
                  {238,211,99},{233,94,76},{168,157,132}},
    },
    {
        .name = "warm-studio",
        .display_name = "Warm Studio",
        .description = "Human, conversational, tactile warmth",
        .light = false,
        .shadow_opacity = 0.32f,
        .bg_top = {27,17,15}, .bg_bottom = {20,12,11},
        .panel = {43,28,25}, .panel_alt = {60,39,34},
        .text = {244,225,206}, .dim = {177,139,120},
        .accent = {231,133,91}, .accent_alt = {193,94,116},
        .success = {132,192,133}, .warning = {235,180,91},
        .danger = {230,102,88}, .brand = {231,133,91},
        .chart = {{231,133,91},{193,94,116},{132,192,133},
                  {235,180,91},{230,102,88},{176,142,122}},
    },
    {
        .name = "blueprint",
        .display_name = "Blueprint",
        .description = "Technical, diagrammatic, precise cyan lines",
        .light = false,
        .shadow_opacity = 0.10f,
        .bg_top = {2,18,31}, .bg_bottom = {1,13,23},
        .panel = {4,28,46}, .panel_alt = {6,39,62},
        .text = {190,232,245}, .dim = {96,160,182},
        .accent = {49,191,225}, .accent_alt = {99,130,235},
        .success = {88,206,164}, .warning = {240,180,74},
        .danger = {240,100,111}, .brand = {49,191,225},
        .chart = {{49,191,225},{99,130,235},{88,206,164},
                  {240,180,74},{240,100,111},{108,174,198}},
    },
    {
        .name = "spatial-command",
        .display_name = "Spatial Command",
        .description = "Floating controls and focused telemetry",
        .light = false,
        .shadow_opacity = 0.52f,
        .bg_top = {10,6,18}, .bg_bottom = {6,4,13},
        .panel = {19,13,31}, .panel_alt = {31,21,49},
        .text = {234,225,250}, .dim = {137,119,162},
        .accent = {153,108,245}, .accent_alt = {72,199,211},
        .success = {67,205,173}, .warning = {239,177,73},
        .danger = {241,102,141}, .brand = {153,108,245},
        .chart = {{153,108,245},{72,199,211},{67,205,173},
                  {239,177,73},{241,102,141},{145,132,190}},
    },
    {
        .name = "carbon",
        .display_name = "Carbon",
        .description = "Graphite discipline with a clear blue signal",
        .light = false,
        .shadow_opacity = 0.28f,
        .bg_top = {12,14,17}, .bg_bottom = {8,10,12},
        .panel = {22,25,29}, .panel_alt = {31,35,40},
        .text = {231,234,236}, .dim = {139,148,156},
        .accent = {112,188,255}, .accent_alt = {145,127,232},
        .success = {89,201,145}, .warning = {237,183,83},
        .danger = {237,104,111}, .brand = {112,188,255},
        .chart = {{112,188,255},{145,127,232},{89,201,145},
                  {237,183,83},{237,104,111},{147,161,174}},
    },
    {
        .name = "oceanic",
        .display_name = "Oceanic",
        .description = "Pelagic calm with crisp operational signals",
        .light = false,
        .shadow_opacity = 0.38f,
        .bg_top = {4,18,24}, .bg_bottom = {2,13,18},
        .panel = {9,31,40}, .panel_alt = {15,45,57},
        .text = {219,240,239}, .dim = {110,158,160},
        .accent = {36,190,181}, .accent_alt = {70,142,218},
        .success = {81,205,151}, .warning = {239,183,75},
        .danger = {237,99,105}, .brand = {36,190,181},
        .chart = {{36,190,181},{70,142,218},{81,205,151},
                  {239,183,75},{237,99,105},{110,166,171}},
    },
    {
        .name = "high-contrast",
        .display_name = "High Contrast",
        .description = "Maximum legibility and zero state ambiguity",
        .light = false,
        .high_contrast = true,
        .shadow_opacity = 0.0f,
        .bg_top = {0,0,0}, .bg_bottom = {0,0,0},
        .panel = {10,10,10}, .panel_alt = {22,22,22},
        .text = {255,255,255}, .dim = {192,192,192},
        .accent = {0,225,255}, .accent_alt = {188,132,255},
        .success = {68,255,132}, .warning = {255,218,58},
        .danger = {255,78,96}, .brand = {255,255,255},
        .chart = {{0,225,255},{188,132,255},{68,255,132},
                  {255,218,58},{255,78,96},{210,210,210}},
    },
};

#define PX_THEME_COUNT ((int)(sizeof(k_themes) / sizeof(k_themes[0])))

static int s_active = -1; /* -1: env not consulted yet */

int px_theme_count(void) {
    return PX_THEME_COUNT;
}

const px_theme_t *px_theme_get(int index) {
    if (index < 0 || index >= PX_THEME_COUNT) return NULL;
    return &k_themes[index];
}

static int theme_index(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < PX_THEME_COUNT; i++)
        if (!strcasecmp(k_themes[i].name, name)) return i;
    return -1;
}

const px_theme_t *px_theme_find(const char *name) {
    int at = theme_index(name);
    return at >= 0 ? &k_themes[at] : NULL;
}

const px_theme_t *px_theme_active(void) {
    if (s_active < 0) {
        int at = theme_index(getenv("DSCO_PIXEL_THEME"));
        if (at < 0) at = theme_index(getenv("DSCO_THEME"));
        s_active = at >= 0 ? at : 0;
    }
    return &k_themes[s_active];
}

const px_theme_t *px_theme_set_active(const char *name) {
    int at = theme_index(name);
    if (at >= 0) s_active = at;
    return px_theme_active();
}

const px_theme_t *px_theme_cycle(int direction) {
    px_theme_active(); /* resolve the env default before stepping */
    int step = direction < 0 ? -1 : 1;
    s_active = (s_active + step + PX_THEME_COUNT) % PX_THEME_COUNT;
    return &k_themes[s_active];
}

px_backend_palette_t px_theme_palette(const px_theme_t *theme) {
    if (!theme) theme = px_theme_active();
    px_backend_palette_t palette = {0};
    palette.colors[NATIVE_UI_COLOR_CLEAR] = theme->bg_top;
    palette.colors[NATIVE_UI_COLOR_CANVAS] = theme->bg_top;
    palette.colors[NATIVE_UI_COLOR_SURFACE] = theme->panel;
    palette.colors[NATIVE_UI_COLOR_SURFACE_RAISED] = theme->panel_alt;
    palette.colors[NATIVE_UI_COLOR_TEXT] = theme->text;
    palette.colors[NATIVE_UI_COLOR_TEXT_MUTED] = theme->dim;
    palette.colors[NATIVE_UI_COLOR_ACCENT] = theme->accent;
    palette.colors[NATIVE_UI_COLOR_SUCCESS] = theme->success;
    palette.colors[NATIVE_UI_COLOR_WARNING] = theme->warning;
    palette.colors[NATIVE_UI_COLOR_DANGER] = theme->danger;
    palette.colors[NATIVE_UI_COLOR_BORDER] = theme->dim;
    palette.colors[NATIVE_UI_COLOR_FOCUS] = theme->accent;
    return palette;
}
