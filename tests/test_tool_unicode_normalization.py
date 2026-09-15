#!/usr/bin/env python3
"""Production normalizer Unicode correction, shell argv and source-file round trips."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'bench'))
from native_normalize_bench import HEADER

MAIN = r'''
int main(int argc,char**argv){
 if(argc!=2)return 2;
 fixture_schema="{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}}}";
 char *normalized=tools_normalize_input("bash",argv[1]);
 puts(normalized?normalized:argv[1]);free(normalized);return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--before', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binaries = {}
    hashes = {}
    for label, path in [('before', args.before), ('after', ROOT / 'src/tools.c')]:
        source = path.read_text()
        start = source.index('static const char *tools_json_ws(')
        end = source.index('static bool tools_code_or_file_tool(', start)
        code = HEADER + source[start:end] + MAIN
        cfile = out / (label + '.c')
        cfile.write_text(code)
        binary = out / label
        subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I' + str(ROOT / 'include'), str(cfile),
                        str(ROOT / 'src/json_util.c'), '-lm', '-o', str(binary)],
                       capture_output=True, text=True, check=True)
        binaries[label] = binary
        hashes[label] = hashlib.sha256(source[start:end].encode()).hexdigest()

    def normalize(label, raw):
        cp = subprocess.run([str(binaries[label]), raw], capture_output=True, text=True, timeout=5)
        assert cp.returncode == 0, cp.stderr
        return cp.stdout.rstrip('\n')

    fixtures = [
        ('ASCII', r'''{"command":"printf '%s' ascii"}'''),
        ('common-escapes', r'''{"content":"line\n\t\"quoted\"\\path"}'''),
        ('shell-metacharacters', r'''{"command":"printf '%s' '\u003e\u0026'"}'''),
        ('heredoc', r'''{"command":"cat \u003c\u003c'EOF'\nFIXTURE\nEOF\n"}'''),
        ('literal-backslash-u', r'''{"command":"printf '%s' '\\u003e\\u0026'"}'''),
        ('source-unicode', r'''{"content":"value = \"\u05d0\"\n"}'''),
        ('source-literal-escape', r'''{"content":"value = \"\\u05d0\"\n"}'''),
        ('surrogate-pair', r'''{"content":"before \ud83d\ude00 after"}'''),
        ('lone-surrogate', r'''{"content":"before \ud800 after"}'''),
        ('NUL-preserved-raw', r'''{"content":"before \u0000 after"}'''),
        ('mixed-literal-and-Unicode', r'''{"content":"\\u003e \u003e end"}'''),
        ('Unicode-key', r'''{"\u0063ommand":"printf '%s' '\u003e'"}'''),
    ]
    rows = []
    for name, raw in fixtures:
        before = normalize('before', raw)
        after = normalize('after', raw)
        expected = json.loads(raw)
        if name == 'lone-surrogate':
            expected['content'] = expected['content'].replace('\ud800', '\ufffd')
        assert json.loads(after) == expected, (name, raw, after, expected)
        rows.append({'case': name, 'raw_json': raw, 'before': before, 'after': after,
                     'corrected': json.loads(before) != expected})
    for raw in [r'''{"command":"\u12x4"}''', r'''{"command":"\u12"}''',
                r'''{"command":"\u003e\"''']:
        after = normalize('after', raw)
        assert after == raw, (raw, after)
        rows.append({'case': 'malformed-preserved-for-validator', 'raw_json': raw, 'after': after})

    # Execute only fixture commands after exact production normalization.
    # This is an argv boundary proof, not a replacement for capability tests.
    shell = []
    with tempfile.TemporaryDirectory(prefix='dsco-unicode-roundtrip-') as work:
        for name, expected_stdout in [('shell-metacharacters', '>&'), ('heredoc', 'FIXTURE\n'),
                                       ('literal-backslash-u', r'\u003e\u0026')]:
            row = next(row for row in rows if row['case'] == name)
            command = json.loads(row['after'])['command']
            cp = subprocess.run(['/bin/bash', '-c', command], cwd=work, capture_output=True, text=True, timeout=5)
            assert cp.returncode == 0 and cp.stdout == expected_stdout, (name, cp.stdout, cp.stderr)
            shell.append({'case': name, 'argv_command': command, 'stdout': cp.stdout, 'exit': cp.returncode})
        for name in ['source-unicode', 'source-literal-escape']:
            row = next(row for row in rows if row['case'] == name)
            content = json.loads(row['after'])['content']
            path = Path(work) / 'fixture.py'
            path.write_text(content)
            assert path.read_bytes() == json.loads(row['raw_json'])['content'].encode()
            cp = subprocess.run([sys.executable, '-c', 'from fixture import value; print(value)'],
                                cwd=work, capture_output=True, text=True, timeout=5)
            assert cp.returncode == 0 and cp.stdout == '\u05d0\n', (name, cp.stdout, cp.stderr)
    result = {'passed': True, 'normalization_cases': len(rows), 'shell': shell, 'cases': rows,
              'source_hashes': hashes, 'sanitizers': ['address', 'undefined'],
              'historical_raw_arguments_available': False}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'normalization_cases': len(rows), 'shell_cases': len(shell),
                      'source_roundtrips': 2, 'corrected_cases': sum(row.get('corrected', False) for row in rows)}, indent=2))


if __name__ == '__main__':
    main()
