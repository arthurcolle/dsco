#ifndef DSCO_BUFFER_VIEW_H
#define DSCO_BUFFER_VIEW_H
#include <stdbool.h>
#include <stddef.h>

#define BUFFER_VIEW_DESCRIPTION \
    "Open persistent buffers in owned Kitty splits, tabs or desktop windows. " \
    "Select by buffer_id/name; returned surface_id identifies one view. " \
    "Modes: edit (vi), view (less), follow (live less), textedit (macOS copy/paste). " \
    "For copyable macOS drafts explicitly use open with mode=textedit and buffer_id/name; " \
    "TextEdit verifies the open document without Kitty and preserves unsaved edits. " \
    "TextEdit accepts no Kitty view options or request_id; manage its window in TextEdit. " \
    "Opening reuses a live matching view unless new_view=true; request_id reconciles retries. " \
    "Multiple views share the same content file. Closing a view terminates its editor and can " \
    "discard unsaved editor changes, but retains saved buffer content. Mutations with multiple " \
    "matching views require surface_id. No arbitrary executables or shell commands."
#define BUFFER_VIEW_SCHEMA \
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"open\",\"list\",\"focus\",\"resize\",\"layout\",\"detach\",\"close\"]}," \
    "\"workspace\":{\"type\":\"string\",\"maxLength\":63}," \
    "\"buffer_id\":{\"type\":\"string\",\"maxLength\":36}," \
    "\"name\":{\"type\":\"string\",\"maxLength\":160}," \
    "\"surface_id\":{\"type\":\"string\",\"maxLength\":79}," \
    "\"source_surface_id\":{\"type\":\"string\",\"maxLength\":79}," \
    "\"request_id\":{\"type\":\"string\",\"maxLength\":79}," \
    "\"mode\":{\"type\":\"string\",\"enum\":[\"edit\",\"view\",\"follow\",\"textedit\"]}," \
    "\"type\":{\"type\":\"string\",\"enum\":[\"window\",\"tab\",\"os-window\"]}," \
    "\"location\":{\"type\":\"string\",\"enum\":[\"split\",\"vsplit\",\"hsplit\"]}," \
    "\"new_view\":{\"type\":\"boolean\"},\"focus\":{\"type\":\"boolean\"}," \
    "\"visible\":{\"type\":\"boolean\"}," \
    "\"layout\":{\"type\":\"string\",\"enum\":[\"splits\",\"tall\",\"grid\",\"stack\"]}," \
    "\"axis\":{\"type\":\"string\",\"enum\":[\"horizontal\",\"vertical\",\"reset\"]}," \
    "\"increment\":{\"type\":\"integer\",\"minimum\":-1000,\"maximum\":1000}" \
    "},\"required\":[\"action\"]}"

bool tool_buffer_view(const char *input, char *result, size_t cap);
#endif
