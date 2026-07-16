#include "agent_ui_canvas.h"
#include "agent_ui_components.h"
#include "agent_ui_gallery.h"
#include "agent_ui_theme.h"
#include "native_ui.h"
#include "px_theme.h"
#include "px_widgets.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int passes = 0;
static int failures = 0;

#define CHECK(condition, ...) do { \
    if (condition) passes++; \
    else { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
           fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

static const native_ui_node_t *find_key(const native_ui_scene_t *scene,
                                        uint64_t key) {
    if (!scene) return NULL;
    for (int i = 0; i < scene->count; i++)
        if (scene->nodes[i].key == key) return &scene->nodes[i];
    return NULL;
}

static bool frame_inside(native_ui_rect_t frame, native_ui_rect_t viewport) {
    return frame.x >= viewport.x && frame.y >= viewport.y && frame.width >= 0 &&
           frame.height >= 0 && frame.x + frame.width <= viewport.x + viewport.width &&
           frame.y + frame.height <= viewport.y + viewport.height;
}

static void test_theme_catalog(void) {
    CHECK(agent_ui_theme_count() == 16, "theme catalog should contain 16 tastes");
    CHECK(px_theme_count() == 16,
          "live compositor and component gallery should share 16 themes");
    for (size_t i = 0; i < agent_ui_theme_count(); i++) {
        const agent_ui_theme_t *theme = agent_ui_theme_at(i);
        char message[128];
        CHECK(theme && agent_ui_theme_validate(theme, message, sizeof(message)),
              "theme %zu should validate: %s", i, message);
        CHECK(theme && agent_ui_theme_contrast(
              theme->palette.colors[NATIVE_UI_COLOR_TEXT],
              theme->palette.colors[NATIVE_UI_COLOR_CANVAS]) >= 4.5,
              "theme %zu should preserve primary text contrast", i);
        CHECK(theme && agent_ui_theme_find(theme->id) == theme,
              "theme id should round-trip through catalog lookup");
        const px_theme_t *pixel_theme = px_theme_get((int)i);
        CHECK(theme && theme->pixel_theme == pixel_theme && pixel_theme &&
              !strcmp(theme->id, pixel_theme->name) &&
              theme->palette.colors[NATIVE_UI_COLOR_ACCENT].r ==
                  pixel_theme->accent.r,
              "component theme %zu should adapt the live pixel theme", i);
        for (size_t j = i + 1; j < agent_ui_theme_count(); j++)
            CHECK(strcmp(theme->id, agent_ui_theme_at(j)->id) != 0,
                  "theme ids should be unique");
    }
    const agent_ui_theme_t *first = agent_ui_theme_at(0);
    const agent_ui_theme_t *last = agent_ui_theme_at(agent_ui_theme_count()-1);
    CHECK(agent_ui_theme_next(first, -1) == last &&
          agent_ui_theme_next(last, 1) == first,
          "theme navigation should wrap in both directions");
}

static void test_foundation_widgets(void) {
    native_ui_scene_t scene;
    native_ui_scene_init(&scene, 1000, 720);
    native_ui_node_t *root = native_ui_scene_node(&scene, scene.root);
    root->style.flow = NATIVE_UI_FLOW_GRID;
    root->style.grid_columns = 3;
    root->style.padding = (native_ui_insets_t){12,12,12,12};
    root->style.gap = 10;
    double series[] = {2,5,3,8,6,11,9};
    CHECK(px_widget_card(&scene,scene.root,UINT64_C(0x1001),"Foundation card")>0,
          "foundation card should build");
    CHECK(px_widget_kv_row(&scene,scene.root,UINT64_C(0x1002),"MODEL","gpt-5.6")>0,
          "key/value row should build");
    CHECK(px_widget_badge(&scene,scene.root,UINT64_C(0x1003),"HEALTHY",PX_WIDGET_TONE_SUCCESS)>0,
          "badge should build");
    CHECK(px_widget_meter(&scene,scene.root,UINT64_C(0x1004),"CONTEXT",0.62,PX_WIDGET_TONE_ACCENT)>0,
          "meter should build");
    CHECK(px_widget_sparkline(&scene,scene.root,UINT64_C(0x1005),series,7,PX_WIDGET_TONE_SUCCESS)>0,
          "sparkline should build");
    CHECK(px_widget_progress(&scene,scene.root,UINT64_C(0x1006),-1.0,0.4,PX_WIDGET_TONE_ACCENT)>0,
          "indeterminate progress should build");
    CHECK(px_widget_gauge(&scene,scene.root,UINT64_C(0x1007),"68%",0.68,PX_WIDGET_TONE_WARNING)>0,
          "gauge should build");
    CHECK(px_widget_bar_chart(&scene,scene.root,UINT64_C(0x1008),series,7,PX_WIDGET_TONE_ACCENT)>0,
          "bar chart should build");
    CHECK(px_widget_spinner(&scene,scene.root,UINT64_C(0x1009),0.3,PX_WIDGET_TONE_ACCENT)>0 &&
          px_widget_activity_dots(&scene,scene.root,UINT64_C(0x100a),0.7,PX_WIDGET_TONE_SUCCESS)>0,
          "liveness widgets should build");
    px_widget_agent_t agent = {.name="Worker 03",.task="Visual regression",
        .model="local/clang",.state=NATIVE_UI_AGENT_EXECUTING,
        .progress=0.55,.cost_usd=0.012};
    CHECK(px_widget_agent_card(&scene,scene.root,UINT64_C(0x100b),&agent)>0,
          "foundation agent card should build");
    CHECK(px_widget_toast(&scene,scene.root,UINT64_C(0x100c),"Build complete",PX_WIDGET_TONE_SUCCESS)>0,
          "toast should build");
    native_ui_layout(&scene);
    CHECK(scene.count > 20 && scene.count < NATIVE_UI_MAX_NODES,
          "foundation widget set should remain bounded");
    const native_ui_node_t *spinner=find_key(&scene,UINT64_C(0x1009));
    const native_ui_node_t *progress=find_key(&scene,UINT64_C(0x1006));
    CHECK(spinner && !strcmp(spinner->text,PX_WIDGET_KIND_SPINNER) &&
          (spinner->state&NATIVE_UI_STATE_LIVE),
          "spinner should carry backend-neutral custom kind and liveness");
    CHECK(progress && progress->value < -1.0f &&
          (progress->state&NATIVE_UI_STATE_LIVE),
          "indeterminate progress should encode phase for the shared backend");
}

static void test_component_keys_and_damage(void) {
    uint64_t root_key = agent_ui_component_key(AGENT_UI_COMPONENT_MESSAGE, 7);
    CHECK(root_key != 0 && root_key ==
          agent_ui_component_key(AGENT_UI_COMPONENT_MESSAGE, 7),
          "component root keys should be stable");
    for (int part = AGENT_UI_PART_ROOT; part <= AGENT_UI_PART_DETAIL; part++) {
        uint64_t key = agent_ui_component_part_key(
            root_key, (agent_ui_component_part_t)part);
        CHECK(key != 0, "component part key %d should be nonzero", part);
        for (int other = part + 1; other <= AGENT_UI_PART_DETAIL; other++)
            CHECK(key != agent_ui_component_part_key(
                  root_key, (agent_ui_component_part_t)other),
                  "component part keys should be unique");
    }

    native_ui_scene_t before, after;
    native_ui_scene_init(&before, 900, 220);
    before.nodes[before.root].style.flow = NATIVE_UI_FLOW_COLUMN;
    before.nodes[before.root].style.padding = (native_ui_insets_t){12,12,12,12};
    agent_ui_builder_t builder;
    CHECK(agent_ui_builder_init(&builder, &before, agent_ui_theme_default()),
          "component builder should initialize on retained scene");
    agent_ui_message_model_t message = {
        .kind = AGENT_UI_MESSAGE_ASSISTANT, .author = "DSCO",
        .body = "Initial response", .meta = "NOW",
    };
    int message_root = agent_ui_add_message(&builder, before.root, root_key, &message);
    native_ui_layout(&before);
    CHECK(message_root > 0 && find_key(&before, root_key) &&
          find_key(&before, root_key)->accessibility_label[0],
          "message should expose a stable accessible root");

    native_ui_scene_init(&after, 900, 220);
    after.nodes[after.root].style.flow = NATIVE_UI_FLOW_COLUMN;
    after.nodes[after.root].style.padding = (native_ui_insets_t){12,12,12,12};
    CHECK(agent_ui_builder_init(&builder, &after, agent_ui_theme_default()),
          "second builder should initialize");
    message.body = "Updated streaming response";
    message.streaming = true;
    CHECK(agent_ui_add_message(&builder, after.root, root_key, &message) > 0,
          "updated retained message should build");
    native_ui_layout(&after);
    native_ui_damage_t damage;
    native_ui_diff(&before, &after, &damage);
    CHECK(!damage.full_repaint && damage.count > 0,
          "message mutation should produce bounded retained damage");
}

static void test_governance_actions(void) {
    native_ui_scene_t scene;
    native_ui_scene_init(&scene, 900, 300);
    scene.nodes[scene.root].style.flow = NATIVE_UI_FLOW_COLUMN;
    scene.nodes[scene.root].style.padding = (native_ui_insets_t){12,12,12,12};
    agent_ui_builder_t builder;
    CHECK(agent_ui_builder_init(&builder, &scene,
                                agent_ui_theme_find("high-contrast")),
          "governance builder should initialize");
    uint64_t key = agent_ui_component_key(AGENT_UI_COMPONENT_PERMISSION, 1);
    agent_ui_permission_model_t permission = {
        .title = "Run local build", .detail = "Compile the gallery.",
        .scope = "exec: make dsco-agent-ui-gallery",
        .primary_label = "ALLOW", .secondary_label = "DENY",
        .risk = AGENT_UI_TONE_WARNING,
    };
    CHECK(agent_ui_add_permission(&builder, scene.root, key, &permission) > 0,
          "permission component should build");
    native_ui_layout(&scene);
    const native_ui_node_t *primary = find_key(
        &scene, agent_ui_component_part_key(key, AGENT_UI_PART_PRIMARY_ACTION));
    const native_ui_node_t *secondary = find_key(
        &scene, agent_ui_component_part_key(key, AGENT_UI_PART_SECONDARY_ACTION));
    CHECK(primary && secondary &&
          (primary->state & NATIVE_UI_STATE_FOCUSABLE) &&
          (secondary->state & NATIVE_UI_STATE_FOCUSABLE),
          "permission decisions should be keyboard-focusable stable parts");
    int first = native_ui_focus_move(&scene, false);
    int second = native_ui_focus_move(&scene, false);
    CHECK(first >= 0 && second >= 0 && first != second,
          "focus traversal should reach both permission decisions");
}

static void test_gallery_pages(void) {
    bool roles[NATIVE_UI_ROLE_SCRIM + 1];
    memset(roles, 0, sizeof(roles));
    for (int page = 0; page < AGENT_UI_GALLERY_PAGE_COUNT; page++) {
        native_ui_scene_t scene;
        const agent_ui_theme_t *theme = agent_ui_theme_at((size_t)page * 2U);
        CHECK(agent_ui_gallery_build(&scene, 1800, 1000,
              (agent_ui_gallery_page_t)page, theme),
              "gallery page %d should build", page + 1);
        CHECK(scene.count > 20 && scene.count < NATIVE_UI_MAX_NODES,
              "gallery page %d should use bounded retained storage", page + 1);
        bool valid = true;
        for (int i = 0; i < scene.count; i++) {
            const native_ui_node_t *node = &scene.nodes[i];
            if ((node->state & NATIVE_UI_STATE_VISIBLE) &&
                !frame_inside(node->frame, scene.viewport)) valid = false;
            if (node->role >= 0 && node->role <= NATIVE_UI_ROLE_SCRIM)
                roles[node->role] = true;
            for (int j = i + 1; j < scene.count; j++)
                if (node->key == scene.nodes[j].key) valid = false;
        }
        CHECK(valid, "gallery page %d should keep unique keys and frames in viewport",
              page + 1);
    }
    native_ui_role_t required[] = {
        NATIVE_UI_ROLE_MESSAGE, NATIVE_UI_ROLE_REASONING_ACTIVITY,
        NATIVE_UI_ROLE_TOOL_ACTIVITY, NATIVE_UI_ROLE_AGENT_TOPOLOGY,
        NATIVE_UI_ROLE_ARTIFACT, NATIVE_UI_ROLE_PERMISSION,
        NATIVE_UI_ROLE_PLAN, NATIVE_UI_ROLE_QUEUE, NATIVE_UI_ROLE_TIMELINE,
        NATIVE_UI_ROLE_COMPOSER, NATIVE_UI_ROLE_COMMAND_PALETTE,
        NATIVE_UI_ROLE_CODE, NATIVE_UI_ROLE_NOTIFICATION,
    };
    for (size_t i = 0; i < sizeof(required)/sizeof(required[0]); i++)
        CHECK(roles[required[i]], "gallery should exercise semantic role %s",
              native_ui_role_name(required[i]));
}

static void test_retina_canvas(void) {
    const agent_ui_theme_t *theme = agent_ui_theme_find("spatial-command");
    agent_ui_canvas_t *canvas = agent_ui_canvas_create(1800, 1400, 2, theme);
    CHECK(canvas && agent_ui_canvas_physical_width(canvas) == 1800 &&
          agent_ui_canvas_physical_height(canvas) == 1400 &&
          agent_ui_canvas_logical_width(canvas) == 900 &&
          agent_ui_canvas_logical_height(canvas) == 700 &&
          agent_ui_canvas_backing_scale(canvas) == 2,
          "canvas should keep logical and exact backing dimensions separate");
    native_ui_scene_t scene;
    CHECK(canvas && agent_ui_gallery_build(&scene, 900, 700,
          AGENT_UI_GALLERY_THEMES, theme),
          "minimum Retina gallery fixture should build");
    CHECK(canvas && agent_ui_canvas_render(canvas, &scene, NULL),
          "semantic gallery should rasterize through pixel backend");
    const uint8_t *pixels = agent_ui_canvas_pixels(canvas);
    size_t bytes = agent_ui_canvas_size(canvas);
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; pixels && i < bytes; i += 97) {
        hash ^= pixels[i]; hash *= UINT64_C(1099511628211);
    }
    CHECK(pixels && bytes == (size_t)1800 * 1400 * 3 && hash != 0,
          "Retina canvas should own a populated exact-size RGB store");
    char path[160];
    snprintf(path, sizeof(path), "/tmp/dsco-agent-ui-components-%ld.ppm",
             (long)getpid());
    CHECK(canvas && agent_ui_canvas_write_ppm(canvas, path),
          "canvas should export a headless proof frame");
    struct stat info;
    CHECK(stat(path, &info) == 0 && info.st_size > (off_t)bytes,
          "PPM proof should contain header plus every device pixel");
    unlink(path);
    agent_ui_canvas_destroy(canvas);
}

int main(void) {
    test_theme_catalog();
    test_foundation_widgets();
    test_component_keys_and_damage();
    test_governance_actions();
    test_gallery_pages();
    test_retina_canvas();
    printf("agent ui components: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
