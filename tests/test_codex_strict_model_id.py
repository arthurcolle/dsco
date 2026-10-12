#!/usr/bin/env python3
"""Production Codex request preserves a strict lane ID with a stale catalog."""
import ast
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
provider = (ROOT / "src/provider.c").read_text()
fixture_ast = ast.parse((ROOT / "tests/test_codex_reasoning_replay.py").read_text())
head = next(ast.literal_eval(node.value) for node in fixture_ast.body
            if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == "HEAD" for target in node.targets))
head = re.sub(r"static const char \*chatgpt_model_id\([^\n]+\n", "", head)
head += r'''
#include <strings.h>
static const char *provider_request_model_id(const char*p,const char*m){(void)p;return m;}
const char *codex_cache_default_model(void){return "fixture-default-model";}
bool codex_cache_model_supported(const char*m){return !strcmp(m,"fixture-known-model");}
'''


def function(name):
    match = re.search(r"^static [^\n;]*\b" + name + r"\([^;{]*\)\s*\{", provider, re.M)
    assert match, name
    return provider[match.start():provider.index("\n}\n", match.start()) + 3]


main = r'''
static void check(const char*flag,const char*requested,const char*expected){
 if(flag)setenv("DSCO_STRICT_MODEL_ID",flag,1);else unsetenv("DSCO_STRICT_MODEL_ID");
 session_state_t s={0};snprintf(s.model,sizeof(s.model),"%s",requested);
 char*body=chatgpt_native_build_request(NULL,NULL,&s,128,NULL);
 assert(body&&json_is_valid_container(body));
 char*model=json_get_str(body,"model");assert(model&&!strcmp(model,expected));
 assert(!json_get_raw(body,"service_tier"));
 free(model);free(body);
}
int main(void){
 check(NULL,"fixture-unknown-model","fixture-default-model");
 check("0","fixture-unknown-model","fixture-default-model");
 check(NULL,"fixture-known-model","fixture-known-model");
 check("1","fixture-unknown-model","fixture-unknown-model");
 check("1","openai/fixture-new-checkpoint","fixture-new-checkpoint");
 check("1","chatgpt/fixture-new-checkpoint","fixture-new-checkpoint");
 check("1","fixture-known-model","fixture-known-model");
 check("true","fixture-unknown-model","fixture-unknown-model");
 puts("PASS: 8 production wire requests; strict IDs preserved despite stale cache; legacy fallback unchanged");
}
'''

with tempfile.TemporaryDirectory(prefix="dsco-strict-model-") as directory:
    folder = Path(directory)
    source = folder / "test.c"
    source.write_text(head + "\n".join(function(name) for name in (
        "provider_env_truthy", "chatgpt_model_id", "chatgpt_append_tool_items",
        "chatgpt_append_message_item", "chatgpt_native_build_request")) + main)
    subprocess.run(["cc", "-std=c11", "-D_DARWIN_C_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                    "-fsanitize=address,undefined", "-g", "-I", str(ROOT / "include"),
                    str(source), str(ROOT / "src/json_util.c"), "-o", str(folder / "test")], check=True)
    subprocess.run([str(folder / "test")], check=True)
    # The controller must distinguish a supporting binary from an older one
    # that ignores the environment flag. These are read-only CLI requests.
    for flag, expected in (("0", "no"), ("1", "yes")):
        env = {"PATH": "/usr/bin:/bin", "HOME": str(folder), "TMPDIR": str(folder),
               "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
               "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_SETUP_NO_AUTO_BOOTSTRAP": "1",
               "DSCO_DISABLE_SHARED_HOME_OAUTH": "1", "DSCO_SECURE_STORE_NO_PROMPT": "1",
               "DSCO_DISABLE_CODEX_OAUTH_DISCOVERY": "1",
               "DSCO_CHATGPT_OAUTH_TOKEN": "fixture-only", "DSCO_CHATGPT_ACCOUNT_ID": "fixture-account",
               "DSCO_STRICT_MODEL_ID": flag}
        result = subprocess.run([str(ROOT / "dsco"), "--provider", "openai-codex",
                                 "--route-explain", "fixture-unknown-model"],
                                cwd=folder, env=env, capture_output=True, text=True, timeout=20)
        assert result.returncode == 0, result.stderr
        assert f"strict_model_id: {expected}\n" in result.stdout, result.stdout
    print("PASS: built CLI advertises strict-model enforcement when enabled, without inference")
