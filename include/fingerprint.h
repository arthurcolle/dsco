#ifndef DSCO_FINGERPRINT_H
#define DSCO_FINGERPRINT_H

/*
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │  DSCO Deep Machine Fingerprint — v3 (Full Spectrum)                    │
 * │  Platforms: macOS · Linux · Windows                                    │
 * │  Arch:      arm64 · x86_64 · x86 (32-bit fallback)                    │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │  Signal Layers                                                          │
 * │   L1  SILICON     CPU brand, vendor, model, P/E cores, cache topology  │
 * │   L2  IDENTITY    Board serials, UUIDs — OS-specific hardware IDs      │
 * │   L3  STORAGE     Primary block device model, serial, volume UUID      │
 * │   L4  NETWORK     Permanent hardware MAC of first physical interface    │
 * │   L5  FIRMWARE    BIOS/EFI/SMC versions, boot mode                    │
 * │   L6  SENSORS     Battery, GPU, display, TPM presence                  │
 * │   L7  HYPERVISOR  VM detection, hypervisor vendor, guest UUID          │
 * │   L8  RUNTIME     Page size, pointer width, endianness, ASLR entropy   │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │  Stability Tiers                                                        │
 * │   TIER_A  never changes for a given physical board                     │
 * │           serial_number, platform_uuid, board_id, mlb, efi_guid,      │
 * │           machine_id, tpm_ekpub_hash                                   │
 * │   TIER_B  changes only on hardware swap                                │
 * │           cpu_brand, cpu_vendor, machine_model, primary_mac,          │
 * │           storage_model, storage_serial, battery_serial                │
 * │   TIER_C  can change without hardware change                           │
 * │           hostname, os_version, boot_uuid, kernel_build                │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │  Derived IDs                                                            │
 * │   composite_id = SHA-256(TIER_A || TIER_B)   stable across reboots    │
 * │   session_id   = SHA-256(composite_id || boot_time)  per-boot unique  │
 * │   hw_id        = SHA-256(TIER_A only)         max-stability fallback   │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │  Privacy                                                                │
 * │   Only composite_id is transmitted externally by default.              │
 * │   Raw PII (serials, MACs, battery) requires include_pii=true.         │
 * └─────────────────────────────────────────────────────────────────────────┘
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Platform detection ─────────────────────────────────────────────────── */
#if defined(_WIN32) || defined(_WIN64)
#  define DSCO_FP_WINDOWS 1
#elif defined(__APPLE__)
#  define DSCO_FP_MACOS   1
#elif defined(__linux__)
#  define DSCO_FP_LINUX   1
#else
#  define DSCO_FP_GENERIC 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#  define DSCO_FP_ARCH_ARM64  1
#elif defined(__x86_64__) || defined(_M_X64)
#  define DSCO_FP_ARCH_X64    1
#elif defined(__i386__) || defined(_M_IX86)
#  define DSCO_FP_ARCH_X86    1
#elif defined(__riscv)
#  define DSCO_FP_ARCH_RISCV  1
#endif

/* ── Signal availability bitmask ────────────────────────────────────────── */
typedef uint32_t dsco_fp_signals_t;
#define DSCO_FPS_SERIAL      (1u << 0)   /* platform serial number */
#define DSCO_FPS_UUID        (1u << 1)   /* platform UUID / machine-id */
#define DSCO_FPS_BOARD_ID    (1u << 2)   /* board-id / baseboard serial */
#define DSCO_FPS_MLB         (1u << 3)   /* MLB / logic board serial */
#define DSCO_FPS_EFI_GUID    (1u << 4)   /* EFI GlobalId / Windows MachineGuid */
#define DSCO_FPS_CPU         (1u << 5)   /* CPU brand string */
#define DSCO_FPS_MODEL       (1u << 6)   /* machine model identifier */
#define DSCO_FPS_MAC         (1u << 7)   /* permanent hardware MAC */
#define DSCO_FPS_STORAGE     (1u << 8)   /* storage model/serial */
#define DSCO_FPS_BATTERY     (1u << 9)   /* battery serial */
#define DSCO_FPS_TPM         (1u << 10)  /* TPM EK public key hash */
#define DSCO_FPS_GPU         (1u << 11)  /* GPU model/cores */
#define DSCO_FPS_HYPERVISOR  (1u << 12)  /* hypervisor detected */
#define DSCO_FPS_BIOS        (1u << 13)  /* BIOS/firmware version */
#define DSCO_FPS_VOLUME_UUID (1u << 14)  /* primary volume UUID */

