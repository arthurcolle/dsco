# DSCO Examples

This cookbook gives copyable starting points for `dsco-cli`. Examples use
`./dsco` from a checkout root. Replace paths, ports, model names, and credentials
for your machine.

Some examples require provider keys, local model servers, MCP servers, or market
API credentials. Provider-specific examples show the intended route explicitly so
saved defaults such as `DSCO_EXEC` or `DSCO_MODEL` do not surprise you.

## Command Conventions

- Put options before or after the prompt: both `./dsco -m openai/gpt-5.5 "task"`
  and `./dsco "task" -m openai/gpt-5.5` are valid.
- Use `-p`, `--p`, or `--prompt` when the prompt should be explicit.
- Use `--provider NAME` to force DSCO's native provider path.
- Use `-e claude` or `-e codex` for external CLI executor paths.
- Use `--` only for executor passthrough arguments.
- For direct tools, quote JSON with single quotes in zsh/bash.

## Everyday One-Shots

1. **001** `./dsco "summarize this repository in ten precise bullets"`
2. **002** `./dsco -p "find the highest-risk TODO comments and rank them"`
3. **003** `./dsco "read README.md and tell me the fastest way to start"`
4. **004** `./dsco "explain what changed in the current dirty worktree"`
5. **005** `./dsco "find all C source files larger than 2000 lines and summarize why"`
6. **006** `./dsco "inspect the build system and explain the default target"`
7. **007** `./dsco "find the C programming language PDFs on this computer; return paths first"`
8. **008** `./dsco "make a focused checklist for debugging the current failing command"`
9. **009** `./dsco "read docs/INDEX.md and propose the best next doc to improve"`
10. **010** `./dsco "scan this repo for generated files that should not be edited by hand"`
11. **011** `./dsco "find stale documentation that disagrees with --help output"`
12. **012** `./dsco "create a concise runbook for reproducing the last terminal error"`

## Prompt Forms And Quoting

1. **013** `./dsco -p "use an explicit one-shot prompt"`
2. **014** `./dsco --prompt "use the long prompt alias"`
3. **015** `./dsco --p "use the short long-form prompt alias"`
4. **016** `./dsco "prompt first still works" -m openai/gpt-5.5`
5. **017** `./dsco -m openai/gpt-5.5 "model first still works"`
6. **018** `./dsco -m openai/gpt-5.5 -p "model plus explicit prompt"`
7. **019** `./dsco --model openai/gpt-5.5 --prompt "long option model and prompt"`
8. **020** `./dsco "quote JSON examples carefully: {\"ok\": true}"`
9. **021** `./dsco 'single quotes preserve shell metacharacters like $HOME and *'`
10. **022** `./dsco "double quotes allow shell expansion like $PWD when desired"`
11. **023** `./dsco -e codex "inspect src/main.c" -- --sandbox read-only`
12. **024** `./dsco --provider openrouter -m openrouter/openai/gpt-5.5 "route through OpenRouter"`

## Provider Routing

1. **025** `./dsco --provider openai -m openai/gpt-5.5 "answer with the active provider"`
2. **026** `./dsco --provider openrouter -m openrouter/anthropic/claude-sonnet-4-6 "review this file tree"`
3. **027** `./dsco --provider anthropic -m anthropic/claude-sonnet-4-6 "audit the API surface"`
4. **028** `./dsco --provider google -m google/gemini-2.5-pro "summarize docs/OPERATIONS.md"`
5. **029** `./dsco --provider groq -m llama-3.3-70b-versatile "give a quick repo orientation"`
6. **030** `./dsco --provider deepseek -m deepseek-chat "find likely parser bugs"`
7. **031** `./dsco --provider mistral -m mistralai/mistral-large-latest "compare architecture docs"`
8. **032** `./dsco --provider xai -m x-ai/grok-4-fast "trace startup routing"`
9. **033** `./dsco --provider together -m meta-llama/Llama-4-Maverick-17B-128E-Instruct-FP8 "draft a plan"`
10. **034** `./dsco --provider perplexity -m perplexity/sonar-pro "answer with citations if web is enabled"`
11. **035** `./dsco --provider cerebras -m cerebras/qwen-3-235b-a22b-instruct-2507 "fast code review"`
12. **036** `./dsco --provider cohere -m cohere/command-a-03-2025 "summarize release risks"`

