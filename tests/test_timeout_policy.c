#include "timeout_policy.h"
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    unsetenv("DSCO_COMMAND_TIMEOUT_S"); unsetenv("DSCO_MCP_TIMEOUT_MS");
    unsetenv("DSCO_MCP_CALL_TIMEOUT_MS"); unsetenv("DSCO_TOOL_TIMEOUT_BASH");
    unsetenv("DSCO_TOOL_DEFAULT_TIMEOUT");
    assert(dsco_execution_default_s() == 600);
    assert(dsco_job_timeout_s("{}") == 0);
    assert(dsco_job_timeout_s("{\"timeout_seconds\":0}") == 0);
    assert(dsco_job_timeout_s("{\"timeout_seconds\":1}") == 1);
    assert(dsco_job_timeout_s("{\"timeout\":30}") == 30);
    assert(dsco_job_timeout_s("{\"timeout_seconds\":-1}") == -1);
    assert(dsco_job_timeout_s("{\"timeout_seconds\":\"bad\"}") == -1);
    assert(dsco_job_timeout_s("{\"timeout_seconds\":86401}") == -1);
    assert(dsco_execution_timeout_s("run_command", "{}") == 600);
    assert(tool_timeout_for("unknown_work") == 600);
    assert(tool_timeout_for("bash") == 600);
    assert(dsco_mcp_call_timeout_ms() == 600000);
    assert(tool_timeout_for_input("bash", "{}") >= 605);
    assert(tool_timeout_for_input("run_command", "{\"timeout\":3600}") >= 3605);
    assert(dsco_execution_timeout_s("bash", "{\"timeout\":3600}") == 3600);
    assert(dsco_execution_timeout_s("Bash", "{\"timeout\":90000}") == 90);
    assert(dsco_execution_timeout_s("Bash", "{\"timeout_seconds\":3600}") == 3600);
    assert(dsco_execution_timeout_s("bash", "{\"timeout\":-1}") == 600);
    assert(dsco_execution_timeout_s("bash", "{\"timeout\":999999}") == 86400);
    assert(tool_timeout_for_input("bash", "{\"timeout\":86400}") == 86400);
    setenv("DSCO_COMMAND_TIMEOUT_S", "7200", 1);
    assert(dsco_job_timeout_s("{}") == 7200);
    assert(dsco_job_timeout_s("{\"timeout_seconds\":0}") == 0);
    assert(dsco_execution_timeout_s("python", "{}") == 7200);
    assert(tool_timeout_for_input("dsco-python-3x", "{}") >= 7205);
    setenv("DSCO_MCP_TIMEOUT_MS", "3600000", 1);
    assert(dsco_mcp_call_timeout_ms() == 3600000);
    assert(tool_timeout_for_input("mcp__fixture__slow", "{}") >= 3605);
    setenv("DSCO_MCP_CALL_TIMEOUT_MS", "900000", 1);
    assert(dsco_mcp_call_timeout_ms() == 900000);
    setenv("DSCO_TOOL_TIMEOUT_BASH", "2", 1);
    assert(tool_timeout_for_input("bash", "{\"timeout\":3600}") == 2);
    unsetenv("DSCO_TOOL_TIMEOUT_BASH");
    setenv("DSCO_COMMAND_TIMEOUT_S", "bad", 1);
    assert(dsco_job_timeout_s("{}") == -1);
    assert(dsco_execution_default_s() == 600);
    puts("PASS: command budgets and completion-driven durable lifetime, explicit units, environment precedence");
    return 0;
}
