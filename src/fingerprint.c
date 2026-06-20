#define _DARWIN_C_SOURCE
#define _GNU_SOURCE

#include "fingerprint.h"
#include "crypto.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
  #include <sys/sysctl.h>
  #include <mach/mach_time.h>
  #include <mach-o/dyld.h>
  #include <CoreFoundation/CoreFoundation.h>
  #include <IOKit/IOKitLib.h>
#endif

#if defined(__linux__)
  #include <sys/sysinfo.h>
#endif

static dsco_fingerprint_t g_fp = {0};
static bool                g_collected = false;

/* ──────────────────────────────────────────────────────────────────────────
 *  Small helpers
 * ────────────────────────────────────────────────────────────────────────── */

static void copy_trimmed(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (*src && isspace((unsigned char)*src)) src++;
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

#if defined(__APPLE__)
static bool sysctl_int(const char *name, int *out) {
    int v = 0;
    size_t sz = sizeof(v);
    if (sysctlbyname(name, &v, &sz, NULL, 0) != 0) return false;
    *out = v;
    return true;
}
static bool sysctl_u64(const char *name, uint64_t *out) {
    uint64_t v = 0;
    size_t sz = sizeof(v);
    if (sysctlbyname(name, &v, &sz, NULL, 0) != 0) return false;
    *out = v;
    return true;
}
static bool sysctl_str(const char *name, char *out, size_t cap) {
    size_t sz = cap;
    if (sysctlbyname(name, out, &sz, NULL, 0) != 0) return false;
    out[cap - 1] = '\0';
    return true;
}

/* IOPlatformUUID — stable per-machine; treat as sensitive. */
static void darwin_platform_uuid(char *out, size_t cap) {
    out[0] = '\0';
    io_registry_entry_t entry = IORegistryEntryFromPath(kIOMasterPortDefault,
                                "IOService:/");
    if (entry == MACH_PORT_NULL) return;
    CFStringRef key = CFSTR("IOPlatformUUID");
    CFTypeRef v = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    IOObjectRelease(entry);
    if (!v) return;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)v, out, cap, kCFStringEncodingUTF8);
    }
    CFRelease(v);
}
#endif

#if defined(__linux__)
/* Find the first line starting with `key :` in /proc/cpuinfo. */
static void linux_cpuinfo_field(const char *key, char *out, size_t cap) {
    out[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[1024];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        copy_trimmed(out, cap, colon + 1);
        break;
    }
    fclose(f);
}

static void linux_read_file(const char *path, char *out, size_t cap) {
    out[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n > 0) {
        out[n] = '\0';
        /* strip trailing whitespace */
        for (ssize_t i = n - 1; i >= 0 && isspace((unsigned char)out[i]); i--) out[i] = '\0';
    }
}
#endif

/* ──────────────────────────────────────────────────────────────────────────
 *  Boot time
 * ────────────────────────────────────────────────────────────────────────── */

static int64_t detect_boot_time(void) {
#if defined(__APPLE__)
    struct timeval tv = {0, 0};
    size_t sz = sizeof(tv);
    if (sysctlbyname("kern.boottime", &tv, &sz, NULL, 0) == 0) {
        return (int64_t)tv.tv_sec;
    }
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        return (int64_t)time(NULL) - (int64_t)si.uptime;
    }
