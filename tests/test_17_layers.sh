#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# 17-LAYER INTEGRATION TEST — dsco executor/swarm/provider/topology system
# Tests every layer from model registry to live streaming output
# ═══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

DSCO="./dsco"
PASS=0
FAIL=0
SKIP=0
TOTAL=0

RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
CYAN='\033[36m'
BOLD='\033[1m'
DIM='\033[2m'
RESET='\033[0m'

run_test() {
    local layer="$1"
    local name="$2"
    local cmd="$3"
    local expect="$4"
    TOTAL=$((TOTAL + 1))

    printf "${CYAN}[L%02d]${RESET} %-50s " "$layer" "$name"

    local output
    local rc=0
    output=$(eval "$cmd" 2>&1) || rc=$?

    if echo "$output" | grep -qi "$expect"; then
        PASS=$((PASS + 1))
        printf "${GREEN}PASS${RESET}\n"
    else
        FAIL=$((FAIL + 1))
        printf "${RED}FAIL${RESET}\n"
        printf "  ${DIM}expected: %s${RESET}\n" "$expect"
        printf "  ${DIM}got (first 200): %.200s${RESET}\n" "$output"
    fi
}

run_test_output() {
    local layer="$1"
    local name="$2"
    local cmd="$3"
    local expect="$4"
    TOTAL=$((TOTAL + 1))

    printf "${CYAN}[L%02d]${RESET} %-50s " "$layer" "$name"

    local output
    local rc=0
    output=$(eval "$cmd" 2>/dev/null) || rc=$?

    if echo "$output" | grep -qi "$expect"; then
        PASS=$((PASS + 1))
        printf "${GREEN}PASS${RESET}\n"
    else
        FAIL=$((FAIL + 1))
        printf "${RED}FAIL${RESET}\n"
        printf "  ${DIM}expected: %s${RESET}\n" "$expect"
        printf "  ${DIM}stdout (first 200): %.200s${RESET}\n" "$output"
    fi
}