/* ── CPU feature flags ──────────────────────────────────────────────────── */
typedef struct {
    /* ARM */
    bool neon;
    bool sve;
    bool sve2;
    bool amx;            /* Apple AMX co-processor */
    bool bf16;
    bool fp16;
    bool i8mm;
    bool dotprod;
    bool sha512;
    bool sha3;
    bool lse;            /* Large System Extensions (atomic ops) */
    bool lse2;
    bool lrcpc;          /* Load-Acquire RCpc */
    bool pauth;          /* Pointer Authentication */
    bool mte;            /* Memory Tagging Extension */
    /* x86 */
    bool sse2;
    bool sse4_1;
    bool sse4_2;
    bool avx;
    bool avx2;
    bool avx512f;
    bool avx512bw;
    bool avx512vl;
    bool avx512vnni;
    bool avx512bf16;
    bool avx_vnni;
    bool f16c;
    bool fma;
    bool bmi2;
    bool aes;
    bool sha;
    bool rdrand;
    bool rdseed;
    bool popcnt;
    bool vaes;
    /* platform-specific accelerators */
    bool neural_engine;  /* Apple ANE */
    bool metal;          /* Apple Metal */
    bool secure_enclave; /* Apple SEP */
    bool touchid;        /* Apple Touch ID */
    bool amd_xnack;      /* AMD XNACK (unified memory) */
    bool intel_avx10;    /* Intel AVX10 */
} dsco_fp_features_t;

/* ── Cache topology ─────────────────────────────────────────────────────── */
typedef struct {
    uint32_t l1d_bytes;
    uint32_t l1i_bytes;
    uint32_t l2_bytes;
    uint32_t l3_bytes;
    uint32_t l4_bytes;
    uint32_t cache_line_bytes;
    /* Per performance level (Apple Silicon / hybrid Intel) */
    uint32_t perf_l2_bytes;  /* P-core L2 */
    uint32_t eff_l2_bytes;   /* E-core L2 */
} dsco_fp_cache_t;

/* ── Hypervisor info ────────────────────────────────────────────────────── */
typedef enum {
    DSCO_HV_NONE = 0,
    DSCO_HV_VMWARE,
    DSCO_HV_VIRTUALBOX,
    DSCO_HV_HYPERV,        /* Windows Hyper-V */
    DSCO_HV_KVM,
    DSCO_HV_QEMU,
    DSCO_HV_XEN,
    DSCO_HV_APPLE_HV,      /* macOS Hypervisor.framework */
    DSCO_HV_PARALLELS,
    DSCO_HV_BHYVE,
    DSCO_HV_UNKNOWN,
} dsco_hv_type_t;

/* ── Display info ────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t width_px;
    uint32_t height_px;
    uint32_t refresh_hz;
    float    scale_factor;   /* HiDPI: 2.0 for Retina */
    char     model[64];
} dsco_fp_display_t;

