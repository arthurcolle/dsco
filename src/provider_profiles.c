#include "provider_profiles.h"

#include <string.h>

#define CAP_OPENAI_COMPAT \
    (PROVIDER_CAP_TOOLS | PROVIDER_CAP_MULTITURN | PROVIDER_CAP_STREAMING | \
     PROVIDER_CAP_JSON | PROVIDER_CAP_HEALTH_CHECK)

#define CAP_OPENAI_COMPAT_VISION \
    (CAP_OPENAI_COMPAT | PROVIDER_CAP_VISION | PROVIDER_CAP_VISION_TOOL_MESSAGES)

#define CAP_ANTHROPIC \
    (PROVIDER_CAP_TOOLS | PROVIDER_CAP_MULTITURN | PROVIDER_CAP_STREAMING | \
     PROVIDER_CAP_VISION | PROVIDER_CAP_VISION_TOOL_MESSAGES | \
     PROVIDER_CAP_REASONING | PROVIDER_CAP_JSON | PROVIDER_CAP_PROMPT_CACHE | \
     PROVIDER_CAP_HEALTH_CHECK)

static const provider_profile_t PROVIDER_PROFILES[] = {
    {
        .name = "anthropic",
        .display_name = "Anthropic",
        .description = "Native Anthropic Messages API and Claude Code OAuth.",
        .api_mode = PROVIDER_API_ANTHROPIC_MESSAGES,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_ANTHROPIC_MESSAGES,
        .base_url = "https://api.anthropic.com",
        .transport_base_url = "https://api.anthropic.com",
        .env_vars = { "ANTHROPIC_API_KEY", "ANTHROPIC_TOKEN",
                      "CLAUDE_CODE_OAUTH_TOKEN", "DSCO_CLAUDE_CODE_OAUTH_TOKEN" },
        .aliases = { "claude", "claude-oauth", "claude-code" },
        .default_model = "claude-sonnet-4-6",
        .default_aux_model = "claude-haiku-4-5-20251001",
        .caps = CAP_ANTHROPIC,
    },
    {
        .name = "openai",
        .display_name = "OpenAI",
        .description = "OpenAI Chat Completions-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.openai.com/v1",
        .transport_base_url = "https://api.openai.com/v1",
        .env_vars = { "OPENAI_API_KEY", "OPENAI_KEY", "CHATGPT_API_KEY" },
        .aliases = { "chatgpt" },
        .default_model = "gpt-4.1",
        .caps = CAP_OPENAI_COMPAT_VISION,
    },
    {
        .name = "openrouter",
        .display_name = "OpenRouter",
        .description = "OpenRouter multi-model OpenAI-compatible gateway.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://openrouter.ai/api/v1",
        .transport_base_url = "https://openrouter.ai/api/v1",
        .env_vars = { "OPENROUTER_API_KEY", "OPEN_ROUTER_API_KEY" },
        .aliases = { "or" },
        .default_model = "z-ai/glm-5.2",
        .caps = CAP_OPENAI_COMPAT_VISION,
    },
    {
        .name = "google",
        .display_name = "Google Gemini",
        .description = "Google AI Studio Gemini OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://generativelanguage.googleapis.com/v1beta",
        .transport_base_url = "https://generativelanguage.googleapis.com/v1beta/openai",
        .env_vars = { "GOOGLE_API_KEY", "GEMINI_API_KEY", "GOOGLE_AI_API_KEY",
                      "GOOGLE_AI_STUDIO_API_KEY", "GOOGLE_VERTEX_API_KEY" },
        .aliases = { "gemini", "google-gemini", "google-ai-studio" },
        .default_model = "gemini-2.5-pro",
        .caps = CAP_OPENAI_COMPAT_VISION,
    },
    {
        .name = "groq",
        .display_name = "Groq",
        .description = "Groq fast OpenAI-compatible inference.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.groq.com/openai/v1",
        .transport_base_url = "https://api.groq.com/openai/v1",
        .env_vars = { "GROQ_API_KEY" },
        .default_model = "llama-3.3-70b-versatile",
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "deepseek",
        .display_name = "DeepSeek",
        .description = "DeepSeek OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.deepseek.com/v1",
        .transport_base_url = "https://api.deepseek.com/v1",
        .env_vars = { "DEEPSEEK_API_KEY" },
        .aliases = { "deepseek-chat" },
        .default_model = "deepseek-chat",
        .caps = CAP_OPENAI_COMPAT | PROVIDER_CAP_REASONING,
    },
    {
        .name = "mistral",
        .display_name = "Mistral AI",
        .description = "Mistral OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.mistral.ai/v1",
        .transport_base_url = "https://api.mistral.ai/v1",
        .env_vars = { "MISTRAL_API_KEY" },
        .default_model = "mistral-large-latest",
        .caps = CAP_OPENAI_COMPAT_VISION,
    },
    {
        .name = "xai",
        .display_name = "xAI",
        .description = "xAI Grok OpenAI-compatible endpoint in DSCO.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.x.ai/v1",
        .transport_base_url = "https://api.x.ai/v1",
        .env_vars = { "XAI_API_KEY", "GROK_API_KEY", "X_AI_API_KEY" },
        .aliases = { "grok", "x-ai", "x.ai" },
        .default_model = "grok-4-fast",
        .caps = CAP_OPENAI_COMPAT_VISION | PROVIDER_CAP_REASONING,
    },
    {
        .name = "together",
        .display_name = "Together AI",
        .description = "Together OpenAI-compatible inference.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.together.xyz/v1",
        .transport_base_url = "https://api.together.xyz/v1",
        .env_vars = { "TOGETHER_API_KEY", "TOGETHER_TOKEN" },
        .default_model = "meta-llama/Llama-4-Maverick-17B-128E-Instruct-FP8",
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "perplexity",
        .display_name = "Perplexity",
        .description = "Perplexity Sonar API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.perplexity.ai",
        .transport_base_url = "https://api.perplexity.ai",
        .env_vars = { "PERPLEXITY_API_KEY" },
        .aliases = { "pplx" },
        .default_model = "sonar-pro",
        .caps = PROVIDER_CAP_MULTITURN | PROVIDER_CAP_STREAMING | PROVIDER_CAP_HEALTH_CHECK,
    },
    {
        .name = "cerebras",
        .display_name = "Cerebras",
        .description = "Cerebras fast OpenAI-compatible inference.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.cerebras.ai/v1",
        .transport_base_url = "https://api.cerebras.ai/v1",
        .env_vars = { "CEREBRAS_API_KEY" },
        .default_model = "qwen-3-235b-a22b-instruct-2507",
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "cohere",
        .display_name = "Cohere",
        .description = "Cohere OpenAI-compatible endpoint.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.cohere.com/v2",
        .transport_base_url = "https://api.cohere.com/v2",
        .env_vars = { "COHERE_API_KEY" },
        .default_model = "command-a-03-2025",
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "moonshot",
        .display_name = "Moonshot Kimi",
        .description = "Moonshot/Kimi OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.moonshot.ai/v1",
        .transport_base_url = "https://api.moonshot.ai/v1",
        .env_vars = { "MOONSHOT_API_KEY", "KIMI_API_KEY", "KIMI_CODING_API_KEY" },
        .aliases = { "kimi", "kimi-coding", "kimi-for-coding" },
        .default_model = "kimi-k2.7-code",
        .caps = CAP_OPENAI_COMPAT_VISION | PROVIDER_CAP_REASONING,
    },
    {
        .name = "alibaba",
        .display_name = "Alibaba DashScope",
        .description = "DashScope OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
        .transport_base_url = "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
        .env_vars = { "DASHSCOPE_API_KEY" },
        .aliases = { "dashscope", "alibaba-cloud", "qwen-dashscope" },
        .default_model = "qwen3-coder-plus",
        .caps = CAP_OPENAI_COMPAT_VISION,
    },
    {
        .name = "alibaba-coding-plan",
        .display_name = "Alibaba Coding Plan",
        .description = "DashScope coding plan OpenAI-compatible endpoint.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://coding-intl.dashscope.aliyuncs.com/v1",
        .transport_base_url = "https://coding-intl.dashscope.aliyuncs.com/v1",
        .env_vars = { "ALIBABA_CODING_PLAN_API_KEY", "DASHSCOPE_API_KEY" },
        .aliases = { "alibaba_coding", "alibaba-coding", "dashscope-coding" },
        .default_model = "qwen3-coder-plus",
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "arcee",
        .display_name = "Arcee AI",
        .description = "Arcee OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.arcee.ai/api/v1",
        .transport_base_url = "https://api.arcee.ai/api/v1",
        .env_vars = { "ARCEEAI_API_KEY" },
        .aliases = { "arcee-ai", "arceeai" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "azure-foundry",
        .display_name = "Azure AI Foundry",
        .description = "Azure AI Foundry OpenAI-compatible deployments.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "",
        .transport_base_url = "",
        .env_vars = { "AZURE_FOUNDRY_API_KEY" },
        .aliases = { "azure", "azure-ai-foundry", "azure-ai" },
        .caps = CAP_OPENAI_COMPAT_VISION,
    },
    {
        .name = "gmi",
        .display_name = "GMI Cloud",
        .description = "GMI Cloud OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.gmi-serving.com/v1",
        .transport_base_url = "https://api.gmi-serving.com/v1",
        .env_vars = { "GMI_API_KEY" },
        .aliases = { "gmi-cloud", "gmicloud" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "huggingface",
        .display_name = "Hugging Face Router",
        .description = "Hugging Face OpenAI-compatible router.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://router.huggingface.co/v1",
        .transport_base_url = "https://router.huggingface.co/v1",
        .env_vars = { "HF_TOKEN", "HUGGINGFACE_API_KEY" },
        .aliases = { "hf", "hugging-face", "huggingface-hub" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "kilocode",
        .display_name = "KiloCode",
        .description = "KiloCode gateway OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.kilo.ai/api/gateway",
        .transport_base_url = "https://api.kilo.ai/api/gateway",
        .env_vars = { "KILOCODE_API_KEY" },
        .aliases = { "kilo-code", "kilo", "kilo-gateway" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "novita",
        .display_name = "Novita AI",
        .description = "Novita OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.novita.ai/openai/v1",
        .transport_base_url = "https://api.novita.ai/openai/v1",
        .env_vars = { "NOVITA_API_KEY" },
        .aliases = { "novita-ai", "novitaai" },
        .caps = CAP_OPENAI_COMPAT_VISION,
    },
    {
        .name = "nvidia",
        .display_name = "NVIDIA NIM",
        .description = "NVIDIA NIM OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://integrate.api.nvidia.com/v1",
        .transport_base_url = "https://integrate.api.nvidia.com/v1",
        .env_vars = { "NVIDIA_API_KEY" },
        .aliases = { "nvidia-nim" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "ollama-cloud",
        .display_name = "Ollama Cloud",
        .description = "Ollama Cloud OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://ollama.com/v1",
        .transport_base_url = "https://ollama.com/v1",
        .env_vars = { "OLLAMA_API_KEY" },
        .aliases = { "ollama_cloud" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "opencode-zen",
        .display_name = "OpenCode Zen",
        .description = "OpenCode Zen OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://opencode.ai/zen/v1",
        .transport_base_url = "https://opencode.ai/zen/v1",
        .env_vars = { "OPENCODE_ZEN_API_KEY" },
        .aliases = { "opencode", "opencode_zen", "zen" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "opencode-go",
        .display_name = "OpenCode Go",
        .description = "OpenCode Go OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://opencode.ai/zen/go/v1",
        .transport_base_url = "https://opencode.ai/zen/go/v1",
        .env_vars = { "OPENCODE_GO_API_KEY" },
        .aliases = { "opencode_go" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "stepfun",
        .display_name = "StepFun",
        .description = "StepFun coding plan OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.stepfun.ai/step_plan/v1",
        .transport_base_url = "https://api.stepfun.ai/step_plan/v1",
        .env_vars = { "STEPFUN_API_KEY" },
        .aliases = { "step", "stepfun-coding-plan" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "xiaomi",
        .display_name = "Xiaomi MiMo",
        .description = "Xiaomi MiMo OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.xiaomimimo.com/v1",
        .transport_base_url = "https://api.xiaomimimo.com/v1",
        .env_vars = { "XIAOMI_API_KEY" },
        .aliases = { "mimo", "xiaomi-mimo" },
        .caps = CAP_OPENAI_COMPAT | PROVIDER_CAP_VISION,
    },
    {
        .name = "zai",
        .display_name = "Z.AI GLM",
        .description = "Z.AI/GLM OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://api.z.ai/api/paas/v4",
        .transport_base_url = "https://api.z.ai/api/paas/v4",
        .env_vars = { "GLM_API_KEY", "ZAI_API_KEY", "Z_AI_API_KEY" },
        .aliases = { "glm", "z-ai", "z.ai", "zhipu" },
        .default_model = "glm-5.2",
        .caps = CAP_OPENAI_COMPAT_VISION,
    },
    {
        .name = "qwen-oauth",
        .display_name = "Qwen OAuth",
        .description = "Qwen portal OpenAI-compatible endpoint with OAuth-style auth.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_OAUTH_EXTERNAL,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://portal.qwen.ai/v1",
        .transport_base_url = "https://portal.qwen.ai/v1",
        .env_vars = { "QWEN_API_KEY" },
        .aliases = { "qwen", "qwen-portal", "qwen-cli" },
        .default_model = "qwen3-coder-plus",
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "nous",
        .display_name = "Nous Research",
        .description = "Nous Research inference API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_OAUTH_DEVICE_CODE,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "https://inference.nousresearch.com/v1",
        .transport_base_url = "https://inference.nousresearch.com/v1",
        .env_vars = { "NOUS_API_KEY" },
        .aliases = { "nous-portal", "nousresearch" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "minimax",
        .display_name = "MiniMax",
        .description = "MiniMax Anthropic-compatible endpoint.",
        .api_mode = PROVIDER_API_ANTHROPIC_MESSAGES,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_NONE,
        .base_url = "https://api.minimax.io/anthropic",
        .env_vars = { "MINIMAX_API_KEY" },
        .default_model = "MiniMax-M2",
        .caps = CAP_ANTHROPIC & ~PROVIDER_CAP_PROMPT_CACHE,
    },
    {
        .name = "minimax-cn",
        .display_name = "MiniMax China",
        .description = "MiniMax China Anthropic-compatible endpoint.",
        .api_mode = PROVIDER_API_ANTHROPIC_MESSAGES,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_NONE,
        .base_url = "https://api.minimaxi.com/anthropic",
        .env_vars = { "MINIMAX_CN_API_KEY" },
        .default_model = "MiniMax-M2",
        .caps = CAP_ANTHROPIC & ~PROVIDER_CAP_PROMPT_CACHE,
    },
    {
        .name = "minimax-oauth",
        .display_name = "MiniMax OAuth",
        .description = "MiniMax external OAuth provider.",
        .api_mode = PROVIDER_API_ANTHROPIC_MESSAGES,
        .auth_type = PROVIDER_AUTH_OAUTH_EXTERNAL,
        .transport = PROVIDER_TRANSPORT_NONE,
        .base_url = "",
        .caps = CAP_ANTHROPIC & ~PROVIDER_CAP_PROMPT_CACHE,
    },
    {
        .name = "bedrock",
        .display_name = "AWS Bedrock",
        .description = "AWS Bedrock Converse API.",
        .api_mode = PROVIDER_API_BEDROCK_CONVERSE,
        .auth_type = PROVIDER_AUTH_AWS_SDK,
        .transport = PROVIDER_TRANSPORT_BEDROCK_CONVERSE,
        .base_url = "https://bedrock-runtime.us-east-1.amazonaws.com",
        .aliases = { "aws", "aws-bedrock", "amazon-bedrock", "amazon" },
        .caps = PROVIDER_CAP_TOOLS | PROVIDER_CAP_MULTITURN |
                PROVIDER_CAP_STREAMING | PROVIDER_CAP_VISION | PROVIDER_CAP_JSON,
    },
    {
        .name = "copilot",
        .display_name = "GitHub Copilot",
        .description = "GitHub Copilot / GitHub Models provider.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_COPILOT,
        .transport = PROVIDER_TRANSPORT_NONE,
        .base_url = "https://api.githubcopilot.com",
        .env_vars = { "COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN" },
        .aliases = { "github-copilot", "github-models", "github-model", "github" },
        .caps = CAP_OPENAI_COMPAT_VISION | PROVIDER_CAP_REASONING,
    },
    {
        .name = "copilot-acp",
        .display_name = "GitHub Copilot ACP",
        .description = "Copilot Agent Client Protocol provider.",
        .api_mode = PROVIDER_API_ACP,
        .auth_type = PROVIDER_AUTH_EXTERNAL_PROCESS,
        .transport = PROVIDER_TRANSPORT_ACP,
        .base_url = "acp://copilot",
        .aliases = { "github-copilot-acp", "copilot-acp-agent" },
        .caps = PROVIDER_CAP_TOOLS | PROVIDER_CAP_MULTITURN | PROVIDER_CAP_STREAMING,
    },
    {
        .name = "openai-codex",
        .display_name = "OpenAI Codex",
        .description = "OpenAI Codex Responses/OAuth path.",
        .api_mode = PROVIDER_API_CODEX_RESPONSES,
        .auth_type = PROVIDER_AUTH_OAUTH_EXTERNAL,
        .transport = PROVIDER_TRANSPORT_CODEX_RESPONSES,
        .base_url = "https://chatgpt.com/backend-api/codex",
        .aliases = { "codex", "openai_codex" },
        .default_model = "gpt-5.3-codex-spark",
        .caps = PROVIDER_CAP_TOOLS | PROVIDER_CAP_MULTITURN |
                PROVIDER_CAP_STREAMING | PROVIDER_CAP_REASONING | PROVIDER_CAP_JSON,
    },
    {
        .name = "custom",
        .display_name = "Custom",
        .description = "User-configured OpenAI-compatible provider.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_API_KEY,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .env_vars = { "CUSTOM_API_KEY" },
        .aliases = { "custom-openai" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "ollama",
        .display_name = "Ollama Local",
        .description = "Local Ollama OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_NONE,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "http://localhost:11434/v1",
        .transport_base_url = "http://localhost:11434/v1",
        .env_vars = { "OLLAMA_API_KEY" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "lmstudio",
        .display_name = "LM Studio",
        .description = "Local LM Studio OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_NONE,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "http://localhost:1234/v1",
        .transport_base_url = "http://localhost:1234/v1",
        .env_vars = { "LMSTUDIO_API_KEY" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "mlx",
        .display_name = "MLX Local",
        .description = "Local MLX OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_NONE,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "http://localhost:8181/v1",
        .transport_base_url = "http://localhost:8181/v1",
        .env_vars = { "MLX_API_KEY" },
        .caps = CAP_OPENAI_COMPAT,
    },
    {
        .name = "local",
        .display_name = "Local",
        .description = "Generic local OpenAI-compatible API.",
        .api_mode = PROVIDER_API_CHAT_COMPLETIONS,
        .auth_type = PROVIDER_AUTH_NONE,
        .transport = PROVIDER_TRANSPORT_OPENAI_CHAT,
        .base_url = "http://localhost:8181/v1",
        .transport_base_url = "http://localhost:8181/v1",
        .env_vars = { "LOCAL_API_KEY" },
        .caps = CAP_OPENAI_COMPAT,
    },
};

size_t provider_profile_count(void) {
    return sizeof(PROVIDER_PROFILES) / sizeof(PROVIDER_PROFILES[0]);
}

const provider_profile_t *provider_profile_at(size_t index) {
    if (index >= provider_profile_count()) return NULL;
    return &PROVIDER_PROFILES[index];
}

bool provider_profile_has_alias(const provider_profile_t *profile,
                                const char *alias) {
    if (!profile || !alias || !alias[0]) return false;
    for (int i = 0; i < PROVIDER_PROFILE_MAX_ALIASES && profile->aliases[i]; i++) {
        if (strcmp(profile->aliases[i], alias) == 0) return true;
    }
    return false;
}

bool provider_profile_has_env_var(const provider_profile_t *profile,
                                  const char *env_var) {
    if (!profile || !env_var || !env_var[0]) return false;
    for (int i = 0; i < PROVIDER_PROFILE_MAX_ENV_VARS && profile->env_vars[i]; i++) {
        if (strcmp(profile->env_vars[i], env_var) == 0) return true;
    }
    return false;
}

const provider_profile_t *provider_profile_find(const char *name_or_alias) {
    if (!name_or_alias || !name_or_alias[0]) return NULL;
    for (size_t i = 0; i < provider_profile_count(); i++) {
        const provider_profile_t *profile = &PROVIDER_PROFILES[i];
        if (strcmp(profile->name, name_or_alias) == 0) return profile;
        if (provider_profile_has_alias(profile, name_or_alias)) return profile;
    }
    return NULL;
}

const char *provider_profile_canonical_name(const char *name_or_alias) {
    const provider_profile_t *profile = provider_profile_find(name_or_alias);
    return profile ? profile->name : name_or_alias;
}

const char *provider_api_mode_name(provider_api_mode_t mode) {
    switch (mode) {
        case PROVIDER_API_CHAT_COMPLETIONS: return "chat_completions";
        case PROVIDER_API_ANTHROPIC_MESSAGES: return "anthropic_messages";
        case PROVIDER_API_CODEX_RESPONSES: return "codex_responses";
        case PROVIDER_API_BEDROCK_CONVERSE: return "bedrock_converse";
        case PROVIDER_API_ACP: return "acp";
        case PROVIDER_API_EXTERNAL_PROCESS: return "external_process";
    }
    return "unknown";
}

const char *provider_auth_type_name(provider_auth_type_t auth_type) {
    switch (auth_type) {
        case PROVIDER_AUTH_API_KEY: return "api_key";
        case PROVIDER_AUTH_OAUTH_DEVICE_CODE: return "oauth_device_code";
        case PROVIDER_AUTH_OAUTH_EXTERNAL: return "oauth_external";
        case PROVIDER_AUTH_COPILOT: return "copilot";
        case PROVIDER_AUTH_AWS_SDK: return "aws_sdk";
        case PROVIDER_AUTH_EXTERNAL_PROCESS: return "external_process";
        case PROVIDER_AUTH_NONE: return "none";
    }
    return "unknown";
}

const char *provider_transport_kind_name(provider_transport_kind_t transport) {
    switch (transport) {
        case PROVIDER_TRANSPORT_NONE: return "none";
        case PROVIDER_TRANSPORT_OPENAI_CHAT: return "openai_chat";
        case PROVIDER_TRANSPORT_ANTHROPIC_MESSAGES: return "anthropic_messages";
        case PROVIDER_TRANSPORT_CODEX_RESPONSES: return "codex_responses";
        case PROVIDER_TRANSPORT_BEDROCK_CONVERSE: return "bedrock_converse";
        case PROVIDER_TRANSPORT_ACP: return "acp";
        case PROVIDER_TRANSPORT_EXTERNAL_PROCESS: return "external_process";
    }
    return "unknown";
}

const char *provider_profile_primary_env_var(const provider_profile_t *profile) {
    if (!profile) return NULL;
    return profile->env_vars[0];
}

bool provider_profile_transport_supported(const provider_profile_t *profile) {
    if (!profile) return false;
    return profile->transport == PROVIDER_TRANSPORT_OPENAI_CHAT ||
           profile->transport == PROVIDER_TRANSPORT_ANTHROPIC_MESSAGES;
}