#endif
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Collection
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_fingerprint_refresh(void) {
    memset(&g_fp, 0, sizeof(g_fp));
    g_fp.collected_at = (int64_t)time(NULL);
    g_fp.boot_time    = detect_boot_time();

    /* uname */
    struct utsname u;
    if (uname(&u) == 0) {
        copy_trimmed(g_fp.os_name,     sizeof(g_fp.os_name),     u.sysname);
        copy_trimmed(g_fp.os_version,  sizeof(g_fp.os_version),  u.release);
        copy_trimmed(g_fp.kernel_build,sizeof(g_fp.kernel_build),u.version);
        copy_trimmed(g_fp.arch,        sizeof(g_fp.arch),        u.machine);
        copy_trimmed(g_fp.hostname,    sizeof(g_fp.hostname),    u.nodename);
        /* lowercase os name */
        for (char *p = g_fp.os_name; *p; p++) *p = (char)tolower((unsigned char)*p);
    }

    g_fp.page_size_bytes = (uint64_t)sysconf(_SC_PAGESIZE);

#if defined(__APPLE__)
    sysctl_str("machdep.cpu.brand_string", g_fp.cpu_brand, sizeof(g_fp.cpu_brand));
    sysctl_str("hw.model", g_fp.machine_model, sizeof(g_fp.machine_model));
    sysctl_int("hw.logicalcpu",  &g_fp.cores_logical);
    sysctl_int("hw.physicalcpu", &g_fp.cores_physical);
    sysctl_int("hw.perflevel0.logicalcpu", &g_fp.perf_cores);
    sysctl_int("hw.perflevel1.logicalcpu", &g_fp.efficiency_cores);
    sysctl_u64("hw.memsize", &g_fp.mem_total_bytes);

    /* ARM feature flags */
    int v = 0;
    g_fp.has_neon = (strstr(g_fp.arch, "arm") != NULL);
    if (sysctl_int("hw.optional.arm.FEAT_SVE",   &v)) g_fp.has_sve  = (v != 0);
    if (sysctl_int("hw.optional.arm.FEAT_SVE2",  &v)) g_fp.has_sve2 = (v != 0);
    if (sysctl_int("hw.optional.arm.FEAT_FP16",  &v)) g_fp.has_fp16 = (v != 0);
    if (sysctl_int("hw.optional.arm.FEAT_BF16",  &v)) g_fp.has_bf16 = (v != 0);
    if (sysctl_int("hw.optional.arm.FEAT_I8MM",  &v)) g_fp.has_i8mm = (v != 0);
    if (sysctl_int("hw.optional.amx_version",    &v)) g_fp.has_amx  = (v != 0);

    /* Apple-specific capabilities */
    g_fp.has_metal = true;     /* every supported Mac since 10.11 */
    g_fp.has_neural_engine = (strstr(g_fp.cpu_brand, "Apple") != NULL);
    g_fp.has_secure_enclave = (strstr(g_fp.cpu_brand, "Apple") != NULL ||
                               strstr(g_fp.machine_model, "MacBookPro") != NULL);
#ifdef HAVE_TOUCHID
    g_fp.has_touchid = true;
#endif

    darwin_platform_uuid(g_fp.device_uuid, sizeof(g_fp.device_uuid));

#elif defined(__linux__)
    linux_cpuinfo_field("model name", g_fp.cpu_brand, sizeof(g_fp.cpu_brand));
    if (!g_fp.cpu_brand[0]) linux_cpuinfo_field("Processor", g_fp.cpu_brand, sizeof(g_fp.cpu_brand));
    g_fp.cores_logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
    g_fp.cores_physical = g_fp.cores_logical; /* don't have a portable distinct count */
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        g_fp.mem_total_bytes = (uint64_t)si.totalram * (uint64_t)si.mem_unit;
    }
    char flags[2048] = {0};
    linux_cpuinfo_field("Features", flags, sizeof(flags));     /* arm */
    if (!flags[0]) linux_cpuinfo_field("flags", flags, sizeof(flags));  /* x86 */
    if (strstr(flags, "neon")) g_fp.has_neon = true;
    if (strstr(flags, "sve"))  g_fp.has_sve  = true;
    if (strstr(flags, "sve2")) g_fp.has_sve2 = true;
    if (strstr(flags, "fphp")) g_fp.has_fp16 = true;
    if (strstr(flags, "bf16")) g_fp.has_bf16 = true;
    if (strstr(flags, "i8mm")) g_fp.has_i8mm = true;
    if (strstr(flags, "avx2")) g_fp.has_avx2 = true;
    if (strstr(flags, "avx512f")) g_fp.has_avx512 = true;

    char model[256] = {0};
    linux_read_file("/sys/class/dmi/id/product_name", model, sizeof(model));
    copy_trimmed(g_fp.machine_model, sizeof(g_fp.machine_model), model);

    linux_read_file("/etc/machine-id", g_fp.device_uuid, sizeof(g_fp.device_uuid));
    linux_read_file("/proc/sys/kernel/random/boot_id", g_fp.boot_id, sizeof(g_fp.boot_id));
