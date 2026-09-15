#!/usr/bin/env python3
"""Compile production proxy selectors/serializers; check recovery and policy.

Only the registry, profile filter and external catalog are fixtures. No provider
requests, credentials, GUI control or live conversation mutation is involved.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RECOVERY = {"context_status", "context_compact", "context_evict", "context_recall"}
NAMES = ["bash", "dsco-python-3x", "discover_tools", "load_tools", "invoke_tool",
         "evict_tools", "context_status", "context_compact", "context_evict",
         "context_recall", "read_file", "weather", "write_file", "edit_file",
         "list_directory", "find_files", "grep_files", "forced_probe",
         "goal_queue", "get_goal", "update_goal"]


def function(source, name):
    match = re.search(r"^(?:static )?[^\n;]*\b" + name + r"\([^;{]*\)\s*\{", source, re.M)
    assert match, name
    return source[match.start():source.index("\n}\n", match.start()) + 3]


HEAD = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "provider.h"
#include "tools.h"
#include "json_util.h"
int g_cheap_mode, fixture_goal, fixture_external;
bool goal_is_active(const session_state_t *s) { return s && fixture_goal; }
const char *provider_model_family(const char *s) { (void)s; return "openai"; }
static bool provider_wire_allows_openai_cache_params(const char *s) { (void)s; return false; }
bool provider_model_supports_cache_control(const char *s) { (void)s; return false; }
const char *provider_profile_canonical_name(const char *s) { return s; }
bool model_is_moonshot_compatible(const char *s) { (void)s; return false; }
static const char *openai_last_user_context(conversation_t *c) { (void)c; return ""; }
const tool_def_t **tools_get_filtered(const char *c,int n,int *out) {
    (void)c; (void)n; *out=0; return NULL;
}
external_tool_snapshot_t tools_external_snapshot(void) {
    external_tool_snapshot_t s={.count=fixture_external};
    s.items=calloc((size_t)(s.count?s.count:1),sizeof(*s.items)); assert(s.items);
    for(int i=0;i<s.count;i++) {
        snprintf(s.items[i].name,sizeof(s.items[i].name),"external_%d",i);
        s.items[i].input_schema_json="{\"type\":\"object\",\"properties\":{}}";
        s.items[i].loaded=true;
    }
    return s;
}
void tools_external_snapshot_free(external_tool_snapshot_t *s) { free(s->items); }
int tools_rank_external_snapshot(const external_tool_snapshot_t *s,const char *c,int *out,int n) {
    (void)c; int count=s->count<n?s->count:n;
    for(int i=0;i<count;i++) out[i]=i;
    return count;
}
'''

MAIN = r'''
bool tools_profile_allows_index(int i) {
    if(i<0 || i>=(int)(sizeof(catalog)/sizeof(*catalog))) return false;
    const char *allow=getenv("DSCO_TOOL_ALLOWLIST");
    if(!allow || !*allow) return true;
    size_t size=strlen(catalog[i].name);
    for(const char *p=allow;*p;) {
        const char *end=strchr(p,','); if(!end) end=p+strlen(p);
        if((size_t)(end-p)==size && !strncmp(p,catalog[i].name,size)) return true;
        p=*end?end+1:end;
    }
    return false;
}
static void emit(int native, const char *label, session_state_t *s) {
    jbuf_t b; jbuf_init(&b,4096); jbuf_append(&b,"{\"label\":");
    jbuf_append_json_str(&b,label);
    bool emitted=native?chatgpt_append_tools(&b,NULL,s):openai_append_tools_json(&b,NULL,s,"openai");
    jbuf_append(&b,emitted?",\"emitted\":true}":",\"emitted\":false}");
    puts(b.data); jbuf_free(&b);
}
int main(int argc,char **argv) {
    assert(argc==2); int native=atoi(argv[1]); session_state_t s={0};
    snprintf(s.model,sizeof(s.model),"gpt-6-astra");
    setenv("DSCO_TOOL_PROXY","1",1);
    unsetenv("DSCO_TOOL_ALLOWLIST"); unsetenv("DSCO_OR_DISABLE_TOOLS");
    for(int cheap=0;cheap<2;cheap++) {
        g_cheap_mode=cheap; emit(native,cheap?"cheap":"default",&s);
    }
    fixture_external=5; emit(native,"external-pressure",&s);
    fixture_goal=1; snprintf(s.tool_choice,sizeof(s.tool_choice),"tool:forced_probe");
    emit(native,"forced-goal-external",&s);
    setenv("DSCO_TOOL_ALLOWLIST","context_evict,forced_probe",1);
    emit(native,"allowlist-forced",&s);
    setenv("DSCO_TOOL_ALLOWLIST","read_file",1);
    emit(native,"denied-forced",&s);
    s.tool_choice[0]=0; emit(native,"allowlist",&s);
    setenv("DSCO_TOOL_ALLOWLIST","unknown_fixture",1); emit(native,"empty-allowlist",&s);
    unsetenv("DSCO_TOOL_ALLOWLIST"); fixture_goal=fixture_external=0;
    s.direct_answer_mode=true; emit(native,"direct-answer",&s);
    s.direct_answer_mode=false; setenv("DSCO_OR_DISABLE_TOOLS","1",1);
    emit(native,"disabled",&s);
    return 0;
}
'''


def main():
    source = (ROOT / "src/provider.c").read_text()
    names = re.search(r"static const char \*const openai_proxy_tool_names\[\] = \{.*?\n\};\n"
                      r"enum \{ OPENAI_PROXY_TOOL_LIMIT =.*?\};", source, re.S)
    assert names
    entries = []
    schemas = {}
    for name in NAMES:
        schema = {"type": "object", "properties": {"marker": {"const": name}}}
        schemas[name] = schema
        entries.append("{.name=" + json.dumps(name) + ",.description=" + json.dumps(name) +
                       ",.input_schema_json=" + json.dumps(json.dumps(schema)) + "}")
    catalog = "static const tool_def_t catalog[]={" + ",".join(entries) + "};\n"
    catalog += "const tool_def_t *tools_get_all(int *n){*n=sizeof(catalog)/sizeof(*catalog);return catalog;}\n"
    functions = ["openai_wire_safe_tool_name", "openai_append_function_tool", "openai_tools_disabled",
                 "openai_tool_proxy_enabled", "openai_proxy_tools", "openai_append_tools_json",
                 "chatgpt_append_tools"]
    unit = HEAD + catalog + names[0] + "\n" + "\n".join(function(source, n) for n in functions) + MAIN
    with tempfile.TemporaryDirectory(prefix="dsco-context-proxy-") as tmp:
        work = Path(tmp)
        (work / "test.c").write_text(unit)
        subprocess.run(["cc", "-std=c11", "-O1", "-g", "-D_DARWIN_C_SOURCE",
                        "-D_POSIX_C_SOURCE=200809L", "-fsanitize=address,undefined",
                        "-I", str(ROOT / "include"), str(work / "test.c"),
                        str(ROOT / "src/json_util.c"), str(ROOT / "src/json_fast.c"),
                        "-o", str(work / "test")], check=True, timeout=30)
        scenarios = 0
        for native in (False, True):
            run = subprocess.run([str(work / "test"), str(int(native))], capture_output=True,
                                 text=True, check=True, timeout=15,
                                 env={"PATH": os.environ.get("PATH", "/usr/bin:/bin")})
            results = {row["label"]: row for row in map(json.loads, run.stdout.splitlines())}
            for label, row in results.items():
                wire = [item.get("function", item) for item in row.get("tools", [])]
                selected = [item["name"] for item in wire]
                assert len(selected) == len(set(selected)) and len(selected) <= 17, (native, label, selected)
                for item in wire:
                    if item["name"] in schemas:
                        assert item["parameters"] == schemas[item["name"]], item
                if label in ("default", "cheap", "external-pressure", "forced-goal-external"):
                    assert RECOVERY <= set(selected), (native, label, selected)
                if label in ("default", "cheap"):
                    assert set(selected) == set(NAMES[:17]), (native, label, selected)
                if label == "forced-goal-external":
                    assert {"forced_probe", "goal_queue", "get_goal", "update_goal"} <= set(selected)
                if label == "allowlist-forced":
                    assert set(selected) == {"forced_probe", "context_evict"}, (native, row)
                if label == "denied-forced":
                    assert set(selected) <= {"read_file"}, (native, row)
                if label == "allowlist":
                    assert selected == ["read_file"], (native, row)
                if label in ("empty-allowlist", "direct-answer", "disabled"):
                    assert not selected and not row["emitted"], (native, row)
                scenarios += 1
    print(f"PASS: {scenarios} sanitized production proxy scenarios; recovery tools, schemas, caps, goals, forced names and allowlists")


if __name__ == "__main__":
    main()
