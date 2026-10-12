#!/usr/bin/env python3
"""Official model documentation parser + live GET smoke (no inference)."""
import ctypes
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

class Metadata(ctypes.Structure):
    _fields_ = [('context_tokens', ctypes.c_int), ('max_input_tokens', ctypes.c_int),
                ('max_output_tokens', ctypes.c_int), ('observed_at', ctypes.c_long),
                ('source', ctypes.c_char * 192)]

with tempfile.TemporaryDirectory() as tmp:
    stub = pathlib.Path(tmp) / 'pool.c'
    stub.write_text('void dsco_http_pool_apply(void *c) { (void)c; }\n')
    so = pathlib.Path(tmp) / 'metadata.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-Iinclude', 'src/model_official_metadata.c', str(stub),
                    '-lcurl', '-o', str(so)], cwd=ROOT, check=True)
    lib = ctypes.CDLL(str(so))
    lib.model_official_metadata_parse.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                                   ctypes.c_bool, ctypes.POINTER(Metadata)]
    lib.model_official_metadata_parse.restype = ctypes.c_bool
    lib.model_official_metadata_refresh.argtypes = [ctypes.c_char_p, ctypes.POINTER(Metadata)]
    lib.model_official_metadata_refresh.restype = ctypes.c_bool

    def parse(model, body, anthropic):
        result = Metadata()
        ok = lib.model_official_metadata_parse(model.encode(), body.encode(), anthropic, ctypes.byref(result))
        return ok, result

    body = '''# GPT-6 Sol\nModel ID: `gpt-6-sol`\n- 1,050,000 context window\n- 128,000 max output tokens\n- Maximum input tokens: 922,000\n'''
    ok, r = parse('gpt-6-sol', body, False)
    assert ok and (r.context_tokens, r.max_input_tokens, r.max_output_tokens) == (1050000, 922000, 128000)
    assert not parse('gpt-6-fake', body, False)[0]
    assert not parse('gpt-6-sol', '# GPT-6 Sol context window\nModel ID: `gpt-6-sol`\n', False)[0]
    table = '''| Feature | Claude Sonnet 5 | Claude Haiku 4.5 |\n| Claude API ID | `claude-sonnet-5` | `claude-haiku-4-5-20251001` |\n| [Context window](https://docs.example/context) | 1M tokens | 200K tokens |\n| Max output | 128K tokens | 64K tokens |\n'''
    ok, r = parse('claude-sonnet-5', table, True)
    assert ok and (r.context_tokens, r.max_output_tokens) == (1000000, 128000)
    ok, r = parse('claude-haiku-4-5-20251001', table, True)
    assert ok and (r.context_tokens, r.max_output_tokens) == (200000, 64000)
    assert not parse('claude-hallucinated', table, True)[0]
    assert not lib.model_official_metadata_refresh(b'gpt-6-sol;touch /tmp/pwn', ctypes.byref(Metadata()))

    for model, expect in ((b'gpt-6-sol', 1050000), (b'claude-sonnet-5', 1000000)):
        result = Metadata()
        assert lib.model_official_metadata_refresh(model, ctypes.byref(result)), model
        assert result.context_tokens == expect, (model, result.context_tokens)
        print(model.decode(), 'published_context=', result.context_tokens,
              'published_output=', result.max_output_tokens, 'source=', result.source.decode())
print('PASS: official model metadata parser, live vendor docs, ID matching and injection rejection')