#endif

    /* fingerprint_id = SHA-256(cpu_brand || machine_model || arch || hostname) */
    sha256_ctx_t s;
    sha256_init(&s);
    sha256_update(&s, (const uint8_t *)g_fp.cpu_brand,      strlen(g_fp.cpu_brand));
    sha256_update(&s, (const uint8_t *)"|", 1);
    sha256_update(&s, (const uint8_t *)g_fp.machine_model,  strlen(g_fp.machine_model));
    sha256_update(&s, (const uint8_t *)"|", 1);
    sha256_update(&s, (const uint8_t *)g_fp.arch,           strlen(g_fp.arch));
    sha256_update(&s, (const uint8_t *)"|", 1);
    sha256_update(&s, (const uint8_t *)g_fp.os_name,        strlen(g_fp.os_name));
    sha256_update(&s, (const uint8_t *)"|", 1);
    /* Use hostname only as a weak salt — fp_id should change if user
     * deliberately renames the host, but the user can also pin a stable
     * salt via DSCO_FINGERPRINT_SALT for cross-rename continuity. */
    const char *salt = getenv("DSCO_FINGERPRINT_SALT");
    sha256_update(&s, (const uint8_t *)(salt ? salt : g_fp.hostname),
                  strlen(salt ? salt : g_fp.hostname));
    uint8_t h[32];
    sha256_final(&s, h);
    hex_encode(h, 32, g_fp.fingerprint_id);
    g_fp.fingerprint_id[64] = '\0';

    g_collected = true;
    return 0;
}

const dsco_fingerprint_t *dsco_fingerprint_get(void) {
    if (!g_collected) dsco_fingerprint_refresh();
    return &g_fp;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  JSON & summary
 * ────────────────────────────────────────────────────────────────────────── */

static int json_escape(const char *s, char *out, size_t cap) {
    if (cap == 0) return 0;
    size_t i = 0;
    if (!s) { out[0] = '\0'; return 0; }
    while (*s && i < cap - 2) {
        char c = *s++;
        if (c == '"' || c == '\\') {
            if (i + 2 >= cap) break;
            out[i++] = '\\'; out[i++] = c;
        } else if (c == '\n') { out[i++] = '\\'; out[i++] = 'n'; }
        else if (c == '\r') { out[i++] = '\\'; out[i++] = 'r'; }
        else if (c == '\t') { out[i++] = '\\'; out[i++] = 't'; }
        else if ((unsigned char)c < 0x20) { /* skip controls */ }
        else out[i++] = c;
    }
    out[i] = '\0';
    return (int)i;
}

#define J(KEY, FMT, ...) do { \
    int _n = snprintf(p, end - p, "%s\"" KEY "\":" FMT, first ? "" : ",", __VA_ARGS__); \
    if (_n < 0 || _n >= end - p) goto done; \
    p += _n; first = false; \
} while (0)

