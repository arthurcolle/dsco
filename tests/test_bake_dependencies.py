#!/usr/bin/env python3
"""Exercise the production bake rules in isolation without touching real keys."""
import pathlib
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parents[1]
text = (root / 'Makefile').read_text()
start = text.index('.PHONY: bake_data\n')
end = text.index('\n\n', text.index('\t@test -f "$@"', start))
rules = text[start:end]
with tempfile.TemporaryDirectory(prefix='dsco-bake-test-') as tmp:
    p = pathlib.Path(tmp)
    for d in ('scripts', 'data', 'include', 'src/generated', 'build'):
        (p / d).mkdir(parents=True)
    (p / 'scripts/bake_data.py').write_text('# mock dependency\n')
    (p / 'data/input').write_text('fixture\n')
    (p / 'scripts/bake_data.sh').write_text(
        'set -eu\necho bake >> calls\n'
        'touch src/generated/blob.c include/embedded_data_registry.h include/embedded_key.gen.h\n')
    (p / 'Makefile').write_text(
        'BUILD_DIR=build\nBAKED_DATA=data/input\n'
        'GENERATED_C=src/generated/blob.c\n'
        'GENERATED_REGISTRY=include/embedded_data_registry.h\n' + rules + '\n')
    def run(*targets):
        subprocess.run(['make', '--no-print-directory', *targets], cwd=p, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def count():
        return len((p / 'calls').read_text().splitlines())
    run('-j4', 'src/generated/blob.c', 'include/embedded_data_registry.h', 'include/embedded_key.gen.h')
    assert count() == 1, 'fresh parallel build must bake once'
    run('bake_data')
    assert count() == 1, 'unchanged build must not rebake'
    time.sleep(1.1)
    (p / 'scripts/bake_data.py').touch()
    run('bake_data')
    assert count() == 2, 'Python baker changes must invalidate stamp'
    (p / 'include/embedded_key.gen.h').unlink()
    run('include/embedded_key.gen.h')
    assert count() == 3, 'missing key header must regenerate through shared stamp'
    assert (p / 'include/embedded_key.gen.h').is_file()
print('PASS: fresh parallel bake, no-op rebuild, baker invalidation, missing key recovery')
