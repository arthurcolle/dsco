#ifndef DSCO_NATIVE_STARTUP_CARD_H
#define DSCO_NATIVE_STARTUP_CARD_H
#include <stddef.h>
#include <stdbool.h>
/* Semantic transcript content for the pixel-native session card. No box
 * characters, ANSI escapes or terminal-width assumptions. The legacy
 * terminal renderer remains responsible for its own fallback presentation. */
bool native_startup_card_format(char *out, size_t cap, int active, int total,
                                const char *trust, const char *branch,
                                double today, double session_cap, double daily_cap);
#endif
