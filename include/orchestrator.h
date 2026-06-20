#ifndef DSCO_ORCHESTRATOR_H
#define DSCO_ORCHESTRATOR_H

/*
 * Orchestrator mode: Haiku as chat/routing model + specialist worker agents
 * with domain-filtered tool subsets (not all 421 tools at once).
 *
 * Architecture:
 *   User → Haiku Orchestrator (2 tools: dispatch_agent, list_domains)
 *             → dispatch_agent(domain, task, model)
 *                 → Worker Agent (domain-filtered toolkit, up to ~30 tools)
 *                     → Returns result text
 *             → Synthesizes response → User
 *
 * Domains: file, git, system, code, web, trading, market, wings, text, general
 * Worker model: sonnet (default), opus (complex), haiku (fast/cheap)
 */

/* Launch orchestrated mode.
 * chat_model:       lightweight routing model (default: claude-haiku-4-5-20251001)
 * worker_model:     execution model (default: claude-sonnet-4-6)
 *                   Also accepts env DSCO_WORKER_MODEL.
 * provider_override: NULL = auto-detect from model name */
void agent_run_orchestrated(const char *api_key,
                             const char *chat_model,
                             const char *worker_model,
                             const char *provider_override);

#endif /* DSCO_ORCHESTRATOR_H */