## Expanded Providers

1. **037** `./dsco --provider alibaba -m dashscope/qwen3-coder-plus "inspect command parsing"`
2. **038** `./dsco --provider alibaba-coding-plan -m dashscope-coding/qwen3-coder-plus "fix a C warning"`
3. **039** `./dsco --provider arcee -m arcee/auto "classify source modules by ownership"`
4. **040** `AZURE_BASE_URL=https://example.openai.azure.com/openai/deployments/dsco/v1 ./dsco --provider azure -m azure/gpt-4.1 "use Azure Foundry"`
5. **041** `./dsco --provider gmi -m gmi/auto "triage failing tests"`
6. **042** `./dsco --provider huggingface -m hf/meta-llama/Llama-3.3-70B-Instruct "summarize src/provider.c"`
7. **043** `./dsco --provider kilocode -m kilo/auto "draft a patch plan"`
8. **044** `./dsco --provider nous -m nous/auto "explain the swarm tool"`
9. **045** `./dsco --provider novita -m novita/meta-llama/llama-3.1-8b-instruct "write a test matrix"`
10. **046** `./dsco --provider nvidia -m nvidia/meta/llama-3.1-70b-instruct "review provider aliases"`
11. **047** `./dsco --provider stepfun -m step/auto "summarize code paths"`
12. **048** `./dsco --provider zai -m zai/glm-5.2 "default general-purpose routing example"`

## External Executors

1. **049** `./dsco -e claude "use Claude Code for a codebase edit"`
2. **050** `./dsco -e codex "use Codex CLI for repository inspection"`
3. **051** `./dsco -e auto "let DSCO choose between external executors"`
4. **052** `./dsco -e smart "classify this task and choose an execution path"`
5. **053** `./dsco -e list`
6. **054** `./dsco -e bench "What is 7 factorial? Reply with only the number."`
7. **055** `./dsco -e bench-tools "Read README.md and report the first heading."`
8. **056** `./dsco -e smoke`
9. **057** `./dsco -e smoke-full`
10. **058** `DSCO_EXEC=codex ./dsco -m openai/gpt-5.5 "force a Codex-compatible OpenAI model"`
11. **059** `DSCO_EXEC=claude ./dsco -m anthropic/claude-sonnet-4-6 "force a Claude-compatible model"`
12. **060** `DSCO_EXEC=auto ./dsco "allow saved auto executor selection"`

## Local Models

1. **061** `./dsco --local "run this against the default LM Studio local path"`
2. **062** `./dsco --local -m lmstudio:local-model "pin an LM Studio served model"`
3. **063** `./dsco --ollama "use the default local Ollama model"`
4. **064** `./dsco --ollama -m ollama:llama3.2 "pin Ollama llama3.2"`
5. **065** `./dsco --pull-and-use llama3.2 "pull then use an Ollama model"`
6. **066** `./dsco --pull-and-use qwen2.5-coder:7b "pull and use a coding model"`
7. **067** `./dsco --provider mlx -m mlx:local-model "use a local MLX server"`
8. **068** `./dsco --provider vllm -m vllm:Qwen2.5-Coder "use a vLLM OpenAI-compatible server"`
9. **069** `./dsco --provider llamacpp -m llamacpp:local-model "use llama.cpp server mode"`
10. **070** `./dsco --provider localai -m localai:assistant "use LocalAI"`
11. **071** `./dsco --provider tgi -m tgi:meta-llama "use a local TGI endpoint"`
12. **072** `./dsco --tool-exec ol_call '{"provider":"ollama","model":"llama3.2","prompt":"say ok"}'`

## Setup And Auth

