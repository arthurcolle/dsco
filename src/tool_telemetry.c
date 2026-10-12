#include "tool_telemetry.h"
/* Execution-thread-local: output text cannot forge terminal status. */
static _Thread_local tool_timeout_origin_t origin;
void tool_telemetry_reset(void) { origin = TOOL_TIMEOUT_NONE; }
void tool_telemetry_timeout(tool_timeout_origin_t value) { if (origin == TOOL_TIMEOUT_NONE) origin = value; }
tool_timeout_origin_t tool_telemetry_origin(void) { return origin; }
const char *tool_timeout_origin_name(tool_timeout_origin_t value) {
    switch (value) {
    case TOOL_TIMEOUT_WALL: return "harness_wall";
    case TOOL_TIMEOUT_IDLE: return "harness_idle";
    case TOOL_TIMEOUT_WATCHDOG: return "tool_deadline";
    default: return "none";
    }
}
