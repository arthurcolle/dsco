#ifndef DSCO_IMPROVEMENT_SYNC_H
#define DSCO_IMPROVEMENT_SYNC_H

/* Content-addressed DSCO improvement exchange.
 *
 * The private DHT is used only to locate providers for a SHA-256 bundle key.
 * Immutable bundle bytes travel over the authenticated, encrypted mesh. Every
 * bundle carries an Ed25519 signature and is hash-verified before persistence.
 * Unknown signers are quarantined; nothing received here is auto-applied.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMPROVEMENT_HASH_HEX_LEN 64
#define IMPROVEMENT_SIGNER_HEX_LEN 64
#define IMPROVEMENT_MAX_BUNDLE_BYTES (32u * 1024u * 1024u)

typedef struct improvement_sync improvement_sync_t;

typedef struct {
    char hash[IMPROVEMENT_HASH_HEX_LEN + 1];
    char signer[IMPROVEMENT_SIGNER_HEX_LEN + 1];
    char name[129];
    char base_version[65];
    char target_version[65];
    char description[513];
    char kind[17];
    char status[17];              /* object, staged, or quarantine */
    uint64_t created_unix;
    uint64_t payload_bytes;
    bool signature_valid;
    bool trusted;
} improvement_bundle_info_t;

/* A context owns one bundle store and may be attached to one mesh node. The
 * root defaults to $DSCO_IMPROVEMENT_ROOT or ~/.dsco/improvements. */
improvement_sync_t *improvement_sync_create(void *mesh_node, const char *root_override);
void improvement_sync_destroy(improvement_sync_t *sync);

bool improvement_sync_publish(improvement_sync_t *sync,
                              const char *payload_path,
                              const char *kind,
                              const char *name,
                              const char *base_version,
                              const char *target_version,
                              const char *description,
                              improvement_bundle_info_t *out,
                              char *err, size_t err_len);

bool improvement_sync_fetch(improvement_sync_t *sync, const char *hash_hex,
                            int timeout_seconds, improvement_bundle_info_t *out,
                            char *err, size_t err_len);
bool improvement_sync_inspect(improvement_sync_t *sync, const char *hash_hex,
                              improvement_bundle_info_t *out,
                              char *err, size_t err_len);
bool improvement_sync_trust_signer(improvement_sync_t *sync, const char *signer_hex,
                                   char *err, size_t err_len);
bool improvement_sync_promote(improvement_sync_t *sync, const char *hash_hex,
                              improvement_bundle_info_t *out,
                              char *err, size_t err_len);
bool improvement_sync_materialize(improvement_sync_t *sync, const char *hash_hex,
                                  const char *output_path,
                                  char *err, size_t err_len);

/* JSON catalog/status surfaces used by the read-only tool. */
bool improvement_sync_status_json(improvement_sync_t *sync, char *out, size_t out_len);
bool improvement_sync_list_json(improvement_sync_t *sync, int limit,
                                char *out, size_t out_len);

/* Re-advertise every locally publishable object into the DHT and mesh. */
void improvement_sync_announce_all(improvement_sync_t *sync);

/* Process-global lifecycle, wired to the normal DSCO mesh lifecycle. */
bool improvement_sync_global_init(void *mesh_node);
/* Stop background announcements and detach outbound mesh use while keeping the
 * receive context alive until mesh_node_destroy() drains connection threads. */
void improvement_sync_global_stop_network(void);
void improvement_sync_global_shutdown(void);
improvement_sync_t *improvement_sync_global(void);

/* Agent tool surfaces. Mutating/network operations and read-only catalog
 * operations are separate so the capability gate can classify them exactly. */
bool tool_improvement_sync(const char *input_json, char *result, size_t result_len);
bool tool_improvement_catalog(const char *input_json, char *result, size_t result_len);

#endif /* DSCO_IMPROVEMENT_SYNC_H */
