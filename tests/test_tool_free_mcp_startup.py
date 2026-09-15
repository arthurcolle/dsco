"""Compile production attach logic; tool-free requests never start integrations."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/main.c').read_text()
start=source.index('static int oneshot_mcp_attach(void) {')
end=source.index('\n}\n',start)+3
stubs=r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int server_count; } mcp_registry_t;
typedef struct { char name[8]; char *description,*input_schema,*output_schema; } mcp_tool_t;
static mcp_registry_t g_oneshot_mcp;
static int attaches;
static void dsco_setup_load_saved_env(void) {}
static void mcp_cancel_reset(void) {}
static void mcp_set_silent(bool b) {(void)b;}
static int mcp_init(mcp_registry_t *r) {(void)r;attaches++;return 0;}
static const mcp_tool_t *mcp_get_tools(mcp_registry_t *r,int *n) {(void)r;*n=0;return NULL;}
#define tools_register_external_with_output(...) ((void)0)
'''
main=r'''
int main(void) {
 setenv("DSCO_MCP_SERVER","slow-inherited-server",1);
 setenv("DSCO_MCP_HEADLESS","1",1);
 setenv("DSCO_TOOL_CHOICE","none",1);
 if (oneshot_mcp_attach()!=0 || attaches!=0) return 1;
 setenv("DSCO_TOOL_CHOICE","auto",1);
 oneshot_mcp_attach(); if(attaches!=1) return 2;
 unsetenv("DSCO_MCP_SERVER"); unsetenv("DSCO_MCP_SERVERS");unsetenv("DSCO_MCP_HEADLESS");
 oneshot_mcp_attach(); if(attaches!=1) return 3;
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c=Path(d)/'test.c';exe=Path(d)/'test'
    c.write_text(stubs+source[start:end]+main)
    subprocess.run(['cc','-D_DARWIN_C_SOURCE',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: tool-free mode skips inherited MCP attachment; tool-enabled opt-in still attaches')