1. **073** `./dsco login`
2. **074** `./dsco --login`
3. **075** `./dsco status`
4. **076** `./dsco --setup`
5. **077** `./dsco --setup-force`
6. **078** `./dsco --setup-report`
7. **079** `OPENROUTER_API_KEY=... ./dsco --provider openrouter -m openrouter/openai/gpt-5.5 "test key routing"`
8. **080** `ANTHROPIC_API_KEY=... ./dsco --provider anthropic -m anthropic/claude-sonnet-4-6 "test Anthropic"`
9. **081** `FUGU_API_KEY=... ./dsco --provider sakana -m sakana/fugu "test Fugu explicitly"`
10. **082** `DSCO_ENV_FILE=/tmp/dsco.env ./dsco --setup-report`
11. **083** `DSCO_DISABLE_CODEX_OAUTH_DISCOVERY=1 ./dsco --provider openai -m openai/gpt-5.5 "avoid Codex subscription discovery"`
12. **084** `DSCO_PREFER_METERED_API=1 ./dsco --provider sakana -m fugu "prefer metered Sakana when configured"`

## Repository Recon

1. **085** `./dsco "map the top-level directories and explain what each owns"`
2. **086** `./dsco "find where provider routing is implemented and cite files"`
3. **087** `./dsco "trace the CLI parse path for --provider and -m"`
4. **088** `./dsco "find all generated docs and their generator scripts"`
5. **089** `./dsco "identify modules with the most direct dependencies"`
6. **090** `./dsco "rank source files by likely blast radius for parser changes"`
7. **091** `./dsco "find all tests that mention openrouter and summarize coverage"`
8. **092** `./dsco "build a table of env vars used by provider.c"`
9. **093** `./dsco "find every place DSCO_EXEC is read or written"`
10. **094** `./dsco "explain the difference between provider profiles and endpoints"`
11. **095** `./dsco "locate all startup fast paths and explain when they run"`
12. **096** `./dsco "find all files touched by docs generation"`

## Code Search Direct Tools

1. **097** `./dsco --tool-exec cwd '{}'`
2. **098** `./dsco --tool-exec list_directory '{"path":"src"}'`
3. **099** `./dsco --tool-exec find_files '{"path":"src","pattern":"*.c"}'`
4. **100** `./dsco --tool-exec grep_files '{"path":"src","pattern":"provider_route_for_model"}'`
5. **101** `./dsco --tool-exec read_file '{"path":"src/main.c","offset":3000,"limit":120}'`
6. **102** `./dsco --tool-exec page_file '{"path":"docs/TOOL_CATALOG.md","page":1,"page_size":80}'`
7. **103** `./dsco --tool-exec head_tail '{"path":"README.md","action":"head","lines":40}'`
8. **104** `./dsco --tool-exec head_tail '{"path":"CHANGELOG.md","action":"tail","lines":60}'`
9. **105** `./dsco --tool-exec file_info '{"path":"dsco"}'`
10. **106** `./dsco --tool-exec file_hash '{"path":"src/provider.c"}'`
11. **107** `./dsco --tool-exec file_tree '{"path":"docs","max_depth":2}'`
12. **108** `./dsco --tool-exec word_count '{"path":"docs/OPERATIONS.md"}'`

## Code Intelligence

1. **109** `./dsco --tool-exec self_inspect '{"path":"src"}'`
2. **110** `./dsco --tool-exec inspect_file '{"path":"src/provider.c"}'`
3. **111** `./dsco --tool-exec call_graph '{"path":"src","function":"provider_create"}'`
4. **112** `./dsco --tool-exec dependency_graph '{"path":"src"}'`
5. **113** `./dsco --tool-exec code_index '{"path":"src"}'`
6. **114** `./dsco --tool-exec code_search '{"query":"provider profile canonical name"}'`
7. **115** `./dsco "use AST tools to explain provider_create without guessing"`
8. **116** `./dsco "find dead-looking static functions in src/provider.c and verify references"`
9. **117** `./dsco "map tests to the functions they protect in provider.c"`
10. **118** `./dsco "find unsafe string handling in src/main.c and propose fixes"`
11. **119** `./dsco "compare include/provider.h against src/provider.c implementations"`
12. **120** `./dsco "find public declarations that lack docs coverage"`

## Editing Workflows