/* ── Full fingerprint struct ────────────────────────────────────────────── */
typedef struct {

    /* ── L1: Silicon ────────────────────────────────────────────────────── */
    char     cpu_brand[128];          /* "Apple M4 Max" / "Intel Core i9-14900K" */
    char     cpu_vendor[32];          /* "Apple" / "GenuineIntel" / "AuthenticAMD" */
    char     cpu_microarch[64];       /* "Firestorm" / "Raptor Lake" / "Zen 4" */
    char     machine_model[64];       /* "Mac16,5" / "Dell XPS 15 9500" */
    char     arch[16];                /* "arm64" / "x86_64" / "x86" */
    int      cores_logical;
    int      cores_physical;
    int      perf_cores;              /* P-cores / performance cluster */
    int      efficiency_cores;        /* E-cores / efficiency cluster */
    int      numa_nodes;
    uint64_t mem_total_bytes;
    uint64_t mem_available_bytes;
    uint64_t page_size_bytes;
    uint32_t pointer_width;           /* 32 or 64 */
    bool     is_little_endian;
    uint32_t cpuid_family;            /* x86 CPUID family */
    uint32_t cpuid_model;             /* x86 CPUID model */
    uint32_t cpuid_stepping;
    char     cpu_stepping_str[16];    /* "stepping 0x03" */
    dsco_fp_features_t features;
    dsco_fp_cache_t    cache;

    /* ── L1 extension: GPU ──────────────────────────────────────────────── */
    char     gpu_model[64];           /* "Apple M4 Max" / "NVIDIA RTX 4090" */
    uint32_t gpu_core_count;
    uint64_t gpu_vram_bytes;
    char     gpu_driver_version[32];

    /* ── L2: Identity ───────────────────────────────────────────────────── */
    /* macOS */
    char     serial_number[32];       /* IOPlatformSerialNumber */
    char     platform_uuid[64];       /* IOPlatformUUID */
    char     board_id[64];            /* board-id NVRAM (SIP-gated) */
    char     mlb[32];                 /* MLB logic board serial */
    char     provisioning_udid[48];
    /* Linux */
    char     machine_id[36];          /* /etc/machine-id */
    char     product_name[64];        /* DMI product_name */
    char     product_serial[32];      /* DMI product_serial */
    char     baseboard_serial[32];    /* DMI board_serial */
    char     chassis_serial[32];      /* DMI chassis_serial */
    char     bios_release_date[16];   /* DMI bios_date */
    /* Windows */
    char     windows_machine_guid[40]; /* HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid */
    char     windows_product_id[32];   /* HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProductId */
    char     windows_install_date[20]; /* install date (stable proxy) */
    char     windows_hwprofile_guid[40]; /* HW Profile GUID */
    /* Cross-platform */
    char     efi_guid[40];            /* EFI GlobalId / MachineGuid / IOPlatformUUID */

    /* ── L3: Storage ────────────────────────────────────────────────────── */
    char     storage_model[64];
    char     storage_serial[32];
    char     storage_protocol[32];    /* "Apple Fabric" / "NVMe" / "SATA" / "SCSI" */
    char     storage_firmware[16];
    char     primary_volume_uuid[40]; /* root/C: volume GUID */
    uint64_t storage_capacity_bytes;

    /* ── L4: Network ────────────────────────────────────────────────────── */
    char     primary_mac[18];         /* first permanent, non-virtual MAC */
    char     wifi_mac[18];
    char     iface_name[16];          /* interface the MAC came from */
    uint32_t mac_addr_assign_type;    /* 0=permanent, 1=random, 2=stolen, 3=set */

    /* ── L5: Firmware ───────────────────────────────────────────────────── */
    char     firmware_version[64];    /* SMC / EFI / BIOS version */
    char     bios_vendor[64];         /* "Apple Inc." / "American Megatrends" */
    char     bios_version[64];        /* "1.0" / "F12" */
    char     os_loader_version[32];   /* iBoot / GRUB / Windows Boot Manager */
    char     smc_version[32];         /* AppleSMC */
    bool     secure_boot_enabled;
    bool     uefi_mode;               /* true=UEFI, false=Legacy BIOS */

    /* ── L6: Sensors / Peripherals ──────────────────────────────────────── */
    char     battery_serial[32];
    uint32_t battery_cycle_count;
    uint32_t battery_design_cap_mah;
    float    battery_health_pct;
    bool     is_on_battery;
    /* Display */
    uint32_t display_count;
    dsco_fp_display_t displays[4];

    /* ── L7: Hypervisor ─────────────────────────────────────────────────── */
    bool         is_virtual;
    dsco_hv_type_t hypervisor;
    char         hypervisor_vendor[32];   /* CPUID leaf 0x40000000 string */
    char         hypervisor_uuid[40];     /* VM-assigned UUID if available */

    /* ── L8: Runtime ────────────────────────────────────────────────────── */
    uint32_t aslr_entropy_bits;       /* estimated from repeated &stack */
    bool     nx_enabled;              /* W^X / DEP */
    bool     tpm_present;
    char     tpm_version[8];          /* "2.0" / "1.2" */
    char     tpm_manufacturer[32];
    char     tpm_ekpub_hash[65];      /* SHA-256 of EK public key — TIER_A */

    /* ── OS / Runtime context ────────────────────────────────────────────── */
    char     os_name[32];             /* "darwin" / "linux" / "windows" */
    char     os_version[64];
    char     os_build[32];            /* Windows build number / macOS build */
    char     os_edition[32];          /* "Windows 11 Pro" / "" */
    char     kernel_build[256];
    char     hostname[64];
    char     username[64];

    /* ── Derived IDs ─────────────────────────────────────────────────────── */
    char     composite_id[65];   /* SHA-256(TIER_A||TIER_B) — stable, transmitted */
    char     session_id[65];     /* SHA-256(composite_id||boot_time) — per-boot */
    char     hw_id[65];          /* SHA-256(TIER_A only) — max-stable fallback */
    char     fingerprint_id[65]; /* alias for composite_id */

    /* ── Timestamps ──────────────────────────────────────────────────────── */
    int64_t  collected_at;
    int64_t  boot_time;

    /* ── Metadata ────────────────────────────────────────────────────────── */
    dsco_fp_signals_t signals_present; /* bitmask of collected signals */
    uint8_t  tier_a_count;            /* TIER_A signals collected (max 7) */
    uint8_t  tier_b_count;            /* TIER_B signals collected (max 7) */
    uint8_t  confidence;              /* 0–100 composite_id stability */
    char     platform_tag[16];        /* "macos-arm64" / "linux-x64" / "win-x64" */

} dsco_fingerprint_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Collect all available layers. Result is cached for process lifetime. */
const dsco_fingerprint_t *dsco_fingerprint_get(void);

