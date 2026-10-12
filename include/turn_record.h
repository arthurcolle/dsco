#ifndef DSCO_TURN_RECORD_H
#define DSCO_TURN_RECORD_H

#include "llm.h" /* Reuse the existing usage_t contract. */
#include <stdbool.h>
#include <stddef.h>
#ifdef DSCO_INTERNAL_TESTS
#include <sys/types.h>
#endif

/* Borrowed strings/usage need only remain valid for encode/append.
 * timestamp intentionally retains the legacy signed-int wire representation. */
typedef struct {
    int timestamp;
    int turn;
    const char *model;            /* NULL encodes as an empty string. */
    const usage_t *usage;         /* NULL omits all four usage fields. */
    double cost;                 /* Finite; legacy fixed six-decimal encoding. */
    const char *stop_reason;      /* NULL omits stop; empty string is retained. */
} turn_record_t;

/* Caller frees the returned JSONL bytes. NULL sets errno; length is then zero. */
char *turn_record_encode(const turn_record_t *record, size_t *length);
/* Writes the entire record, then fsyncs. Retries EINTR and short writes.
 * On failure errno is preserved; bytes may already have reached the descriptor.
 * Do not blindly retry the record or continue appending after a failed write. */
bool turn_record_append(int fd, const turn_record_t *record);

#ifdef DSCO_INTERNAL_TESTS
/* Per-call fault injection: no mutable global hooks in production. */
bool turn_record_append_with_io(int fd, const turn_record_t *record,
                               ssize_t (*write_fn)(int, const void *, size_t),
                               int (*sync_fn)(int));
#endif
#endif