1. **121** `./dsco "add a regression test for --model after positional prompt"`
2. **122** `./dsco "rename this helper only within src/main.c and update tests"`
3. **123** `./dsco "add a focused error message for missing provider credentials"`
4. **124** `./dsco "make this parser branch return a usage hint and verify it"`
5. **125** `./dsco "split this long function only if the local style supports it"`
6. **126** `./dsco "change docs only; do not touch source"`
7. **127** `./dsco "make the minimal source change and leave unrelated dirty files alone"`
8. **128** `./dsco "add a test before fixing the bug, then make it pass"`
9. **129** `./dsco "update README and docs/INDEX for this new command"`
10. **130** `./dsco "regenerate generated docs only after explaining which script owns them"`
11. **131** `./dsco "apply a small refactor and show the exact behavior preserved"`
12. **132** `./dsco "remove duplicated provider alias checks if tests stay green"`

## Build And Test

1. **133** `make`
2. **134** `make -j8`
3. **135** `make test_runner`
4. **136** `./test_runner`
5. **137** `make docs-check`
6. **138** `./dsco --version`
7. **139** `./dsco --help`
8. **140** `./dsco --models-json`
9. **141** `./dsco --tools-json`
10. **142** `./dsco -e smoke`
11. **143** `./dsco -e smoke-full`
12. **144** `DSCO_DEBUG_REQUEST=1 ./dsco --provider openrouter -m openrouter/openai/gpt-5.5 "debug request shape"`

## Debugging Failures

1. **145** `./dsco "reproduce the last command exactly and explain the failing branch"`
2. **146** `./dsco "find why this flag is parsed as --profile instead of prompt"`
3. **147** `./dsco "inspect DSCO_EXEC and DSCO_MODEL interactions for this command"`
4. **148** `./dsco "check whether a PATH-installed dsco differs from ./dsco"`
5. **149** `./dsco "find which process owns the dsco HTTP API port range"`
6. **150** `./dsco "separate provider routing failure from provider HTTP failure"`
7. **151** `./dsco "read ~/.dsco/debug/last_failed_request.json and summarize model/prompt"`
8. **152** `./dsco "explain whether this is parser, routing, auth, or upstream error"`
9. **153** `./dsco "turn this terminal transcript into a minimal regression test"`
10. **154** `./dsco "find why a local Ollama endpoint returns HTTP 404"`
11. **155** `./dsco "investigate readonly database warnings without masking test status"`
12. **156** `./dsco "debug this 429 as provider exhaustion, then choose a fallback plan"`

## Git Workflows

1. **157** `./dsco --tool-exec git '{"action":"status"}'`
2. **158** `./dsco --tool-exec git '{"action":"diff"}'`
3. **159** `./dsco --tool-exec git '{"action":"log","limit":5}'`
4. **160** `./dsco "review the unstaged diff for correctness risks"`
5. **161** `./dsco "summarize only my changes in src/main.c and src/provider.c"`
6. **162** `./dsco "write a PR description from the current diff"`
7. **163** `./dsco "find unrelated dirty files and list them without changing them"`
8. **164** `./dsco "create a commit message for the provider parser fix"`
9. **165** `./dsco "inspect the last commit and identify potential regressions"`
10. **166** `./dsco "compare this branch against main and group changes by subsystem"`
11. **167** `./dsco "find generated-doc diffs and trace them to scripts"`
12. **168** `./dsco "prepare a release-note bullet for the CLI flag change"`

## Documentation

1. **169** `./dsco "add docs for --model and --prompt to README"`
2. **170** `./dsco "create a migration note for users with DSCO_EXEC=codex"`
3. **171** `./dsco "update docs/OPERATIONS.md provider troubleshooting"`
4. **172** `./dsco "add examples for Azure custom base configuration"`
5. **173** `./dsco "audit docs for stale references to -p as profile"`
6. **174** `./dsco "regenerate API docs and summarize generated changes"`
7. **175** `./scripts/gen_api_reference.sh`
8. **176** `./scripts/gen_tool_catalog.sh`
9. **177** `python3 scripts/gen_external_tool_catalog.py --root .`
10. **178** `python3 scripts/gen_repo_coverage.py --root .`
11. **179** `./dsco "write a docs-only changelog entry"`
12. **180** `./dsco "turn this examples file into a shorter quickstart"`

## Direct File Operations