size_t dsco_fingerprint_to_json(const dsco_fingerprint_t *fp,
                                 bool include_uuid,
                                 char *out, size_t out_cap) {
    if (!fp || !out || out_cap < 16) return 0;
    char *p = out;
    char *end = out + out_cap;
    bool first = true;

    char esc_cpu[256], esc_model[128], esc_kernel[256], esc_host[128];
    json_escape(fp->cpu_brand,     esc_cpu,    sizeof(esc_cpu));
    json_escape(fp->machine_model, esc_model,  sizeof(esc_model));
    json_escape(fp->kernel_build,  esc_kernel, sizeof(esc_kernel));
    json_escape(fp->hostname,      esc_host,   sizeof(esc_host));

    int n = snprintf(p, end - p, "{");
    p += n; first = true;

    J("fingerprint_id", "\"%s\"", fp->fingerprint_id);
    J("ts",             "%lld",   (long long)fp->collected_at);
    J("boot_time",      "%lld",   (long long)fp->boot_time);
    J("os",             "\"%s\"", fp->os_name);
    J("os_version",     "\"%s\"", fp->os_version);
    J("kernel",         "\"%s\"", esc_kernel);
    J("arch",           "\"%s\"", fp->arch);
    J("cpu_brand",      "\"%s\"", esc_cpu);
    J("machine_model",  "\"%s\"", esc_model);
    J("cores_logical",  "%d",     fp->cores_logical);
    J("cores_physical", "%d",     fp->cores_physical);
    J("perf_cores",     "%d",     fp->perf_cores);
    J("eff_cores",      "%d",     fp->efficiency_cores);
    J("mem_bytes",      "%llu",   (unsigned long long)fp->mem_total_bytes);
    J("page_bytes",     "%llu",   (unsigned long long)fp->page_size_bytes);

    /* features array */
    {
        int nn = snprintf(p, end - p, ",\"features\":[");
        if (nn < 0 || nn >= end - p) goto done;
        p += nn;
        bool ff = true;
#define F(NAME, COND) do { if (COND) { \
    int n2 = snprintf(p, end - p, "%s\"" NAME "\"", ff ? "" : ","); \
    if (n2 < 0 || n2 >= end - p) goto done; p += n2; ff = false; } \
} while (0)
        F("neon",           fp->has_neon);
        F("sve",            fp->has_sve);
        F("sve2",           fp->has_sve2);
        F("amx",            fp->has_amx);
        F("bf16",           fp->has_bf16);
        F("fp16",           fp->has_fp16);
        F("i8mm",           fp->has_i8mm);
        F("avx2",           fp->has_avx2);
        F("avx512",         fp->has_avx512);
        F("metal",          fp->has_metal);
        F("neural_engine",  fp->has_neural_engine);
        F("secure_enclave", fp->has_secure_enclave);
        F("touchid",        fp->has_touchid);
#undef F
        nn = snprintf(p, end - p, "]");
        if (nn < 0 || nn >= end - p) goto done; p += nn;
    }
    if (include_uuid && fp->device_uuid[0]) {
        int nn = snprintf(p, end - p, ",\"device_uuid\":\"%s\"", fp->device_uuid);
        if (nn < 0 || nn >= end - p) goto done; p += nn;
    }
    {
        int nn = snprintf(p, end - p, ",\"hostname\":\"%s\"", esc_host);
        if (nn < 0 || nn >= end - p) goto done; p += nn;
    }

    if (p + 2 < end) { *p++ = '}'; *p = '\0'; }
done:
    return (size_t)(p - out);
}
#undef J

size_t dsco_fingerprint_summary(const dsco_fingerprint_t *fp,
                                char *out, size_t out_cap) {
    if (!fp || !out) return 0;
    int n = snprintf(out, out_cap,
        "%s %s · %s · %d cores · %.1f GB · fp:%.12s",
        fp->cpu_brand[0] ? fp->cpu_brand : "?",
        fp->arch[0] ? fp->arch : "?",
        fp->machine_model[0] ? fp->machine_model : "?",
        fp->cores_logical,
        (double)fp->mem_total_bytes / (1024.0 * 1024.0 * 1024.0),
        fp->fingerprint_id);
    return n < 0 ? 0 : (size_t)n;
}
