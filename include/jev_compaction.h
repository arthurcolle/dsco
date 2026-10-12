#ifndef DSCO_JEV_COMPACTION_H
#define DSCO_JEV_COMPACTION_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define JEV_COMPACTION_MAX 8
#define JEV_COMPACTION_V1_MODE "shadow-v1"
#define JEV_COMPACTION_V1_MODEL "jev-1.13.0"
/* Legacy shadow retains its rolling alias. New opt-ins pin the contract/model.
 * Unknown versions fail closed; there is no semantic/apply mode. */
static inline bool jev_compaction_mode_enabled(const char *mode) {
    return mode && (!strcmp(mode, "shadow") || !strcmp(mode, JEV_COMPACTION_V1_MODE));
}
/* Structural metadata only. No conversation text, tool names, IDs or paths. */
typedef struct {
    int age_messages;
    size_t archive_bytes;
    int calls;
    int results;
    bool archived;
} jev_compaction_candidate_t;
/* Default-off, at most one governed network attempt per process. Never mutates
 * conversation or archive policy. Always returns a small JSON status object. */
void jev_compaction_shadow(const jev_compaction_candidate_t *items, size_t count,
                           char *out, size_t len);
#endif
