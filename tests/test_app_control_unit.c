/* Offline checks for app_control scoring and TypeSafe Box request/response
 * handling. No desktop, network or permissions involved. */
#include "app_control.h"
#include "vision_ground.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void scoring(void) {
    /* Exact label with matching role hint wins outright. */
    assert(app_control_match_score("Save button", "AXButton", "Save", "", true) == 1.0);
    /* Role hint mismatch is penalized below the matching control. */
    double button = app_control_match_score("save button", "AXButton", "Save", "", true);
    double text = app_control_match_score("save button", "AXStaticText", "Save", "", true);
    assert(button > text);
    /* Label containment beats value-only matches. */
    double label = app_control_match_score("recipient", "AXTextField", "Recipient name", "", true);
    double value = app_control_match_score("recipient", "AXTextField", "", "recipient list", true);
    assert(label > value && value > 0);
    /* Unrelated controls score zero; disabled ones are discounted. */
    assert(app_control_match_score("Apply", "AXButton", "Cancel", "", true) == 0.0);
    assert(app_control_match_score("Apply", "AXButton", "Apply", "", false) <
           app_control_match_score("Apply", "AXButton", "Apply", "", true));
    /* A role word that is also the label ("Search") still matches as text. */
    assert(app_control_match_score("search", "AXTextField", "Search", "", true) >= 0.85);
    /* Mutation classification. */
    assert(app_control_action_mutates("press") && app_control_action_mutates("type"));
    assert(!app_control_action_mutates("observe") && !app_control_action_mutates("find") &&
           !app_control_action_mutates(NULL));
}

static void request_body(void) {
    const unsigned char png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 1, 2, 3};
    char *body = NULL;
    size_t len = 0;
    assert(vision_ground_build_body(png, sizeof(png), "png", "the \"Send\" button", "jev-1.14.0",
                                    "img-00ff-screen", &body, &len));
    yyjson_doc *doc = yyjson_read(body, len, 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "model"), "jev-1.14.0"));
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(root, "state"), "screen"), "img-00ff-screen"));
    yyjson_val *asset = yyjson_obj_get(yyjson_obj_get(root, "assets"), "img-00ff-screen");
    assert(yyjson_equals_str(yyjson_obj_get(asset, "format"), "png"));
    assert(yyjson_equals_str(yyjson_obj_get(asset, "data_base64"), "iVBORw0KGgoBAgM="));
    yyjson_val *q = yyjson_obj_get(yyjson_obj_get(root, "questions"), "target");
    assert(yyjson_equals_str(yyjson_obj_get(q, "type"), "box"));
    assert(yyjson_equals_str(yyjson_obj_get(q, "instructions"), "the \"Send\" button in `screen`"));
    yyjson_doc_free(doc);
    free(body);
    assert(!vision_ground_build_body(png, sizeof(png), "png", "", "m", "r", &body, &len));
}

static void responses(void) {
    vision_box_t b;
    char why[160];
    /* Live response shape observed from jev-1.14.0. */
    const char *ok = "{\"model\":\"jev-1.14.0\",\"answers\":{\"target\":{\"type\":\"box\",\"box\":"
                     "{\"top\":0.6,\"bottom\":0.66,\"left\":0.56,\"right\":0.7},\"confidence\":0.84}},"
                     "\"usage\":{\"input_tokens\":1350,\"output_tokens\":36}}";
    assert(vision_ground_parse(ok, strlen(ok), &b, why, sizeof(why)));
    assert(b.left == 0.56 && b.right == 0.7 && b.top == 0.6 && b.bottom == 0.66 && b.confidence == 0.84);
    /* API errors surface their message. */
    const char *err = "{\"detail\":{\"error_type\":\"api_usage_error\",\"message\":\"Invalid request.\"}}";
    assert(!vision_ground_parse(err, strlen(err), &b, why, sizeof(why)));
    assert(strstr(why, "Invalid request."));
    /* Out-of-range, inverted, or missing fields are rejected. */
    const char *bad[] = {
        "{\"answers\":{\"target\":{\"type\":\"box\",\"box\":{\"top\":0.1,\"bottom\":0.2,\"left\":0.1,\"right\":1.5},\"confidence\":0.9}}}",
        "{\"answers\":{\"target\":{\"type\":\"box\",\"box\":{\"top\":0.5,\"bottom\":0.2,\"left\":0.1,\"right\":0.2},\"confidence\":0.9}}}",
        "{\"answers\":{\"target\":{\"type\":\"box\",\"box\":{\"top\":0.1,\"bottom\":0.2,\"left\":0.1,\"right\":0.2}}}}",
        "{\"answers\":{\"target\":{\"type\":\"noul\",\"noul\":0.5}}}",
        "not json",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        assert(!vision_ground_parse(bad[i], strlen(bad[i]), &b, why, sizeof(why)));
}

int main(void) {
    scoring();
    request_body();
    responses();
    puts("PASS app_control: find scoring, mutation classes, Box request body, Box response validation");
    return 0;
}
