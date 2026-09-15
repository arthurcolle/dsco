#!/usr/bin/env python3
"""Differential production tool-input normalization and paired native timings."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import random
import shlex
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>
static const char *fixture_schema;
static char *tools_schema_copy_for_name(const char *name) {
    (void)name; return fixture_schema ? safe_strdup(fixture_schema) : NULL;
}
'''


def schema(properties):
    return json.dumps({"type": "object", "properties": properties}, separators=(",", ":"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    units, hashes = [], {}
    for label, path in [("before", args.before), ("after", ROOT / "src/tools.c")]:
        text = path.read_text(); start = text.index("static const char *tools_json_ws(")
        end = text.index("static bool tools_code_or_file_tool(", start)
        body = text[start:end]; hashes[label] = hashlib.sha256(body.encode()).hexdigest()
        body = body.replace("char *tools_normalize_input(", "static char *tools_normalize_input(")
        wrapper = f'''
char *{label}_normalize(const char *schema,const char *input) {{fixture_schema=schema;return tools_normalize_input("Bash",input);}}
void {label}_free(void *p) {{free(p);}}
double {label}_bench(const char *schema,const char *input,int count) {{
    fixture_schema=schema;struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    for(int i=0;i<count;i++) free(tools_normalize_input("Bash",input));
    clock_gettime(CLOCK_MONOTONIC,&b);
    return ((b.tv_sec-a.tv_sec)*1e6+(b.tv_nsec-a.tv_nsec)/1e3)/count;
}}
'''
        unit = out / f"{label}.c"; unit.write_text(HEADER + body + wrapper); units.append(unit)
    library = out / "normalize.dylib"
    command = shlex.split(os.environ.get("CC", "cc")) + ["-O2", "-std=c11", "-D_POSIX_C_SOURCE=200809L",
        "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC", "-I", str(ROOT / "include"),
        *map(str, units), str(ROOT / "src/json_util.c"), "-lm", "-o", str(library)]
    subprocess.run(command, check=True, timeout=30)
    lib = ctypes.CDLL(str(library))
    for label in ["before", "after"]:
        f = getattr(lib, label + "_normalize"); f.argtypes = [ctypes.c_char_p, ctypes.c_char_p]; f.restype = ctypes.c_void_p
        f = getattr(lib, label + "_free"); f.argtypes = [ctypes.c_void_p]
        f = getattr(lib, label + "_bench"); f.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]; f.restype = ctypes.c_double

    scalar = schema({"i": {"type": "integer"}, "n": {"type": "number"}, "b": {"type": "boolean"}, "s": {"type": "string"},
                     "a": {"type": ["array", "null"]}, "o": {"type": "object"}, "u": {"type": ["integer", "boolean", "string"]}})
    fixtures = [(scalar, json.dumps({"i": " +0042 ", "n": " 1.25e2 ", "b": "yes", "s": "false", "a": "[1,2]", "o": '{"x":1}', "u": "3"})),
        (scalar, '{"s":"literal\nnewline"}'), (scalar, '{"i":"unterminated}'), (scalar, '{"a":"[broken]"}'),
        (scalar, '{}'), (scalar, '{"i":1,"n":1.5,"b":true}'), (scalar, None), (None, '{}'),
        ('{"type":"object"}', '{"s":"line\nbreak"}'),
        (schema({"command": {"type": "string"}, "timeout": {"type": "integer"}}), '"command":"echo ok","timeout":"5"'),
        (schema({"command": {"type": "string"}}), 'echo hello')]
    random.seed(11)
    choices = ["true", "false", "yes", "-17", "+001", "1.2e3", "NaN", "9223372036854775808", "[]", "{}", '[1,"x"]', '{"k":"v"}', "plain", "", "  "]
    for _ in range(250):
        fixtures.append((scalar, json.dumps({key: random.choice(choices) for key in random.sample(list("inbs aou".replace(" ", "")), random.randint(1, 7))})))
    checked = []
    for sch, inp in fixtures:
        values = []
        for label in ["before", "after"]:
            ptr = getattr(lib, label + "_normalize")(sch.encode() if sch else None, inp.encode() if inp else None)
            values.append(ctypes.string_at(ptr).decode() if ptr else None)
            getattr(lib, label + "_free")(ptr)
        assert values[0] == values[1], (sch, inp, values)
        checked.append({"schema": sch, "input": inp, "output": values[1]})
    assert json.loads(checked[0]["output"]) == {"i": 42, "n": 125, "b": True, "s": "false", "a": [1, 2], "o": {"x": 1}, "u": 3}
    # Replay the same malformed/repair/coercion cases in an ASAN executable so
    # newly retained properties buffers are checked on every cleanup path.
    def c_string(value):
        return "NULL" if value is None else json.dumps(value)
    cases_c = ",\n".join("{" + ",".join(c_string(row[k]) for k in ["schema", "input", "output"]) + "}" for row in checked)
    asan_main = out / "asan_main.c"
    asan_main.write_text('''#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
char *after_normalize(const char *,const char *);
static struct {const char *schema,*input,*output;} cases[]={
''' + cases_c + '''};
int main(void) {
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        char *actual=after_normalize(cases[i].schema,cases[i].input);
        assert((actual==NULL)==(cases[i].output==NULL));
        if(actual) assert(!strcmp(actual,cases[i].output));
        free(actual);
    }
    puts("PASS: all normalization equivalence cases under AddressSanitizer");
}
''')
    asan_command = shlex.split(os.environ.get("CC", "cc")) + ["-O1", "-g", "-fsanitize=address", "-std=c11",
        "-D_POSIX_C_SOURCE=200809L", "-I", str(ROOT / "include"), str(units[1]), str(asan_main),
        str(ROOT / "src/json_util.c"), "-lm", "-o", str(out / "normalize_asan")]
    subprocess.run(asan_command, check=True, timeout=30)
    asan = subprocess.run([str(out / "normalize_asan")], capture_output=True, text=True, timeout=30, check=True)
    (out / "asan.stdout.txt").write_text(asan.stdout)
    (out / "asan.stderr.txt").write_text(asan.stderr)
    workloads = []
    for width in [3, 32, 128]:
        props = {f"field{i}": {"type": ["integer", "string"], "description": "A retained optional parameter. " * 4} for i in range(width)}
        inp = {f"field{i}": str(i + 1) for i in range(max(0, width - 12), width)}
        workloads.append((f"properties_{width}", schema(props), json.dumps(inp), 200 if width > 32 else 1000))
    workloads.append(("already_typed", scalar, '{"i":1,"n":1.25,"b":true}', 1000))
    timings = {}
    for name, sch, inp, count in workloads:
        samples = []
        for pair in range(15):
            row = {}
            for label in (["before", "after"] if pair % 2 == 0 else ["after", "before"]):
                row[label + "_us"] = getattr(lib, label + "_bench")(sch.encode(), inp.encode(), count)
            samples.append(row)
        before = statistics.median(r["before_us"] for r in samples); after = statistics.median(r["after_us"] for r in samples)
        timings[name] = {"schema_bytes": len(sch), "arguments": len(json.loads(inp)), "calls_per_sample": count,
            "before_median_us": before, "after_median_us": after, "reduction_percent": 100 * (1 - after / before), "samples": samples}
    result = {"scope": "Production normalization functions and json_util.c; schema lookup is an immutable fixture copy. No inference, no registry traversal or gate overhead measured.",
              "equivalence_cases": len(checked), "asan_passed": True, "source_sha256": hashes,
              "compiler_argv": command, "asan_compiler_argv": asan_command, "timings": timings}
    (out / "equivalence.json").write_text(json.dumps(checked, indent=2) + "\n")
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({**result, "timings": {k: {a: b for a, b in v.items() if a != "samples"} for k, v in timings.items()}}, indent=2))


if __name__ == "__main__":
    main()
