#!/usr/bin/env python3
"""Exercise the production cached-result presenter and native stats policy."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^static [^\n;]*\b" + name + r"\([^;{]*\)\s*\{", source, re.M)
    assert match, name
    return source[match.start():source.index("\n}\n", match.start()) + 3]


HEAD = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TUI_DIM ""
#define TUI_RESET ""
static bool native;
static int operations, headers;
static bool operation_ok;
static const char *operation_result;
static bool pixel_tui_session_active(void) { return native; }
static void native_tool_operation_complete(const char *name,const char *input,
 bool ok,double elapsed,const char *result) {
 assert(!strcmp(name,"read_file")&&!strcmp(input,"{}")&&elapsed==0);
 if(native){operations++;operation_ok=ok;operation_result=result;}
}
static void print_role_header(const char *role,bool ok,const char *trail) {
 (void)ok;assert(!strcmp(role,"tool_response")&&!strcmp(trail,"cached"));
 headers++;fprintf(stderr,"tool_response cached\n");
}
'''

MAIN = r'''
int main(void) {
 assert(freopen("capture.txt","w+",stderr));
 setenv("DSCO_SHOW_TURN_STATS","1",1);
 native=true;assert(!show_turn_stats());
 const char *result="read completed\nfull result retained";
 print_cached_tool_result("read_file","{}",true,result,"cached");
 assert(operations==1&&operation_ok&&operation_result==result&&headers==0);
 const char *failure="permission denied";
 print_cached_tool_result("read_file","{}",false,failure,"cached");
 assert(operations==2&&!operation_ok&&operation_result==failure&&headers==0);
 fflush(stderr);assert(ftell(stderr)==0);
 native=false;assert(show_turn_stats());
 print_cached_tool_result("read_file","{}",true,result,"cached");
 assert(operations==2&&headers==1);
 fflush(stderr);assert(ftell(stderr)>0);
 setenv("DSCO_SHOW_TURN_STATS","0",1);assert(!show_turn_stats());
 unsetenv("DSCO_SHOW_TURN_STATS");assert(!show_turn_stats());
 puts("PASS: cached successes/errors each produce one native result, no transcript duplication; ANSI and accounting opt-in preserved");
}
'''


def main():
    source = (ROOT / "src/agent.c").read_text()
    with tempfile.TemporaryDirectory(prefix="dsco-native-noise-") as tmp:
        folder = Path(tmp)
        fixture = folder / "presenter.c"
        fixture.write_text(HEAD + function(source, "print_cached_tool_result") +
                           function(source, "show_turn_stats") + MAIN)
        binary = folder / "presenter"
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-g",
                        "-fsanitize=address,undefined", str(fixture), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], cwd=folder, check=True)


if __name__ == "__main__":
    main()
