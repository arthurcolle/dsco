#include "chronicle.h"
#include "tool_telemetry.h"
#include "tools.h"
#include "vm.h"
int g_cheap_mode;
vm_t g_vm;
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
static void *parallel_success(void *unused) {
    (void)unused;
    char output[4096];
    tool_telemetry_reset();
    assert(tools_execute_for_tier("bash","{\"command\":\"sleep 2; printf ok\",\"timeout\":5}","trusted",output,sizeof(output)));
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_NONE);
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
    setenv("DSCO_CHRONICLE_DIR",argv[1],1);
    setenv("DSCO_CHRONICLE_MODE","metadata",1);
    setenv("DSCO_JOURNAL","off",1);
    assert(chronicle_start(NULL));
    char span[64];
    assert(chronicle_tool_call_start("trace",NULL,"invoke_tool","call","{\"name\":\"weather_fixture\",\"input\":{\"secret\":\"DO_NOT_COPY\"}}",span));
    tool_telemetry_reset();tool_telemetry_timeout(TOOL_TIMEOUT_IDLE);
    assert(chronicle_tool_call_end("trace",span,"invoke_tool","ignored",false,true,10));
    assert(chronicle_tool_call_start("trace",NULL,"bash","call2","{}",span));
    tool_telemetry_reset();tool_telemetry_timeout(TOOL_TIMEOUT_WALL);
    assert(chronicle_tool_call_end("trace",span,"bash","ignored",false,true,10));
    assert(chronicle_tool_call_start("trace",NULL,"bash","call3","{}",span));
    tool_telemetry_reset();
    assert(chronicle_tool_call_end("trace",span,"bash","[killed: idle timeout 1s]",true,false,10));
    tools_init_local_only();
    char result[4096];
    pthread_t parallel;
    assert(!pthread_create(&parallel,NULL,parallel_success,NULL));
    assert(chronicle_tool_call_start("trace",NULL,"bash","actual-wall","{}",span));
    tool_telemetry_reset();
    bool ok=tools_execute_for_tier("bash","{\"command\":\"sleep 3\",\"timeout\":1}","trusted",result,sizeof(result));
    assert(!ok);
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_WALL);
    assert(chronicle_tool_call_end("trace",span,"bash",result,ok,true,1000));
    assert(!pthread_join(parallel,NULL));
    assert(tool_telemetry_origin()==TOOL_TIMEOUT_WALL);
    if (getenv("DSCO_TEST_REAL_IDLE")) {
        assert(chronicle_tool_call_start("trace",NULL,"bash","actual-idle","{}",span));
        tool_telemetry_reset();
        ok=tools_execute_for_tier("bash","{\"command\":\"printf started; sleep 65\",\"timeout\":70}","trusted",result,sizeof(result));
        assert(!ok && tool_telemetry_origin()==TOOL_TIMEOUT_IDLE);
        assert(chronicle_tool_call_end("trace",span,"bash",result,ok,true,60000));
    }
    chronicle_stop();
}
