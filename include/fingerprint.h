#ifndef DSCO_FINGERPRINT_H
#define DSCO_FINGERPRINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Deep machine fingerprint — v2
 *
 *  Six signal layers, three stability tiers, one composite ID.
 *
 *  Layer 1 — SILICON:   CPU brand, model, core topology, SIMD caps, AMX, ANE
 *  Layer 2 — IDENTITY:  IOPlatformSerialNumber, IOPlatformUUID, board-id, MLB
 *  Layer 3 — STORAGE:   NVMe model + protocol, primary volume UUID
 *  Layer 4 — NETWORK:   Permanent MAC of first non-loopback ethernet
 *  Layer 5 — FIRMWARE:  SMC version, firmware build, OS loader version
 *  Layer 6 — SENSORS:   Battery serial (if present), GPU core count, display
 *
 *  Stability tiers:
 *    TIER_A (never changes):  serial_number, platform_uuid, board_id, mlb
 *    TIER_B (rare change):    cpu_brand, machine_model, primary_mac, storage_model
 *    TIER_C (can change):     hostname, os_version, boot_time
 *
 *  composite_id   = SHA-256(TIER_A || TIER_B)          — stable across reboots/renames
 *  session_id     = SHA-256(composite_id || boot_time)  — unique per boot
 *  fingerprint_id = composite_id                         — what trust.c sends
 *
 *  Privacy: raw serial numbers, UUIDs, MACs are NEVER sent externally.
 *  Only composite_id (a hash) is transmitted, and only when trust is enabled.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* ── Layer 1: Silicon ──────────────────────────────────────────────── */
    char     cpu_brand[128];        /* "Apple M4 Max" */
    char     machine_model[64];     /* "Mac16,5" */
    char     arch[16];              /* "arm64" / "x86_64" */
    int      cores_logical;
    int      cores_physical;
    int      perf_cores;            /* hw.perflevel0.logicalcpu (Apple only) */
    int      efficiency_cores;      /* hw.perflevel1.logicalcpu (Apple only) */
    uint64_t mem_total_bytes;
    uint64_t page_size_bytes;
    uint32_t gpu_core_count;        /* hw.perflevel0.gpu (Apple) or 0 */
    /* SIMD / accelerator flags */
    bool     has_neon;
    bool     has_sve;
    bool     has_sve2;
    bool     has_amx;               /* Apple AMX coprocessor */
    bool     has_bf16;
    bool     has_fp16;
    bool     has_i8mm;
    bool     has_avx2;
    bool     has_avx512;
    bool     has_neural_engine;     /* Apple Silicon ANE */
    bool     has_metal;
    bool     has_secure_enclave;
    bool     has_touchid;

    /* ── Layer 2: Identity (IOKit, never changes for a given board) ─────── */
    char     serial_number[32];     /* IOPlatformSerialNumber  e.g. "C9JXXNQNFN" */
    char     platform_uuid[64];     /* IOPlatformUUID          e.g. "8C5733C9-..." */
    char     board_id[64];          /* board-id NVRAM key      e.g. "Mac-..." */
    char     mlb[32];               /* MLB (logic board serial) */
    char     provisioning_udid[48]; /* Provisioning UDID */

    /* ── Layer 3: Storage ───────────────────────────────────────────────── */
    char     storage_model[64];     /* "APPLE SSD AP1024Z" */
    char     storage_protocol[32];  /* "Apple Fabric" / "NVMe" / "PCIe" */
    char     primary_volume_uuid[40]; /* Root / volume UUID */
    uint64_t storage_capacity_bytes;

    /* ── Layer 4: Network ───────────────────────────────────────────────── */
    char     primary_mac[18];       /* "ae:55:e2:56:36:47" — first non-virtual iface */
    char     wifi_mac[18];          /* en0 on mac if wifi, else empty */

    /* ── Layer 5: Firmware ──────────────────────────────────────────────── */
    char     firmware_version[32];  /* SMC / EFI firmware version */
    char     os_loader_version[32]; /* iBoot / OS loader version */
    char     smc_version[32];       /* AppleSMC version */

    /* ── Layer 6: Sensors / Peripherals ────────────────────────────────── */
    char     battery_serial[32];    /* BatteryData.Serial from AppleSmartBattery */
    uint32_t battery_cycle_count;   /* CycleCount */
    uint32_t battery_design_cap_mah;/* DesignCapacity */

    /* ── OS / Runtime ───────────────────────────────────────────────────── */
    char     os_name[32];           /* "darwin" / "linux" */
    char     os_version[64];        /* uname.release */
    char     kernel_build[128];     /* uname.version */
    char     hostname[64];

    /* ── Derived IDs ────────────────────────────────────────────────────── */
    char     composite_id[65];      /* SHA-256(TIER_A||TIER_B) — stable, sent in attest */
    char     session_id[65];        /* SHA-256(composite_id||boot_time) — per-boot */
    char     fingerprint_id[65];    /* alias for composite_id (back-compat) */

    /* ── Timestamps ─────────────────────────────────────────────────────── */
    int64_t  collected_at;          /* unix seconds */
    int64_t  boot_time;             /* unix seconds when kernel booted */

    /* ── Stability metadata ─────────────────────────────────────────────── */
    uint8_t  tier_a_signals;        /* count of TIER_A signals collected */
    uint8_t  tier_b_signals;        /* count of TIER_B signals collected */
    uint8_t  confidence;            /* 0-100: how confident in composite_id */
} dsco_fingerprint_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Collect all layers. Cached for process lifetime. */
const dsco_fingerprint_t *dsco_fingerprint_get(void);

/* Force re-collection (for testing or after hardware change). */
int dsco_fingerprint_refresh(void);

/* Serialize to JSON.
 *   include_pii: include serial_number, platform_uuid, mac, battery_serial
 *   include_all: include everything (for local diagnostics) */
size_t dsco_fingerprint_to_json(const dsco_fingerprint_t *fp,
                                bool include_pii,
                                bool include_all,
                                char *out, size_t out_cap);

/* One-line summary for status overlay. */
size_t dsco_fingerprint_summary(const dsco_fingerprint_t *fp,
                                char *out, size_t out_cap);

/* Stability report: what tier_a/b signals were collected, confidence score. */
size_t dsco_fingerprint_stability_report(const dsco_fingerprint_t *fp,
                                         char *out, size_t out_cap);

/* Back-compat: same as fingerprint_to_json with include_pii=false, include_all=false. */
static inline size_t dsco_fingerprint_to_json_compat(const dsco_fingerprint_t *fp,
                                                      bool include_uuid,
                                                      char *out, size_t out_cap) {
    return dsco_fingerprint_to_json(fp, include_uuid, false, out, out_cap);
}

#endif /* DSCO_FINGERPRINT_H */
