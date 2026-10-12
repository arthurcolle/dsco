#include "native_session_details.h"
#include <stdio.h>
#include <math.h>

void native_session_details_format(const native_session_details_t *s, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!s) return;
    char reported[64], estimated[64], budget[64];
    if (s->reported_samples > 0) snprintf(reported, sizeof(reported), "$%.4f", s->reported_usd);
    else snprintf(reported, sizeof(reported), "unknown");
    if (s->estimated_samples > 0) snprintf(estimated, sizeof(estimated), "$%.4f", s->estimated_usd);
    else snprintf(estimated, sizeof(estimated), "unknown");
    if (s->budget_usd > 0) snprintf(budget, sizeof(budget), "$%.2f", s->budget_usd);
    else snprintf(budget, sizeof(budget), "not set");
    double elapsed = isfinite(s->elapsed_s) && s->elapsed_s > 0 ? s->elapsed_s : 0;
    snprintf(out, cap,
        "Model: %.127s\nSlot: %.63s\nState: %.32s\nTurn: %d\nElapsed: %.0fs\n"
        "\nUSAGE\nContext: %.1f%%\nInput tokens: %d\nOutput tokens: %d\n"
        "Tools used: %d\nTools running: %d\nQueue: %d / %d\n"
        "Workers: %d active / %d done / %d errors\n"
        "\nCOST OBSERVATIONS\nTracked value: $%.4f%s\nBudget: %s\n"
        "Provider reported: %s\nReference estimate: %s\nUnpriced responses: %d\n"
        "Reported and estimated values are not additive.\n"
        "Subscription usage is not a cash charge.\n"
        "\nCONFIGURATION (at last prompt)\n%.2047s",
        s->model && *s->model ? s->model : "unknown",
        s->slot && *s->slot ? s->slot : "default",
        s->state ? s->state : "unknown", s->turn, floor(elapsed),
        s->context_percent, s->input_tokens, s->output_tokens,
        s->tools_used, s->running_tools, s->queue_depth, s->queue_capacity,
        s->swarm_active, s->swarm_done, s->swarm_errors,
        s->cost_usd, s->unpriced > 0 ? " + unknown" : "", budget,
        reported, estimated, s->unpriced,
        s->configuration && *s->configuration ? s->configuration : "Not available");
}
