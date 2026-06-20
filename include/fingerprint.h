#ifndef DSCO_FINGERPRINT_H
#define DSCO_FINGERPRINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Native context fingerprint.
 *
 *  Gathered once at startup, cached for the lifetime of the process. Used by:
 *    - trust.c (sends an attestation event)
 *    - dsco_accel (picks the best backend)
 *    - mux (shows host capability in status overlay)
 *    - tamper.c (cross-checks binary identity)
 *
 *  Privacy: the *only* identifier we expose externally is `fingerprint_id` —
 *  SHA-256(cpu_brand || machine_model || arch || mac_address[0]). The raw
 *  device UUID stays local and is only sent if the user has explicitly
 *  opted in via DSCO_TRUST_INCLUDE_UUID=1.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* OS */
    char     os_name[32];         /* "darwin", "linux", ... */
    char     os_version[64];      /* uname.release */
    char     kernel_build[128];   /* uname.version */
    char     arch[16];            /* "arm64", "x86_64" */
    char     hostname[64];

    /* Hardware */
    char     cpu_brand[128];      /* e.g., "Apple M3 Max" */
    char     machine_model[64];   /* e.g., "Mac15,9" */
    int      cores_logical;
    int      cores_physical;
    int      perf_cores;          /* macOS only: hw.perflevel0.logicalcpu */
    int      efficiency_cores;    /* macOS only: hw.perflevel1.logicalcpu */
    uint64_t mem_total_bytes;
    uint64_t page_size_bytes;

    /* SIMD / accelerators */
    bool     has_neon;            /* arm64 baseline */
    bool     has_sve;
    bool     has_sve2;
    bool     has_amx;             /* Apple AMX coprocessor */
    bool     has_bf16;
    bool     has_fp16;
    bool     has_i8mm;
    bool     has_avx2;            /* x86_64 */
    bool     has_avx512;
    bool     has_neural_engine;   /* Apple Silicon */
    bool     has_metal;
    bool     has_secure_enclave;
    bool     has_touchid;

    /* Identity (local, not sent unless user opts in) */
    char     device_uuid[64];     /* IOPlatformUUID on macOS, machine-id on Linux */
    char     boot_id[64];         /* /proc/sys/kernel/random/boot_id on Linux */

    /* Derived */
    char     fingerprint_id[65];  /* SHA-256 hex; what *is* sent in attest */
    int64_t  collected_at;        /* unix seconds */
    int64_t  boot_time;           /* unix seconds when kernel booted */
} dsco_fingerprint_t;

/* Collect once. Subsequent calls return the cached snapshot. */
const dsco_fingerprint_t *dsco_fingerprint_get(void);

/* Force re-collection (for testing). */
int dsco_fingerprint_refresh(void);

/* Serialize to JSON for trust.c. `include_uuid` controls whether device_uuid
 * is included (default false — user must opt in). Returns bytes written
 * (excluding NUL). */
size_t dsco_fingerprint_to_json(const dsco_fingerprint_t *fp,
                                 bool include_uuid,
                                 char *out, size_t out_cap);

/* Compact one-line summary for status overlay and banner. */
size_t dsco_fingerprint_summary(const dsco_fingerprint_t *fp,
                                char *out, size_t out_cap);

#endif /* DSCO_FINGERPRINT_H */
