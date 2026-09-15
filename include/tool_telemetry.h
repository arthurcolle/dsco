#ifndef DSCO_TOOL_TELEMETRY_H
#define DSCO_TOOL_TELEMETRY_H
typedef enum { TOOL_TIMEOUT_NONE, TOOL_TIMEOUT_WALL, TOOL_TIMEOUT_IDLE, TOOL_TIMEOUT_WATCHDOG } tool_timeout_origin_t;
void tool_telemetry_reset(void);
void tool_telemetry_timeout(tool_timeout_origin_t origin);
tool_timeout_origin_t tool_telemetry_origin(void);
const char *tool_timeout_origin_name(tool_timeout_origin_t origin);
#endif
