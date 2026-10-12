#include "native_startup_card.h"
#include <stdio.h>
#include <string.h>

/* Untrusted branch names should never introduce synthetic transcript rows or
 * terminal control sequences. Keep the original branch in git; only the
 * bounded presentation copy is sanitized. */
static void field(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!src) src = "";
    for (; *src && n + 1 < cap; ++src) {
        unsigned char ch = (unsigned char)*src;
        dst[n++] = ch < 32 || ch == 127 ? ' ' : (char)ch;
    }
    dst[n] = '\0';
}

bool native_startup_card_format(char *out, size_t cap, int active, int total,
                                const char *trust, const char *branch,
                                double today, double session_cap, double daily_cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    char safe_trust[48], safe_branch[128], budget[144];
    field(safe_trust, sizeof(safe_trust), trust && *trust ? trust : "unknown");
    field(safe_branch, sizeof(safe_branch), branch);
    if (active < 0) active = 0;
    if (total < 0) total = 0;
    if (active > total) active = total;
    int n = snprintf(budget, sizeof(budget), "$%.2f today", today);
    if (n < 0 || (size_t)n >= sizeof(budget)) return false;
    if (session_cap > 0) {
        n += snprintf(budget + n, sizeof(budget) - (size_t)n, " / $%.2f session", session_cap);
        if (n < 0 || (size_t)n >= sizeof(budget)) return false;
    }
    if (daily_cap > 0) {
        n += snprintf(budget + n, sizeof(budget) - (size_t)n, " / $%.2f daily", daily_cap);
        if (n < 0 || (size_t)n >= sizeof(budget)) return false;
    }
    n = snprintf(out, cap, "Features  %d/%d\nTrust  %s%s%s\nBudget  %s",
                 active, total, safe_trust,
                 safe_branch[0] ? "\nBranch  " : "", safe_branch, budget);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return false; }
    return true;
}
