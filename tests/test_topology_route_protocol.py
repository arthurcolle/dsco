#!/usr/bin/env python3
"""Exercise the actual private prompt builder and route parser without inference.

Extract complete file-local functions into a temporary translation unit to keep
this regression from adding a production API solely for testing.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/topology.c").read_text()


def function(name):
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;{]*\)\s*\{", source, re.M)
    if not match:
        raise AssertionError(f"missing private function {name}")
    end = source.index("\n}\n", match.start()) + 3
    return source[match.start():end]


headers = r'''
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "topology.h"
#include "json_util.h"
'''
markers = "\n".join(re.findall(r'^#define TOPO_STAGE_MARKER_.*$', source, re.M))
functions = "\n".join(function(name) for name in (
    "trim_copy", "extract_marked_output", "strcasestr_simple",
    "choose_conditional_target", "append_trimmed_section", "build_node_prompt"))
checks = r'''
int main(void) {
    topology_t t = {0}; t.node_count = 3; t.edge_count = 2;
    t.nodes[0].id = 0; t.nodes[0].role = ROLE_CLASSIFIER;
    snprintf(t.nodes[1].tag, sizeof(t.nodes[1].tag), "first");
    snprintf(t.nodes[2].tag, sizeof(t.nodes[2].tag), "second");
    t.edges[0].from = 0; t.edges[0].to = 1; t.edges[0].type = EDGE_CONDITIONAL;
    t.edges[1].from = 0; t.edges[1].to = 2; t.edges[1].type = EDGE_CONDITIONAL;
    char *outputs[TOPO_MAX_NODES] = {0};
    char *prompt = build_node_prompt(&t, &t.nodes[0], "choose second", outputs, outputs, 0);
    const char *begin = strstr(prompt, TOPO_STAGE_MARKER_BEGIN);
    const char *end = begin ? strstr(begin, TOPO_STAGE_MARKER_END) : NULL;
    const char *route = begin ? strstr(begin, "ROUTE: <tag>") : NULL;
    assert(begin && end && route && route < end);
    assert(!strstr(prompt, "End your response with a line like"));
    free(prompt);
    char *inside = extract_marked_output(TOPO_STAGE_MARKER_BEGIN
        "\nEvidence selected the alternate branch.\nROUTE: second\n" TOPO_STAGE_MARKER_END);
    assert(inside && choose_conditional_target(&t, 0, inside) == 2);
    free(inside);
    /* Demonstrate why putting the route after the closing marker loses it. */
    char *outside = extract_marked_output(TOPO_STAGE_MARKER_BEGIN
        "\nEvidence selected the alternate branch.\n" TOPO_STAGE_MARKER_END "\nROUTE: second");
    assert(outside && !strstr(outside, "ROUTE:") && choose_conditional_target(&t, 0, outside) == 1);
    free(outside);
    t.edge_count = 0;
    prompt = build_node_prompt(&t, &t.nodes[0], "finish", outputs, outputs, 0);
    assert(!strstr(prompt, "ROUTE:")); free(prompt);
    puts("PASS: conditional route survives stage extraction; prompt places it inside markers");
}
'''
with tempfile.TemporaryDirectory(prefix="dsco-topology-route-") as directory:
    root = Path(directory)
    unit = root / "protocol.c"
    binary = root / "protocol"
    unit.write_text(headers + markers + "\n" + functions + "\n" + checks)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-D_DARWIN_C_SOURCE", "-D_POSIX_C_SOURCE=200809L",
        "-I", str(ROOT / "include"), str(unit), str(ROOT / "src/json_util.c"),
        "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
