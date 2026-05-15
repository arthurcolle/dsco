#ifndef DSCO_AUDIT_LOG_H
#define DSCO_AUDIT_LOG_H

/* ── Tamper-Evident Audit Log ──────────────────────────────────────────────
 *
 * Append-only log where each entry is HMAC-SHA-256 chained to the previous.
 * An attacker who modifies or deletes an entry breaks the chain — detectable
 * on next open via audit_log_verify().
 *
 * Wire format per entry (binary):
 *   [uint32_t entry_len]          — total byte length of this entry
 *   [uint64_t seq]                — monotonic sequence number (host byte order)
 *   [int64_t  ts]                 — unix epoch (host byte order)
 *   [uint16_t tag_len]            — length of tag string
 *   [uint32_t msg_len]            — length of message
 *   [uint8_t  hmac[32]]           — HMAC-SHA-256(key, prev_hmac || seq || ts || tag || msg)
 *   [char     tag[tag_len]]       — category / source tag
 *   [char     msg[msg_len]]       — human-readable message
 *
 * Requires HAVE_LIBSODIUM (falls back to no-op stubs when absent).
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct audit_log audit_log_t;

/* Open (or create) the log at path, keyed by a 32-byte HMAC key.
 * Pass key=NULL to auto-derive from a process-stable secret in sealed_store. */
audit_log_t *audit_log_open(const char *path, const uint8_t key[32]);

/* Append an entry.  tag is a short category string ("tamper", "llm", "mesh").
 * Returns sequence number of new entry, or -1 on error. */
int64_t audit_log_write(audit_log_t *al, const char *tag, const char *msg);

/* Verify the entire chain.  Returns true if intact, false if broken.
 * Sets *bad_seq to the first broken entry's sequence number (or -1). */
bool audit_log_verify(audit_log_t *al, int64_t *bad_seq);

/* Iterate entries from seq_from to seq_to (inclusive, -1 = end).
 * Calls cb(seq, ts, tag, msg, ctx) for each.  Stops if cb returns false. */
typedef bool (*audit_log_iter_fn)(int64_t seq, int64_t ts,
                                  const char *tag, const char *msg, void *ctx);
void audit_log_iter(audit_log_t *al, int64_t seq_from, int64_t seq_to,
                    audit_log_iter_fn cb, void *ctx);

void audit_log_close(audit_log_t *al);

/* Global convenience instance — init'd by audit_log_global_init(). */
void    audit_log_global_init(const char *path);
int64_t audit_log(const char *tag, const char *msg);

#endif /* DSCO_AUDIT_LOG_H */
