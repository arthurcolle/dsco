#include "critical_stamp.h"
#include <string.h>

/* Copy bounded, single-line fields. A provider-supplied control sequence must
 * never be able to change the terminal or native transcript presentation. */
static void field(char out[241], const char *value) {
    if (!value || !*value) value = "unknown";
    size_t i = 0;
    for (; value[i] && i < 240; i++) {
        unsigned char ch = (unsigned char)value[i];
        out[i] = (ch < 32 || ch == 127) ? ' ' : (char)ch;
    }
    out[i] = '\0';
    if (value[i] && i > 3) memcpy(out + i - 3, "...", 3);
}

void critical_stamp_format(char *out, size_t cap, const char *reason,
                           const char *impact, const char *next_step) {
    if (!out || !cap) return;
    char err[241], effect[241], next[241];
    field(err, reason);
    field(effect, impact);
    field(next, next_step);
    snprintf(out, cap,
        "[!! CRITICAL ERROR !!] Task stopped before verified completion.\n"
        "Error: %s\nImpact: %s\nNext: %s\n[!! END CRITICAL ERROR !!]",
        err, effect, next);
}

void critical_stamp_print(FILE *out, const char *reason, const char *impact,
                          const char *next_step) {
    if (!out) return;
    char text[1024];
    critical_stamp_format(text, sizeof(text), reason, impact, next_step);
    fprintf(out, "\n%s\n", text);
    fflush(out);
}