1. **181** `./dsco --tool-exec write_file '{"path":"/tmp/dsco-note.txt","content":"hello\n"}'`
2. **182** `./dsco --tool-exec append_file '{"path":"/tmp/dsco-note.txt","content":"next line\n"}'`
3. **183** `./dsco --tool-exec copy_file '{"src":"/tmp/dsco-note.txt","dest":"/tmp/dsco-note-copy.txt"}'`
4. **184** `./dsco --tool-exec move_file '{"src":"/tmp/dsco-note-copy.txt","dest":"/tmp/dsco-note-moved.txt"}'`
5. **185** `./dsco --tool-exec mkdir '{"path":"/tmp/dsco-example-dir","parents":true}'`
6. **186** `./dsco --tool-exec delete_file '{"path":"/tmp/dsco-note-moved.txt"}'`
7. **187** `./dsco --tool-exec chmod_tool '{"path":"/tmp/dsco-note.txt","mode":"600"}'`
8. **188** `./dsco "create a temp report under /tmp and verify the file exists"`
9. **189** `./dsco "copy docs/INDEX.md to /tmp, edit the copy, and show the diff"`
10. **190** `./dsco "append a timestamped note to a scratch file and verify bytes"`
11. **191** `./dsco "find duplicate markdown headings under docs"`
12. **192** `./dsco "search for PDFs under ~/Documents and list size plus modified time"`

## JSON, CSV, XML, And Text

1. **193** `./dsco --tool-exec jq '{"json":"{\"a\":1,\"b\":2}","expr":".a"}'`
2. **194** `./dsco --tool-exec json_format '{"json":"{\"b\":2,\"a\":1}","mode":"pretty"}'`
3. **195** `./dsco --tool-exec csv_parse '{"text":"name,score\nada,10\n","headers":true}'`
4. **196** `./dsco --tool-exec regex_match '{"text":"abc 123 def 456","pattern":"[0-9]+","global":true}'`
5. **197** `./dsco --tool-exec template_render '{"template":"Hello {{name}}","vars":{"name":"DSCO"}}'`
6. **198** `./dsco --tool-exec text_diff '{"text_a":"one\ntwo\n","text_b":"one\nthree\n"}'`
7. **199** `./dsco --tool-exec xml_extract '{"text":"<root><title>DSCO</title></root>","tag":"title"}'`
8. **200** `./dsco --tool-exec string_transform '{"text":"Hello World","action":"slugify"}'`
9. **201** `./dsco --tool-exec privacy_filter '{"text":"email me at user@example.com"}'`
10. **202** `./dsco "parse this pasted CSV and rank rows by score"`
11. **203** `./dsco "convert this JSON blob into a markdown table"`
12. **204** `./dsco "diff these two config snippets and explain behavioral changes"`

## Math And Crypto

1. **205** `./dsco --tool-exec calc '{"expr":"sqrt(144)+7"}'`
2. **206** `./dsco --tool-exec eval '{"expr":"2^10 + 3*7"}'`
3. **207** `./dsco --tool-exec big_factorial '{"n":100}'`
4. **208** `./dsco --tool-exec sha256 '{"text":"hello"}'`
5. **209** `./dsco --tool-exec md5 '{"text":"hello"}'`
6. **210** `./dsco --tool-exec hmac '{"key":"secret","text":"message"}'`
7. **211** `./dsco --tool-exec hkdf '{"ikm_hex":"0011223344556677","salt_hex":"00","info":"dsco","length":32}'`
8. **212** `./dsco --tool-exec random_bytes '{"n":32}'`
9. **213** `./dsco --tool-exec uuid '{}'`
10. **214** `./dsco --tool-exec base64_tool '{"action":"encode","text":"hello"}'`
11. **215** `./dsco --tool-exec jwt_decode '{"token":"header.payload.signature"}'`
12. **216** `./dsco "compute break-even token cost for three provider choices"`

## Network And Web

