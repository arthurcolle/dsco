#ifndef DSCO_ORCHESTRATOR_H
#define DSCO_ORCHESTRATOR_H

#include <stdbool.h>

/*
 * Orchestrator mode: GLM as chat/routing model + specialist worker agents
 * with domain-filtered tool subsets (not all 421 tools at once).
 *
 * Architecture:
 *   User → GLM Orchestrator (2 tools: dispatch_agent, list_domains)
 *             → dispatch_agent(domain, task, model)
 *                 → Worker Agent (domain-filtered toolkit, up to ~30 tools)
 *                     → Returns result text
 *             → Synthesizes response → User
 *
 * Domains: file, git, system, code, web, trading, market, wings, text, general
 * Worker model: kimi-k2.7-code (default), glm (general), or a full model ID
 */

/* Launch orchestrated mode.
 * chat_model:       lightweight routing model (default: z-ai/glm-5.2)
 * worker_model:     execution model (default: kimi-k2.7-code)
 *                   Also accepts env DSCO_WORKER_MODEL.
 * provider_override: NULL = auto-detect from model name */
bool agent_run_orchestrated(const char *api_key,
                             const char *chat_model,
                             const char *worker_model,
                             const char *provider_override);

#endif /* DSCO_ORCHESTRATOR_H */
