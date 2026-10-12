#!/usr/bin/env python3
"""Compare production JSON string decoding with a source snapshot; no inference."""
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
WRAPPER = r'''
#include "json_util.h"
#include <stdlib.h>
#include <time.h>
void release_string(void *p) {free(p);}
double bench_string(const char *json,int count) {
    struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    for(int i=0;i<count;i++) free(json_get_str(json,"text"));
    clock_gettime(CLOCK_MONOTONIC,&b);
    return ((b.tv_sec-a.tv_sec)*1e6+(b.tv_nsec-a.tv_nsec)/1e3)/count;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    wrapper = out / "wrapper.c"; wrapper.write_text(WRAPPER)
    libs, hashes, commands = {}, {}, []
    for label, path in [("before", args.before), ("after", ROOT / "src/json_util.c")]:
        copy = out / (label + ".c"); copy.write_bytes(path.read_bytes())
        hashes[label] = hashlib.sha256(copy.read_bytes()).hexdigest()
        target = out / (label + ".dylib")
        command = shlex.split(os.environ.get("CC", "cc")) + ["-O2", "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC", "-I", str(ROOT / "include"),
            str(copy), str(wrapper), "-lm", "-o", str(target)]
        subprocess.run(command, check=True, timeout=30); commands.append(command)
        lib = ctypes.CDLL(str(target)); libs[label] = lib
        lib.json_get_str.argtypes = [ctypes.c_char_p, ctypes.c_char_p]; lib.json_get_str.restype = ctypes.c_void_p
        lib.release_string.argtypes = [ctypes.c_void_p]
        lib.bench_string.argtypes = [ctypes.c_char_p, ctypes.c_int]; lib.bench_string.restype = ctypes.c_double

    def decode(lib, value):
        ptr = lib.json_get_str(value, b"text")
        result = ctypes.string_at(ptr) if ptr else None
        lib.release_string(ptr); return result

    documents = [b'{}', b'{"text":""}', b'{"text":"value"}', b'{"text":"unterminated', b'{"text":"slash\\',
        b'{"text":"\\uD83D\\uDE00"}', b'{"text":"\\uD800"}', b'{"text":"\\uD800\\u"}',
        b'{"text":"\\u0"}', b'{"text":"\\q"}', b'{"te\\u0078t":"escaped key"}',
        b'{"other":[1,2],"text":"x"}', b'{"text":42}']
    random.seed(31)
    for n in [0, 1, 2, 15, 16, 31, 32, 254, 255, 256, 257, 4095, 4096, 65536]:
        for ending in [b'"}', b'', b'\\', b'\\nrest"}', b'\\uD83D\\uDE00"}']:
            documents.append(b'{"text":"' + b'a' * n + ending)
    for _ in range(300):
        payload = bytes(random.randrange(1, 256) for _ in range(random.randrange(1, 200)))
        documents.append(b'{"text":"' + payload + b'"}')
    for value in documents:
        assert decode(libs["before"], value) == decode(libs["after"], value), value
    # Small and escape-heavy controls ensure the early-run path is not only
    # evaluated against artificial all-ASCII buffers.
    workloads = {
        "small_text": {"text": "READY"},
        "small_empty": {"text": ""},
        "small_escape": {"text": "a\nb"},
        "prompt_64k": {"text": ("Review the implementation and preserve its public behavior. " * 1200)},
        "code_64k": {"text": ('def test_value():\n    assert parse("quote\\value") == 42\n' * 1200)},
        "long_first_line": {"text": "context " * 8192 + '\nfunction("argument")\n'},
        "utf8_64k": {"text": "日本語 café 🙂 data " * 2400},
        "escaped_unicode": {"text": "日本語 café 🙂" * 2400},
    }
    timings = {}
    for name, value in workloads.items():
        payload = json.dumps(value, ensure_ascii=name == "escaped_unicode", separators=(",", ":")).encode()
        assert decode(libs["after"], payload) == value["text"].encode()
        count = 1000 if len(payload) > 10000 else 100000
        samples = []
        for i in range(15):
            sample = {}
            for label in (["before", "after"] if i % 2 == 0 else ["after", "before"]):
                sample[label + "_us"] = libs[label].bench_string(payload, count)
            samples.append(sample)
        before = statistics.median(s["before_us"] for s in samples); after = statistics.median(s["after_us"] for s in samples)
        timings[name] = {"input_bytes": len(payload), "calls_per_sample": count, "before_us": before, "after_us": after,
                         "reduction_percent": 100 * (1 - after / before), "samples": samples}
        documents.append(payload)
    # ASAN checks allocation boundaries, truncated escapes, invalid UTF-8, and
    # the exact-byte fixtures used above. Expected bytes come from the baseline.
    def literal(data):
        return "NULL" if data is None else '"' + ''.join(f'\\x{b:02x}' for b in data) + '"'
    fixture_rows = ["{" + literal(doc) + "," + literal(decode(libs["before"], doc)) + "}" for doc in documents]
    main_c = out / "asan_main.c"
    main_c.write_text('#include "json_util.h"\n#include <assert.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n'
        + 'static struct {const char *json,*expected;} cases[]={\n' + ',\n'.join(fixture_rows) + '\n};\n'
        + 'int main(void){for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++){char *p=json_get_str(cases[i].json,"text");'
        + 'assert((p==NULL)==(cases[i].expected==NULL));if(p)assert(!strcmp(p,cases[i].expected));free(p);}puts("PASS: JSON string differential ASAN cases");}\n')
    asan_command = shlex.split(os.environ.get("CC", "cc")) + ["-O1", "-g", "-fsanitize=address", "-std=c11",
        "-D_POSIX_C_SOURCE=200809L", "-I", str(ROOT / "include"), str(out / "after.c"), str(main_c), "-lm", "-o", str(out / "asan")]
    subprocess.run(asan_command, check=True, timeout=30)
    check = subprocess.run([str(out / "asan")], text=True, capture_output=True, timeout=30, check=True)
    (out / "asan.stdout.txt").write_text(check.stdout); (out / "asan.stderr.txt").write_text(check.stderr)
    result = {"equivalence_cases": len(documents), "asan_passed": True, "source_sha256": hashes,
        "compiler_argv": commands, "asan_compiler_argv": asan_command, "timings": timings}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({**result, "timings": {k: {a: b for a, b in v.items() if a != "samples"} for k, v in timings.items()}}, indent=2))


if __name__ == "__main__":
    main()