1. **217** `./dsco --tool-exec network '{"action":"dns","host":"example.com"}'`
2. **218** `./dsco --tool-exec port_check '{"host":"127.0.0.1","port":11434}'`
3. **219** `./dsco --tool-exec hostname '{"action":"resolve","host":"example.com"}'`
4. **220** `./dsco --tool-exec http_request '{"method":"GET","url":"https://example.com"}'`
5. **221** `./dsco --tool-exec WebFetch '{"url":"https://example.com"}'`
6. **222** `./dsco --tool-exec WebSearch '{"query":"OpenAI compatible API provider docs"}'`
7. **223** `./dsco --tool-exec jina_search '{"query":"DSCO provider routing examples"}'`
8. **224** `./dsco --tool-exec tavily_search '{"query":"C programming language PDF"}'`
9. **225** `./dsco --tool-exec parallel_search '{"query":"latest C23 standard overview"}'`
10. **226** `./dsco "fetch this URL, extract headings, and summarize only source-backed facts"`
11. **227** `./dsco "check whether localhost ports 7547 through 7554 are occupied"`
12. **228** `./dsco "diagnose why a local OpenAI-compatible server is refusing connections"`

## PDFs, Images, And Browser Tasks

1. **229** `./dsco --tool-exec find_files '{"path":".","pattern":"*.pdf"}'`
2. **230** `./dsco --tool-exec view_pdf '{"path":"docs/example.pdf"}'`
3. **231** `./dsco --tool-exec view_image '{"path":"assets/screenshot.png"}'`
4. **232** `./dsco --tool-exec browser '{"action":"snapshot","url":"https://example.com"}'`
5. **233** `./dsco "find local PDFs about C, group by directory, and identify likely books"`
6. **234** `./dsco "extract the title pages from PDFs under docs if readable"`
7. **235** `./dsco "inspect this screenshot and list UI defects"`
8. **236** `./dsco "compare two screenshots and explain visual regressions"`
9. **237** `./dsco "open a browser snapshot and extract the navigation outline"`
10. **238** `./dsco "find image assets that are unused by docs or README"`
11. **239** `./dsco "scan PDFs for C standard references without copying full text"`
12. **240** `./dsco "build a bibliography from local PDF filenames and metadata"`

## Orchestration And Topologies

1. **241** `./dsco --topology-list`
2. **242** `./dsco --topology-show fanout`
3. **243** `./dsco --topology-auto "audit provider routing with parallel specialists"`
4. **244** `./dsco --topology fanout "split docs, tests, and source review"`
5. **245** `./dsco --topology hierarchy "perform a lead-plus-workers architecture review"`
6. **246** `./dsco --topology specialist "assign provider, parser, docs, and tests specialists"`
7. **247** `./dsco --tool-exec swarm '{"action":"status"}'`
8. **248** `./dsco --tool-exec swarm '{"action":"topology_list"}'`
9. **249** `./dsco --tool-exec agent '{"action":"status"}'`
10. **250** `./dsco --tool-exec agent_wait '{"timeout_ms":60000}'`
11. **251** `DSCO_TOPO_THROUGHPUT=1 ./dsco --topology-auto "large parallel repo audit"`
12. **252** `DSCO_PROVIDER_THROUGHPUT=openrouter,openai-codex ./dsco --topology-auto "fan out across provider lanes"`

## Workflow, Plans, And State

1. **253** `./dsco --tool-exec workflow '{"action":"smoke"}'`
2. **254** `./dsco --tool-exec workflow '{"action":"plan","name":"docs-update","steps":"inspect;edit;verify"}'`
3. **255** `./dsco --tool-exec workflow '{"action":"validate","steps":"inspect;edit;verify"}'`
4. **256** `./dsco --tool-exec plan_state '{"action":"init","plan_id":"example-plan"}'`
5. **257** `./dsco --tool-exec control_flow '{"action":"parse","program":"if ok then test"}'`
6. **258** `./dsco --tool-exec recovery '{"action":"log_dump"}'`
7. **259** `./dsco "create a stepwise plan, implement it, and update statuses as you go"`
8. **260** `./dsco "turn this task into a workflow with checkpoints and rollback points"`
9. **261** `./dsco "identify which steps can run in parallel and which must serialize"`
10. **262** `./dsco "write a validation checklist before editing files"`
11. **263** `./dsco "continue the previous plan from the first incomplete item"`
12. **264** `./dsco "recover from the failed command by trying a safer provider path"`

