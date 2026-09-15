"""Compile production status output and verify its inventory scope offline."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/main.c').read_text()
start = source.index('static int runtime_cli_status_json(void) {')
end = source.index('\n}\n', start) + 3
stubs = r'''
#include <stdio.h>
#include <stdlib.h>
#define DSCO_VERSION "test"
#define GOV_MODEL_NONE 0
typedef struct { int installed_skills,active_doctrines,agents_files,has_identity,has_user,has_soul,has_memory; } dsco_workspace_status_t;
typedef struct { int fleet_hosts,readable_hosts,malformed_hosts; } runtime_fleet_stats_t;
static int tools_builtin_count(void) { return 10; }
static int tools_get_core_count(void) { return 3; }
static int tools_loaded_builtin_count(void) { return 2; }
static int tools_external_count(void) { return getenv("TEST_EXTERNAL") ? 7 : 0; }
static void dsco_workspace_status(dsco_workspace_status_t *s, void *p, int n) { (void)s;(void)p;(void)n; }
static void tools_governance_experiment_stats(unsigned long *a,unsigned long *b,double *c) { (void)a;(void)b;(void)c; }
static runtime_fleet_stats_t runtime_scan_fleet(void) { return (runtime_fleet_stats_t){0}; }
static int gov_experiment_model(void) { return 1; }
static const char *gov_model_name(int m) { (void)m; return "test"; }
static const char *dsco_workspace_root(void) { return "/test"; }
static void runtime_print_json_string(FILE *f,const char *s) { (void)s; fputs("null",f); }
'''
with tempfile.TemporaryDirectory(prefix='dsco-inventory-') as directory:
    path = Path(directory)
    c = path / 'test.c'
    exe = path / 'test'
    c.write_text(stubs + source[start:end] + '\nint main(void) { return runtime_cli_status_json(); }\n')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    for external in (0, 7):
        env = {'TEST_EXTERNAL': '1'} if external else {}
        result = json.loads(subprocess.check_output([str(exe)], env=env, text=True))
        assert result['tool_inventory_scope'] == 'current_process_registered_tools'
        assert 'another running session' in result['tool_inventory_note']
        assert result['tools']['external'] == external
        assert result['tools']['exposed_total'] == 5 + external
print('PASS: production status JSON labels process scope with zero and nonzero external inventory')
