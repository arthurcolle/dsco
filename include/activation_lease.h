#ifndef DSCO_ACTIVATION_LEASE_H
#define DSCO_ACTIVATION_LEASE_H

/* activation_lease.h — local activation lease contract + file helpers.
 *
 * This module is intentionally small and side-effect bounded. It does not decide
 * entitlement policy or contact a network service; it only defines the persisted
 * lease shape and helpers for local, atomic lease-file I/O.
 *
 * Default path: ~/.dsco/activation/lease.json
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSCO_ACTIVATION_LEASE_SCHEMA_VERSION 1
#define DSCO_ACTIVATION_LEASE_ID_MAX 64
#define DSCO_ACTIVATION_SUBJECT_MAX 128
#define DSCO_ACTIVATION_PLAN_MAX 64
#define DSCO_ACTIVATION_ISSUER_MAX 128
#define DSCO_ACTIVATION_SCOPE_MAX 512
#define DSCO_ACTIVATION_SIG_MAX 256
#define DSCO_ACTIVATION_PATH_MAX 1024

#define DSCO_ACTIVATION_LEASE_SCHEMA_ID \
    "https://distributed.systems/schemas/activation_lease.schema.json"

typedef struct {
    int schema_version;
    char lease_id[DSCO_ACTIVATION_LEASE_ID_MAX];
    char subject[DSCO_ACTIVATION_SUBJECT_MAX];
    char plan[DSCO_ACTIVATION_PLAN_MAX];
    char issuer[DSCO_ACTIVATION_ISSUER_MAX];
    char scopes[DSCO_ACTIVATION_SCOPE_MAX]; /* comma-separated local capability scopes */
    int64_t issued_at;                      /* unix seconds */
    int64_t expires_at;                     /* unix seconds; 0 = non-expiring local/dev lease */
    char signature[DSCO_ACTIVATION_SIG_MAX]; /* detached signature placeholder */
} activation_lease_t;

typedef enum {
    ACTIVATION_LEASE_OK = 0,
    ACTIVATION_LEASE_ERR_INVALID = -1,
    ACTIVATION_LEASE_ERR_IO = -2,
    ACTIVATION_LEASE_ERR_PARSE = -3,
    ACTIVATION_LEASE_ERR_EXPIRED = -4,
    ACTIVATION_LEASE_ERR_TRUNCATED = -5,
} activation_lease_status_t;

/* Return the default on-disk lease path. Honors DSCO_ACTIVATION_LEASE_PATH when
 * set; otherwise writes ~/.dsco/activation/lease.json. Returns false if the
 * resolved path does not fit in out. */
bool activation_lease_default_path(char *out, size_t out_len);

/* Pure validation of required fields and timestamps. `now_unix` <= 0 skips
 * expiry checks. `err` may be NULL; otherwise it receives a short reason. */
activation_lease_status_t activation_lease_validate(const activation_lease_t *lease,
                                                    int64_t now_unix, char *err,
                                                    size_t err_len);

/* Serialize to a JSON object. Returns bytes that would have been written
 * (snprintf-style), or -1 on invalid args. */
int activation_lease_to_json(const activation_lease_t *lease, char *out, size_t out_len);

/* Parse a lease JSON object. Unknown fields are ignored for forward
 * compatibility. */
activation_lease_status_t activation_lease_from_json(const char *json,
                                                     activation_lease_t *out);

/* Atomic local file helpers. Save writes path.tmp, fsyncs data, renames, then
 * best-effort fsyncs the parent directory. Load validates with current time. */
activation_lease_status_t activation_lease_save_file(const char *path,
                                                     const activation_lease_t *lease);
activation_lease_status_t activation_lease_load_file(const char *path,
                                                     activation_lease_t *out,
                                                     char *err, size_t err_len);
activation_lease_status_t activation_lease_remove_file(const char *path);

/* Basic local lease operations. acquire refuses to overwrite an active lease;
 * when replace_expired is true, an expired lease may be replaced. renew updates
 * expires_at after optional lease_id match. release removes the lease file after
 * optional lease_id match. */
activation_lease_status_t activation_lease_acquire_file(const char *path,
                                                       const activation_lease_t *lease,
                                                       bool replace_expired,
                                                       char *err, size_t err_len);
activation_lease_status_t activation_lease_renew_file(const char *path,
                                                     const char *lease_id,
                                                     int64_t new_expires_at,
                                                     char *err, size_t err_len);
activation_lease_status_t activation_lease_release_file(const char *path,
                                                       const char *lease_id,
                                                       char *err, size_t err_len);

#endif /* DSCO_ACTIVATION_LEASE_H */