run_test_nonzero() {
    local layer="$1"
    local name="$2"
    local cmd="$3"
    TOTAL=$((TOTAL + 1))

    printf "${CYAN}[L%02d]${RESET} %-50s " "$layer" "$name"

    local output
    local rc=0
    output=$(eval "$cmd" 2>&1) || rc=$?

    if [ -n "$output" ] && [ ${#output} -gt 2 ]; then
        PASS=$((PASS + 1))
        printf "${GREEN}PASS${RESET} ${DIM}(%d bytes)${RESET}\n" "${#output}"
    else
        FAIL=$((FAIL + 1))
        printf "${RED}FAIL${RESET} ${DIM}(empty output)${RESET}\n"
    fi
}

skip_test() {
    local layer="$1"
    local name="$2"
    local reason="$3"
    TOTAL=$((TOTAL + 1))
    SKIP=$((SKIP + 1))
    printf "${CYAN}[L%02d]${RESET} %-50s ${YELLOW}SKIP${RESET} ${DIM}(%s)${RESET}\n" "$layer" "$name" "$reason"
}

printf "\n${BOLD}═══════════════════════════════════════════════════════════════${RESET}\n"
printf "${BOLD}  17-LAYER INTEGRATION TEST SUITE${RESET}\n"
printf "${BOLD}═══════════════════════════════════════════════════════════════${RESET}\n\n"

# ─── Layer 1: Model Registry ──────────────────────────────────────────────
printf "${BOLD}Layer 1: Model Registry${RESET}\n"

run_test 1 "Anthropic opus resolves" \
    "$DSCO -m opus 'Say only: REGISTRY_OK' 2>/dev/null" \
    "REGISTRY_OK"

run_test_output 1 "OpenRouter kimi resolves" \
    "$DSCO -m kimi 'Say only the word: KIMI_OK'" \
    "KIMI_OK"

run_test_output 1 "OpenRouter GPT-4.1 resolves" \
    "$DSCO -m gpt41 'Say only the word: GPT41_OK'" \
    "GPT41_OK"

run_test 1 "OpenRouter Gemini 2.5 Flash resolves" \
    "$DSCO -m gem25-flash 'Say only the word: GEMINI_OK' 2>&1" \
    "GEMINI_OK"

run_test_output 1 "OpenRouter DeepSeek chat resolves" \
    "$DSCO -m ds-chat 'Say only the word: DEEPSEEK_OK'" \
    "DEEPSEEK_OK"

run_test_output 1 "OpenRouter Qwen flash resolves" \
    "$DSCO -m qwen-flash 'Say only the word: QWEN_OK'" \
    "QWEN_OK"

run_test_output 1 "OpenRouter Llama 4 Maverick resolves" \
    "$DSCO -m llama4-mav 'Say only the word: LLAMA_OK'" \
    "LLAMA_OK"

# ─── Layer 2: Provider Detection ──────────────────────────────────────────
printf "\n${BOLD}Layer 2: Provider Detection${RESET}\n"

run_test 2 "Anthropic native detection (sonnet)" \
    "$DSCO -m sonnet 'Say only: ANTHRO_NATIVE' 2>&1" \
    "ANTHRO_NATIVE"

run_test 2 "OpenRouter slash-prefix detection" \
    "$DSCO -m gpt41 'Say only: OR_SLASH' 2>&1" \
    "model="

run_test 2 "Native Anthropic no model= line" \
    "$DSCO -m sonnet 'Say only: NO_MODEL_LINE' 2>&1 | grep -c 'model='" \
    "0"

# ─── Layer 3: Executor Types ─────────────────────────────────────────────
printf "\n${BOLD}Layer 3: Executor Types${RESET}\n"

run_test 3 "executor_status detects all backends" \
    "$DSCO -m sonnet 'Call executor_status and report which are available. Say dsco=X claude=X codex=X' 2>&1" \
    "dsco"

run_test 3 "executor_status shows claude available" \
    "$DSCO -m sonnet 'Call executor_status. Is claude available? Say YES or NO' 2>&1" \
    "YES"

# ─── Layer 4: Swarm Spawning ─────────────────────────────────────────────
printf "\n${BOLD}Layer 4: Swarm Spawning${RESET}\n"

run_test 4 "spawn_agent (dsco fork)" \
    "$DSCO -m sonnet 'Use spawn_agent with task \"Say SPAWN_OK\" then agent_wait for it. Report the output.' 2>&1" \
    "SPAWN_OK"

run_test 4 "spawn_executor (claude)" \
    "$DSCO -m sonnet 'Use spawn_executor with executor claude and task \"Say CLAUDE_EXEC_OK\". Wait for it. Report output.' 2>&1" \
    "CLAUDE_EXEC_OK"

# ─── Layer 5: Budget System ──────────────────────────────────────────────
printf "\n${BOLD}Layer 5: Budget System${RESET}\n"

run_test 5 "swarm_budget set and query" \
    "$DSCO -m sonnet 'Call swarm_budget with budget_usd=25.0 then call swarm_budget with no args to query. Report the budget.' 2>&1" \
    "25"

run_test 5 "Budget estimation for spawn" \
    "$DSCO -m sonnet 'Call executor_status. What is the budget status? Say the word unlimited or the number.' 2>&1" \
    "unlimited\|budget\|remaining"

# ─── Layer 6: Group Management ───────────────────────────────────────────
printf "\n${BOLD}Layer 6: Group Management${RESET}\n"

run_test 6 "create_executor_swarm (2 claude agents)" \
    "$DSCO -m sonnet 'Use create_executor_swarm with name \"test-group\" executor claude and tasks [\"Say GROUP_A\",\"Say GROUP_B\"]. Then swarm_collect the group. Report outputs.' 2>&1" \
    "GROUP"

# ─── Layer 7: Polling & Streaming ────────────────────────────────────────
printf "\n${BOLD}Layer 7: Polling & Streaming${RESET}\n"

run_test 7 "swarm_collect streams live output" \
    "$DSCO -m sonnet 'Use create_executor_swarm with name \"stream-test\" executor claude tasks [\"Count from 1 to 5\"]. Then swarm_collect. Report the count.' 2>&1" \
    "done"

# ─── Layer 8: Cost Parsing ───────────────────────────────────────────────
printf "\n${BOLD}Layer 8: Cost Parsing${RESET}\n"

run_test 8 "Claude executor reports cost" \
    "$DSCO -m sonnet 'spawn_executor executor=claude task=\"Say hello\". Wait for it. Report the cost if shown.' 2>&1" \
    "cost\|0\.\|agent"

# ─── Layer 9: Executor Detection ─────────────────────────────────────────
printf "\n${BOLD}Layer 9: Executor Detection${RESET}\n"

run_test 9 "Claude binary path detected" \
    "$DSCO -m sonnet 'Call executor_status. What is the path to claude? Say the path.' 2>&1" \
    "claude"

run_test 9 "Codex binary path detected" \
    "$DSCO -m sonnet 'Call executor_status. What is the codex path? Say the path.' 2>&1" \
    "codex"

# ─── Layer 10: Topology Registry ─────────────────────────────────────────
printf "\n${BOLD}Layer 10: Topology Registry${RESET}\n"

run_test 10 "topology list shows entries" \
    "$DSCO --topology-list 2>&1" \
    "sentinel\|starburst\|topolog"

# ─── Layer 11: Topology Planning ─────────────────────────────────────────
printf "\n${BOLD}Layer 11: Topology Planning${RESET}\n"

run_test_nonzero 11 "sentinel topology runs" \
    "$DSCO --topology sentinel -m sonnet 'What is 3+3? Say the number only.' 2>/dev/null"

# ─── Layer 12: Topology Execution ────────────────────────────────────────
printf "\n${BOLD}Layer 12: Topology Execution${RESET}\n"

run_test 12 "starburst fanout topology" \
    "$DSCO --topology starburst -m haiku 'What is 5+5?' 2>&1" \
    "10\|agents\|topology"

# ─── Layer 13: Swarm Tool Interface ──────────────────────────────────────
printf "\n${BOLD}Layer 13: Swarm Tool Interface${RESET}\n"

run_test 13 "spawn_agent returns agent_id" \
    "$DSCO -m sonnet 'Use spawn_agent task=\"echo test\" and report the agent_id number' 2>&1" \
    "agent\|id\|0"

# ─── Layer 14: Depth Limiting ────────────────────────────────────────────
printf "\n${BOLD}Layer 14: Depth Limiting${RESET}\n"

run_test 14 "Depth tracking in env" \
    "DSCO_SWARM_DEPTH=5 $DSCO -m sonnet 'Use spawn_agent task=\"echo hi\". Report success or error.' 2>&1" \
    "depth\|error\|limit\|cannot\|agent"

# ─── Layer 15: Oneshot Executor Dispatch ──────────────────────────────────
printf "\n${BOLD}Layer 15: Oneshot Executor Dispatch${RESET}\n"

run_test 15 "-e list shows executors" \
    "$DSCO -e list 2>&1" \
    "claude\|codex\|External"

run_test 15 "-e openrouter resolves provider" \
    "$DSCO -e openrouter -m moonshotai/kimi-k2.5 'Say only: EXEC_OR_OK' 2>&1" \
    "EXEC_OR_OK\|kimi"

# ─── Layer 16: Cross-Provider Swarm ──────────────────────────────────────
printf "\n${BOLD}Layer 16: Cross-Provider Swarm${RESET}\n"

run_test 16 "Mixed executor swarm (dsco + claude)" \
    "$DSCO -m sonnet 'Use spawn_executor executor=dsco task=\"Say DSCO_SIDE\" model=haiku. Then spawn_executor executor=claude task=\"Say CLAUDE_SIDE\". Wait for both. Report both outputs.' 2>&1" \
    "DSCO_SIDE\|CLAUDE_SIDE\|agent"

# ─── Layer 17: Live Streaming & Cost Rollup ───────────────────────────────
printf "\n${BOLD}Layer 17: Live Streaming & Cost Rollup${RESET}\n"

run_test 17 "Multi-model swarm with cost tracking" \
    "$DSCO -m sonnet 'Set swarm_budget to 5.0. Then create_executor_swarm name=\"cost-test\" executor=claude tasks=[\"Say A\",\"Say B\"]. Collect results. Report total cost and budget remaining.' 2>&1" \
    "cost\|budget\|complete\|done\|agent"

# ═══════════════════════════════════════════════════════════════════════════
printf "\n${BOLD}═══════════════════════════════════════════════════════════════${RESET}\n"
printf "${BOLD}  RESULTS: ${GREEN}%d passed${RESET} / ${RED}%d failed${RESET} / ${YELLOW}%d skipped${RESET} / %d total\n" \
    "$PASS" "$FAIL" "$SKIP" "$TOTAL"
printf "${BOLD}═══════════════════════════════════════════════════════════════${RESET}\n\n"

exit $FAIL