/* Force re-collection (after hardware change, or for testing). */
int dsco_fingerprint_refresh(void);

/* JSON serialisation.
 *  include_pii  — include serial_number, MACs, battery serial, UUIDs
 *  include_all  — include every field (firmware, display, hypervisor, etc.)
 * out must be at least 512 bytes. Returns bytes written (excl. NUL). */
size_t dsco_fingerprint_to_json(const dsco_fingerprint_t *fp,
                                bool include_pii,
                                bool include_all,
                                char *out, size_t out_cap);

/* One-line human summary (for TUI / status overlays). */
size_t dsco_fingerprint_summary(const dsco_fingerprint_t *fp,
                                char *out, size_t out_cap);

/* Full stability audit: which tier signals fired, confidence. */
size_t dsco_fingerprint_stability_report(const dsco_fingerprint_t *fp,
                                         char *out, size_t out_cap);

/* hypervisor name string */
const char *dsco_hv_name(dsco_hv_type_t hv);

/* Back-compat (3-arg callers) */
static inline size_t dsco_fingerprint_to_json_compat(
        const dsco_fingerprint_t *fp, bool include_pii,
        char *out, size_t out_cap) {
    return dsco_fingerprint_to_json(fp, include_pii, false, out, out_cap);
}

#endif /* DSCO_FINGERPRINT_H */