## Context And Memory

1. **265** `./dsco --tool-exec context_status '{}'`
2. **266** `./dsco --tool-exec context_compact '{}'`
3. **267** `./dsco --tool-exec context_recall '{}'`
4. **268** `./dsco --tool-exec token_audit '{}'`
5. **269** `./dsco --tool-exec scratchpad '{"action":"put","key":"note","value":"remember this for the session"}'`
6. **270** `./dsco --tool-exec session_memory '{"action":"remember","key":"repo-focus","value":"provider routing","ttl":3600}'`
7. **271** `./dsco --tool-exec session_memory '{"action":"recall","key":"repo-focus"}'`
8. **272** `./dsco --tool-exec memory_tier '{"action":"status"}'`
9. **273** `./dsco "summarize current context pressure and what can be compacted"`
10. **274** `./dsco "store this debugging finding in session memory for one hour"`
11. **275** `./dsco "recall the last provider-routing decision and verify it in code"`
12. **276** `./dsco "produce a compact handoff summary for the next agent turn"`

## Integration And MCP

1. **277** `./dsco --tool-exec discover_integrations '{}'`
2. **278** `./dsco --tool-exec dsco_doctor_integrations '{}'`
3. **279** `./dsco --tool-exec discover_tools '{"search":"gmail"}'`
4. **280** `./dsco --tool-exec discover_tools '{"category":"network"}'`
5. **281** `./dsco "inspect ~/.dsco/mcp.json and explain what dsco points to"`
6. **282** `./dsco "compare configured MCP servers with live tool discovery"`
7. **283** `./dsco "find stale integration connector IDs in the local cache"`
8. **284** `./dsco "diagnose why an MCP server is installed but no tools appear"`
9. **285** `./dsco "draft an MCP config entry for a local stdio server"`
10. **286** `./dsco "explain which integrations are mutating and need confirmation"`
11. **287** `./dsco "list OAuth-gated integrations and their likely setup steps"`
12. **288** `./dsco "write a runbook for reloading MCP after config changes"`

## Plugins And Extensions

1. **289** `./dsco --tool-exec plugin_validate '{"manifest":"{\"name\":\"example\",\"version\":\"0.1.0\"}"}'`
2. **290** `./dsco "inspect plugin manifest and lockfile consistency"`
3. **291** `./dsco "explain how plugin tools differ from MCP tools"`
4. **292** `./dsco "find all dsco_plugin_* symbols in this checkout"`
5. **293** `./dsco "draft a plugin manifest for a read-only reporting extension"`
6. **294** `./dsco "audit plugin docs for missing security notes"`
7. **295** `./dsco "validate a plugin lock pin without changing files"`
8. **296** `./dsco "write a minimal plugin test plan"`
9. **297** `./dsco "compare plugin extension points against MCP extension points"`
10. **298** `./dsco "find whether a plugin can register external tools dynamically"`
11. **299** `./dsco "summarize plugin loading order from source"`
12. **300** `./dsco "create docs for a new plugin capability flag"`

## Observability And Runtime State

1. **301** `./dsco --timeline-server --timeline-port 8421`
2. **302** `./dsco --timeline-server --timeline-port 9001 --timeline-instance INSTANCE_ID`
3. **303** `./dsco --workspace-bootstrap`
4. **304** `./dsco --workspace-status`
5. **305** `./dsco --tool-exec ps '{"filter":"dsco"}'`
6. **306** `./dsco --tool-exec process_tree '{"filter":"dsco"}'`
7. **307** `./dsco --tool-exec sysinfo '{}'`
8. **308** `./dsco --tool-exec system_profiler '{"action":"load"}'`
9. **309** `./dsco "find why netsrv ports are unavailable and identify likely owner"`
10. **310** `./dsco "summarize recent baseline events and failure breadcrumbs"`
11. **311** `./dsco "explain whether autosave recovery is available"`
12. **312** `./dsco "write an incident report from chronicle and terminal logs"`

## Security And Safety

1. **313** `./dsco --tool-exec secret_scan '{"path":"."}'`
2. **314** `./dsco --tool-exec privacy_filter '{"text":"call me at 555-0100 or email a@b.com"}'`
3. **315** `./dsco --tool-exec risk_gate '{"action":"rm -rf /","content":"delete everything"}'`
4. **316** `./dsco --tool-exec governance '{"action":"status"}'`
5. **317** `./dsco --tool-exec killswitch '{"action":"status"}'`
6. **318** `./dsco "scan staged changes for accidental secrets"`
7. **319** `./dsco "audit this shell command for destructive behavior before running it"`
8. **320** `./dsco "find files that might contain credentials and recommend safe handling"`
9. **321** `./dsco "explain risk differences between read-only and mutating tools"`
10. **322** `./dsco "write a safe rollback plan for this migration"`
11. **323** `./dsco "check whether generated logs expose API keys"`
12. **324** `./dsco "review this prompt for injection attempts and explain severity"`

## Market, Weather, And Research Data

1. **325** `./dsco --tool-exec weather '{"location":"New York, NY"}'`
2. **326** `./dsco --tool-exec nws '{"action":"alerts","state":"NY"}'`
3. **327** `./dsco --tool-exec synoptic '{"action":"latest","station":"KNYC"}'`
4. **328** `./dsco --tool-exec kalshi '{"action":"markets","limit":5}'`
5. **329** `./dsco --tool-exec polymarket '{"action":"markets","limit":5}'`
6. **330** `./dsco --tool-exec prediction '{"action":"scan"}'`
7. **331** `./dsco --tool-exec contract_landscape '{}'`
8. **332** `./dsco --tool-exec contract_search '{"query":"Bitcoin above 100000"}'`
9. **333** `./dsco --tool-exec alpha_vantage '{"function":"TIME_SERIES_DAILY","symbol":"IBM"}'`
10. **334** `./dsco --tool-exec strategy '{"action":"spread_scan"}'`
11. **335** `./dsco "compare Kalshi and Polymarket prices for equivalent weather markets"`
12. **336** `./dsco "summarize market-data freshness and which caches need refresh"`

## Advanced Agent Control

1. **337** `./dsco --tool-exec self_assess '{}'`
2. **338** `./dsco --tool-exec self_analyze '{}'`
3. **339** `./dsco --tool-exec self_improve '{"action":"summary"}'`
4. **340** `./dsco --tool-exec bg_learn '{"action":"status"}'`
5. **341** `./dsco --tool-exec meta_optimize '{"action":"analyze"}'`
6. **342** `./dsco --tool-exec ooda '{"action":"status"}'`
7. **343** `./dsco --tool-exec talons '{"action":"recommend","goal":"reduce provider routing regressions"}'`
8. **344** `./dsco --tool-exec wings_talons_status '{}'`
9. **345** `./dsco --tool-exec pheromone '{"action":"status"}'`
10. **346** `./dsco --tool-exec avian '{"action":"status"}'`
11. **347** `./dsco "run an OODA loop over this failure and produce a decision log"`
12. **348** `./dsco "recommend a Talons strategy for a large refactor with failing tests"`

## MetaConstruct And Looping

1. **349** `./dsco --tool-exec StartOfLoopConstruct '{"program":"continue when iteration < 2; break when iteration >= 2; prompt = \"check docs\""}'`
2. **350** `./dsco --tool-exec LoopConstructStatus '{}'`
3. **351** `./dsco --tool-exec EndOfLoopConstruct '{"action":"break"}'`
4. **352** `./dsco "use a bounded loop to improve this doc until examples are grouped cleanly"`
5. **353** `./dsco "start with a plan, run one verification loop, then stop"`
6. **354** `./dsco "perform iterative schema cleanup but cap at three turns"`
7. **355** `./dsco "use loop status to explain whether recursive work is still active"`
8. **356** `./dsco "create a small MetaConstruct example that rewrites a prompt once"`
9. **357** `./dsco "test loop exit behavior without making source edits"`
10. **358** `./dsco "turn this retry procedure into bounded loop DSL"`
11. **359** `./dsco "compare manual checklist iteration with MetaConstruct loop control"`
12. **360** `./dsco "write a safety note for recursive agent loops"`
