/* Deep machine fingerprint — v2
 * Six signal layers, three stability tiers, composite + session IDs.
 * See include/fingerprint.h for full architecture description.
 */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE

#include "fingerprint.h"
#include "crypto.h"
#include "json_util.h"

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
#  include <sys/sysctl.h>
#  include <net/if.h>
#  include <net/if_dl.h>
#  include <ifaddrs.h>
#  include <mach/mach_time.h>
#  include <mach-o/dyld.h>
#  include <CoreFoundation/CoreFoundation.h>
#  include <IOKit/IOKitLib.h>
#endif

#if defined(__linux__)
#  include <sys/sysinfo.h>
#  include <net/if.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <linux/if.h>
#endif

/* ── globals ────────────────────────────────────────────────────────────── */

static dsco_fingerprint_t g_fp = {0};
static bool               g_collected = false;

/* ── small helpers ──────────────────────────────────────────────────────── */

static void copy_trimmed(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (*src && isspace((unsigned char)*src)) src++;
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n-1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ── macOS helpers ──────────────────────────────────────────────────────── */

#if defined(__APPLE__)

static bool sysctl_int(const char *name, int *out) {
    size_t sz = sizeof(int);
    return sysctlbyname(name, out, &sz, NULL, 0) == 0;
}
static bool sysctl_u32(const char *name, uint32_t *out) {
    size_t sz = sizeof(uint32_t);
    return sysctlbyname(name, out, &sz, NULL, 0) == 0;
}
static bool sysctl_u64(const char *name, uint64_t *out) {
    size_t sz = sizeof(uint64_t);
    return sysctlbyname(name, out, &sz, NULL, 0) == 0;
}
static bool sysctl_str(const char *name, char *out, size_t cap) {
    size_t sz = cap;
    if (sysctlbyname(name, out, &sz, NULL, 0) != 0) return false;
    out[cap-1] = '\0';
    return true;
}

/* Read a single string property from an IOKit registry entry. */
static bool iokit_str(io_registry_entry_t entry, const char *key_c,
                      char *out, size_t cap) {
    out[0] = '\0';
    CFStringRef key = CFStringCreateWithCStringNoCopy(
        NULL, key_c, kCFStringEncodingUTF8, kCFAllocatorNull);
    if (!key) return false;
    CFTypeRef v = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    CFRelease(key);
    if (!v) return false;
    bool ok = false;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        ok = CFStringGetCString((CFStringRef)v, out, (CFIndex)cap,
                                kCFStringEncodingUTF8);
    } else if (CFGetTypeID(v) == CFDataGetTypeID()) {
        /* board-id, MLB come back as CFData (NUL-terminated C string inside) */
        CFIndex len = CFDataGetLength((CFDataRef)v);
        if (len > 0 && (size_t)len < cap) {
            memcpy(out, CFDataGetBytePtr((CFDataRef)v), (size_t)len);
            out[len] = '\0';
            ok = true;
        }
    }
    CFRelease(v);
    return ok;
}

/* Layer 2: IOPlatformExpertDevice — serial, UUID, board-id, MLB. */
static void collect_ioplatform(dsco_fingerprint_t *fp) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    io_registry_entry_t entry =
        IORegistryEntryFromPath(kIOMasterPortDefault, "IOService:/");
#pragma clang diagnostic pop
    if (entry == MACH_PORT_NULL) return;

    iokit_str(entry, "IOPlatformSerialNumber",
              fp->serial_number, sizeof(fp->serial_number));
    iokit_str(entry, "IOPlatformUUID",
              fp->platform_uuid, sizeof(fp->platform_uuid));
    iokit_str(entry, "board-id",
              fp->board_id, sizeof(fp->board_id));
    iokit_str(entry, "MLB",
              fp->mlb, sizeof(fp->mlb));
    iokit_str(entry, "ProvisioningUDID",
              fp->provisioning_udid, sizeof(fp->provisioning_udid));
    IOObjectRelease(entry);
}

/* Layer 5: Firmware — SMC, EFI/iBoot versions. */
static void collect_firmware(dsco_fingerprint_t *fp) {
    /* Firmware version lives on the platform device */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    io_registry_entry_t entry =
        IORegistryEntryFromPath(kIOMasterPortDefault, "IOService:/");
#pragma clang diagnostic pop
    if (entry != MACH_PORT_NULL) {
        iokit_str(entry, "system-firmware-version",
                  fp->firmware_version, sizeof(fp->firmware_version));
        iokit_str(entry, "os-loader-version",
                  fp->os_loader_version, sizeof(fp->os_loader_version));
        IOObjectRelease(entry);
    }

    /* SMC version */
    io_iterator_t iter = 0;
    CFMutableDictionaryRef match =
        IOServiceMatching("AppleSMC");
    if (match &&
        IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) == KERN_SUCCESS) {
        io_object_t smc = IOIteratorNext(iter);
        if (smc) {
            iokit_str(smc, "smc-version",
                      fp->smc_version, sizeof(fp->smc_version));
            IOObjectRelease(smc);
        }
        IOObjectRelease(iter);
    }
}

/* Layer 6: Battery — serial, cycle count, design capacity. */
static void collect_battery(dsco_fingerprint_t *fp) {
    io_iterator_t iter = 0;
    CFMutableDictionaryRef match = IOServiceMatching("AppleSmartBattery");
    if (!match) return;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
        return;
    io_object_t bat = IOIteratorNext(iter);
    if (!bat) { IOObjectRelease(iter); return; }

    /* Battery serial is buried in BatteryData dict — read the top-level Serial */
    iokit_str(bat, "Serial", fp->battery_serial, sizeof(fp->battery_serial));

    /* CycleCount and DesignCapacity as integers */
    CFTypeRef v;
    CFStringRef key;

    key = CFSTR("CycleCount");
    v = IORegistryEntryCreateCFProperty(bat, key, kCFAllocatorDefault, 0);
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type,
                             &fp->battery_cycle_count);
        CFRelease(v);
    }

    key = CFSTR("DesignCapacity");
    v = IORegistryEntryCreateCFProperty(bat, key, kCFAllocatorDefault, 0);
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type,
                             &fp->battery_design_cap_mah);
        CFRelease(v);
    }

    IOObjectRelease(bat);
    IOObjectRelease(iter);
}

/* Layer 4: Network — permanent MAC of first real ethernet/wifi interface.
 * macOS MACs are randomized at the AF_LINK level for user-facing ifaces,
 * but the *permanent* hardware MAC is in the Link Layer address returned by
 * getifaddrs for the BSD-level interface. We pick en0 if it has a stable
 * non-multicast/broadcast first byte.
 */
static void collect_network(dsco_fingerprint_t *fp) {
    struct ifaddrs *ifap = NULL, *ifa;
    if (getifaddrs(&ifap) != 0) return;

    /* Priority: en0 (wifi/primary), then first other en* */
    char best_mac[18] = {0};
    char wifi_mac[18]  = {0};

    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
        if (sdl->sdl_alen != 6) continue;
        uint8_t *m = (uint8_t *)LLADDR(sdl);
        /* skip multicast and all-zeros */
        if ((m[0] & 0x01) || (m[0] == 0 && m[1] == 0 && m[2] == 0)) continue;

        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);

        if (strcmp(ifa->ifa_name, "en0") == 0) {
            strncpy(wifi_mac, mac, sizeof(wifi_mac)-1);
            if (!best_mac[0])
                strncpy(best_mac, mac, sizeof(best_mac)-1);
        } else if (strncmp(ifa->ifa_name, "en", 2) == 0 && !best_mac[0]) {
            strncpy(best_mac, mac, sizeof(best_mac)-1);
        }
    }
    freeifaddrs(ifap);

    strncpy(fp->primary_mac, best_mac, sizeof(fp->primary_mac)-1);
    strncpy(fp->wifi_mac,    wifi_mac,  sizeof(fp->wifi_mac)-1);
}

/* Layer 3: Storage — primary NVMe model, protocol, capacity, volume UUID.
 * Uses system_profiler JSON output (subprocess) — only at refresh time. */
static void collect_storage(dsco_fingerprint_t *fp) {
    FILE *f = popen("system_profiler SPStorageDataType -json 2>/dev/null", "r");
    if (!f) return;

    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf)-1, f);
    pclose(f);
    buf[n] = '\0';

    /* Simple JSON field extraction — find first internal physical_drive */
    /* Look for "device_name" inside "physical_drive" for an internal disk */
    const char *internal_marker = "\"internal\":true";
    /* Fallback: just find first device_name that isn't "Disk Image" */
    const char *p = buf;
    while ((p = strstr(p, "\"device_name\"")) != NULL) {
        p += 13; /* skip key */
        const char *vs = strchr(p, '"');
        if (!vs) break;
        vs++;
        const char *ve = strchr(vs, '"');
        if (!ve) break;
        size_t len = (size_t)(ve - vs);
        if (len > 0 && strncmp(vs, "Disk Image", len) != 0) {
            if (len >= sizeof(fp->storage_model)) len = sizeof(fp->storage_model)-1;
            memcpy(fp->storage_model, vs, len);
            fp->storage_model[len] = '\0';
            break;
        }
        p = ve+1;
    }

    /* protocol */
    p = buf;
    if ((p = strstr(p, "\"protocol\"")) != NULL) {
        p += 10;
        const char *vs = strchr(p, '"');
        if (vs) {
            vs++;
            const char *ve = strchr(vs, '"');
            if (ve) {
                size_t len = (size_t)(ve-vs);
                if (len >= sizeof(fp->storage_protocol)) len = sizeof(fp->storage_protocol)-1;
                memcpy(fp->storage_protocol, vs, len);
                fp->storage_protocol[len] = '\0';
            }
        }
    }

    /* primary volume UUID — first volume_uuid that looks real */
    p = buf;
    while ((p = strstr(p, "\"volume_uuid\"")) != NULL) {
        p += 13;
        const char *vs = strchr(p, '"');
        if (!vs) break;
        vs++;
        const char *ve = strchr(vs, '"');
        if (!ve) break;
        size_t len = (size_t)(ve-vs);
        /* UUIDs are 36 chars: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
        if (len == 36) {
            if (len >= sizeof(fp->primary_volume_uuid)) len = sizeof(fp->primary_volume_uuid)-1;
            memcpy(fp->primary_volume_uuid, vs, len);
            fp->primary_volume_uuid[len] = '\0';
            break;
        }
        p = ve+1;
    }
}

/* Layer 1 extension: GPU core count */
static void collect_gpu_cores(dsco_fingerprint_t *fp) {
    /* Apple Silicon: hw.perflevel0.gpu or hw.gpu.core_count */
    int v = 0;
    if (sysctl_int("hw.gpu.core_count", &v) && v > 0)
        fp->gpu_core_count = (uint32_t)v;
    else if (sysctl_int("hw.perflevel0.gpu", &v) && v > 0)
        fp->gpu_core_count = (uint32_t)v;
}

#endif /* __APPLE__ */

/* ── Linux helpers ───────────────────────────────────────────────────────── */

#if defined(__linux__)

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
    ssize_t n = read(fd, out, cap-1);
    close(fd);
    if (n > 0) {
        out[n] = '\0';
        for (ssize_t i = n-1; i >= 0 && isspace((unsigned char)out[i]); i--)
            out[i] = '\0';
    }
}

static void linux_collect_mac(dsco_fingerprint_t *fp) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    struct ifreq ifr;
    const char *ifaces[] = {"eth0","eth1","ens3","eno1","enp0s3",NULL};
    for (int i = 0; ifaces[i]; i++) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifaces[i], IFNAMSIZ-1);
        if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
            uint8_t *m = (uint8_t *)ifr.ifr_hwaddr.sa_data;
            if (m[0]||m[1]||m[2]||m[3]||m[4]||m[5]) {
                snprintf(fp->primary_mac, sizeof(fp->primary_mac),
                         "%02x:%02x:%02x:%02x:%02x:%02x",
                         m[0],m[1],m[2],m[3],m[4],m[5]);
                break;
            }
        }
    }
    close(sock);
}

#endif /* __linux__ */

/* ── Boot time ──────────────────────────────────────────────────────────── */

static int64_t detect_boot_time(void) {
#if defined(__APPLE__)
    struct timeval tv = {0,0};
    size_t sz = sizeof(tv);
    if (sysctlbyname("kern.boottime", &tv, &sz, NULL, 0) == 0)
        return (int64_t)tv.tv_sec;
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return (int64_t)time(NULL) - (int64_t)si.uptime;
#endif
    return 0;
}

/* ── Composite ID computation ────────────────────────────────────────────── */

static void compute_composite_id(dsco_fingerprint_t *fp) {
    sha256_ctx_t s;
    sha256_init(&s);

    /* TIER_A: never changes for this physical board */
    uint8_t tier_a = 0;
#define HASH_A(field) do { \
    if (fp->field[0]) { \
        sha256_update(&s, (const uint8_t *)fp->field, strlen(fp->field)); \
        sha256_update(&s, (const uint8_t *)"|", 1); \
        tier_a++; \
    } \
} while(0)
    HASH_A(serial_number);
    HASH_A(platform_uuid);
    HASH_A(board_id);
    HASH_A(mlb);
#undef HASH_A
    fp->tier_a_signals = tier_a;

    /* TIER_B: stable, changes only on hardware swap */
    uint8_t tier_b = 0;
#define HASH_B(field) do { \
    if (fp->field[0]) { \
        sha256_update(&s, (const uint8_t *)fp->field, strlen(fp->field)); \
        sha256_update(&s, (const uint8_t *)"||", 2); \
        tier_b++; \
    } \
} while(0)
    HASH_B(cpu_brand);
    HASH_B(machine_model);
    HASH_B(arch);
    HASH_B(primary_mac);
    HASH_B(storage_model);
    HASH_B(battery_serial);
#undef HASH_B
    fp->tier_b_signals = tier_b;

    /* Confidence: 100 if all tier_a + 3+ tier_b, degrades gracefully */
    uint8_t conf = 0;
    if (tier_a >= 4)      conf  = 60;
    else if (tier_a >= 2) conf  = 40;
    else if (tier_a >= 1) conf  = 20;
    if (tier_b >= 4)      conf += 40;
    else if (tier_b >= 2) conf += 25;
    else if (tier_b >= 1) conf += 10;
    if (conf > 100) conf = 100;
    fp->confidence = conf;

    uint8_t h[32];
    sha256_final(&s, h);
    hex_encode(h, 32, fp->composite_id);
    fp->composite_id[64] = '\0';

    /* Copy back to fingerprint_id for back-compat */
    memcpy(fp->fingerprint_id, fp->composite_id, 65);

    /* session_id = SHA-256(composite_id || boot_time) */
    char boot_str[24];
    snprintf(boot_str, sizeof(boot_str), "%lld", (long long)fp->boot_time);
    sha256_init(&s);
    sha256_update(&s, (const uint8_t *)fp->composite_id, 64);
    sha256_update(&s, (const uint8_t *)"|", 1);
    sha256_update(&s, (const uint8_t *)boot_str, strlen(boot_str));
    sha256_final(&s, h);
    hex_encode(h, 32, fp->session_id);
    fp->session_id[64] = '\0';
}

/* ── Main collection ─────────────────────────────────────────────────────── */

int dsco_fingerprint_refresh(void) {
    memset(&g_fp, 0, sizeof(g_fp));
    g_fp.collected_at = (int64_t)time(NULL);
    g_fp.boot_time    = detect_boot_time();

    /* OS */
    struct utsname u;
    if (uname(&u) == 0) {
        copy_trimmed(g_fp.os_name,     sizeof(g_fp.os_name),     u.sysname);
        copy_trimmed(g_fp.os_version,  sizeof(g_fp.os_version),  u.release);
        copy_trimmed(g_fp.kernel_build,sizeof(g_fp.kernel_build), u.version);
        copy_trimmed(g_fp.arch,        sizeof(g_fp.arch),         u.machine);
        copy_trimmed(g_fp.hostname,    sizeof(g_fp.hostname),     u.nodename);
        for (char *p = g_fp.os_name; *p; p++)
            *p = (char)tolower((unsigned char)*p);
    }

    g_fp.page_size_bytes = (uint64_t)sysconf(_SC_PAGESIZE);

#if defined(__APPLE__)
    /* Layer 1: Silicon */
    sysctl_str("machdep.cpu.brand_string", g_fp.cpu_brand, sizeof(g_fp.cpu_brand));
    sysctl_str("hw.model", g_fp.machine_model, sizeof(g_fp.machine_model));
    {
        int v = 0;
        sysctl_int("hw.logicalcpu",              &g_fp.cores_logical);
        sysctl_int("hw.physicalcpu",             &g_fp.cores_physical);
        sysctl_int("hw.perflevel0.logicalcpu",   &g_fp.perf_cores);
        sysctl_int("hw.perflevel1.logicalcpu",   &g_fp.efficiency_cores);
        sysctl_u64("hw.memsize",                 &g_fp.mem_total_bytes);
        /* SIMD */
        g_fp.has_neon = (strstr(g_fp.arch, "arm") != NULL);
        if (sysctl_int("hw.optional.arm.FEAT_SVE",   &v)) g_fp.has_sve   = (v!=0);
        if (sysctl_int("hw.optional.arm.FEAT_SVE2",  &v)) g_fp.has_sve2  = (v!=0);
        if (sysctl_int("hw.optional.arm.FEAT_FP16",  &v)) g_fp.has_fp16  = (v!=0);
        if (sysctl_int("hw.optional.arm.FEAT_BF16",  &v)) g_fp.has_bf16  = (v!=0);
        if (sysctl_int("hw.optional.arm.FEAT_I8MM",  &v)) g_fp.has_i8mm  = (v!=0);
        if (sysctl_int("hw.optional.amx_version",    &v)) g_fp.has_amx   = (v!=0);
        if (sysctl_int("hw.optional.avx2_0",         &v)) g_fp.has_avx2  = (v!=0);
        g_fp.has_metal          = true;
        g_fp.has_neural_engine  = (strstr(g_fp.cpu_brand, "Apple") != NULL);
        g_fp.has_secure_enclave = (strstr(g_fp.cpu_brand, "Apple") != NULL);
    }
    collect_gpu_cores(&g_fp);

    /* Layer 2: Identity */
    collect_ioplatform(&g_fp);

    /* Layer 3: Storage */
    collect_storage(&g_fp);

    /* Layer 4: Network */
    collect_network(&g_fp);

    /* Layer 5: Firmware */
    collect_firmware(&g_fp);

    /* Layer 6: Battery */
    collect_battery(&g_fp);

#elif defined(__linux__)
    /* Layer 1: Silicon */
    linux_cpuinfo_field("model name", g_fp.cpu_brand, sizeof(g_fp.cpu_brand));
    if (!g_fp.cpu_brand[0])
        linux_cpuinfo_field("Processor", g_fp.cpu_brand, sizeof(g_fp.cpu_brand));
    g_fp.cores_logical  = (int)sysconf(_SC_NPROCESSORS_ONLN);
    g_fp.cores_physical = g_fp.cores_logical;
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0)
            g_fp.mem_total_bytes = (uint64_t)si.totalram * (uint64_t)si.mem_unit;
    }
    char flags[2048] = {0};
    linux_cpuinfo_field("Features", flags, sizeof(flags));
    if (!flags[0]) linux_cpuinfo_field("flags", flags, sizeof(flags));
    if (strstr(flags,"neon"))   g_fp.has_neon   = true;
    if (strstr(flags,"sve"))    g_fp.has_sve    = true;
    if (strstr(flags,"sve2"))   g_fp.has_sve2   = true;
    if (strstr(flags,"fphp"))   g_fp.has_fp16   = true;
    if (strstr(flags,"bf16"))   g_fp.has_bf16   = true;
    if (strstr(flags,"i8mm"))   g_fp.has_i8mm   = true;
    if (strstr(flags,"avx2"))   g_fp.has_avx2   = true;
    if (strstr(flags,"avx512f"))g_fp.has_avx512 = true;

    /* Layer 2: Identity */
    linux_read_file("/sys/class/dmi/id/product_name",  g_fp.machine_model,   sizeof(g_fp.machine_model));
    linux_read_file("/sys/class/dmi/id/product_serial", g_fp.serial_number,  sizeof(g_fp.serial_number));
    linux_read_file("/sys/class/dmi/id/board_serial",   g_fp.mlb,            sizeof(g_fp.mlb));
    linux_read_file("/etc/machine-id",                  g_fp.platform_uuid,  sizeof(g_fp.platform_uuid));
    linux_read_file("/proc/sys/kernel/random/boot_id",  g_fp.board_id,       sizeof(g_fp.board_id));

    /* Layer 4: Network */
    linux_collect_mac(&g_fp);
#endif

    /* Composite ID (runs on both platforms) */
    compute_composite_id(&g_fp);

    g_collected = true;
    return 0;
}

const dsco_fingerprint_t *dsco_fingerprint_get(void) {
    if (!g_collected) dsco_fingerprint_refresh();
    return &g_fp;
}

/* ── JSON serialization ──────────────────────────────────────────────────── */

static int je(const char *s, char *out, size_t cap) {
    if (!s) { if(cap>0) out[0]='\0'; return 0; }
    size_t i=0;
    while (*s && i<cap-2) {
        char c=*s++;
        if (c=='"'||c=='\\') { out[i++]='\\'; out[i++]=c; }
        else if(c=='\n')     { out[i++]='\\'; out[i++]='n'; }
        else if(c=='\r')     { out[i++]='\\'; out[i++]='r'; }
        else if(c=='\t')     { out[i++]='\\'; out[i++]='t'; }
        else if((unsigned char)c<0x20) { /* skip ctrl */ }
        else out[i++]=c;
    }
    if(i<cap) out[i]='\0';
    return (int)i;
}

#define JSTR(KEY, VAL) do { \
    char _e[512]; je((VAL), _e, sizeof(_e)); \
    int _n=snprintf(p,end-p,"%s\"%s\":\"%s\"", first?"":",", KEY, _e); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)

#define JINT(KEY, VAL) do { \
    int _n=snprintf(p,end-p,"%s\"%s\":%lld", first?"":",", KEY, (long long)(VAL)); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)

#define JU64(KEY, VAL) do { \
    int _n=snprintf(p,end-p,"%s\"%s\":%llu", first?"":",", KEY, (unsigned long long)(VAL)); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)

#define JBOOL(KEY, VAL) do { \
    int _n=snprintf(p,end-p,"%s\"%s\":%s", first?"":",", KEY, (VAL)?"true":"false"); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)

size_t dsco_fingerprint_to_json(const dsco_fingerprint_t *fp,
                                bool include_pii,
                                bool include_all,
                                char *out, size_t out_cap) {
    if (!fp || !out || out_cap < 16) return 0;
    char *p = out, *end = out + out_cap;
    bool first = true;

    int n = snprintf(p, end-p, "{"); p += n; first=true;

    /* Always: composite IDs and silicon summary */
    JSTR("composite_id",    fp->composite_id);
    JSTR("session_id",      fp->session_id);
    JSTR("os",              fp->os_name);
    JSTR("os_version",      fp->os_version);
    JSTR("arch",            fp->arch);
    JSTR("cpu_brand",       fp->cpu_brand);
    JSTR("machine_model",   fp->machine_model);
    JINT("cores_logical",   fp->cores_logical);
    JINT("cores_physical",  fp->cores_physical);
    JINT("perf_cores",      fp->perf_cores);
    JINT("eff_cores",       fp->efficiency_cores);
    JU64("mem_bytes",       fp->mem_total_bytes);
    JINT("gpu_cores",       fp->gpu_core_count);
    JINT("confidence",      fp->confidence);
    JINT("tier_a",          fp->tier_a_signals);
    JINT("tier_b",          fp->tier_b_signals);
    JINT("ts",              fp->collected_at);
    JINT("boot_time",       fp->boot_time);

    /* Features array */
    {
        int nn = snprintf(p, end-p, ",\"features\":["); if(nn<0||nn>=end-p) goto done; p+=nn;
        bool ff = true;
#define F(NAME, COND) do { if(COND){ \
    int n2=snprintf(p,end-p,"%s\"%s\"",ff?"":",",NAME); if(n2<0||n2>=end-p) goto done; p+=n2; ff=false; } } while(0)
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
        nn = snprintf(p, end-p, "]"); if(nn<0||nn>=end-p) goto done; p+=nn;
    }

    if (include_pii || include_all) {
        JSTR("serial_number",       fp->serial_number);
        JSTR("platform_uuid",       fp->platform_uuid);
        JSTR("board_id",            fp->board_id);
        JSTR("mlb",                 fp->mlb);
        JSTR("provisioning_udid",   fp->provisioning_udid);
        JSTR("primary_mac",         fp->primary_mac);
        JSTR("wifi_mac",            fp->wifi_mac);
        JSTR("battery_serial",      fp->battery_serial);
    }

    if (include_all) {
        JSTR("storage_model",       fp->storage_model);
        JSTR("storage_protocol",    fp->storage_protocol);
        JSTR("primary_volume_uuid", fp->primary_volume_uuid);
        JU64("storage_capacity",    fp->storage_capacity_bytes);
        JSTR("firmware_version",    fp->firmware_version);
        JSTR("os_loader_version",   fp->os_loader_version);
        JSTR("smc_version",         fp->smc_version);
        JINT("battery_cycle_count", fp->battery_cycle_count);
        JINT("battery_design_cap",  fp->battery_design_cap_mah);
        JSTR("kernel_build",        fp->kernel_build);
        JSTR("hostname",            fp->hostname);
        JU64("page_bytes",          fp->page_size_bytes);
    }

    if (p+2 < end) { *p++='}'; *p='\0'; }
done:
    return (size_t)(p - out);
}
#undef JSTR
#undef JINT
#undef JU64
#undef JBOOL

size_t dsco_fingerprint_summary(const dsco_fingerprint_t *fp, char *out, size_t out_cap) {
    if (!fp || !out) return 0;
    int n = snprintf(out, out_cap,
        "%s %s · %s · %d P+%d E cores · %.1f GB · %u GPU · conf:%u%% · id:%.12s",
        fp->cpu_brand[0] ? fp->cpu_brand : "?",
        fp->arch[0]      ? fp->arch : "?",
        fp->machine_model[0] ? fp->machine_model : "?",
        fp->perf_cores, fp->efficiency_cores,
        (double)fp->mem_total_bytes / (1024.0*1024.0*1024.0),
        fp->gpu_core_count,
        fp->confidence,
        fp->composite_id);
    return n < 0 ? 0 : (size_t)n;
}

size_t dsco_fingerprint_stability_report(const dsco_fingerprint_t *fp,
                                          char *out, size_t out_cap) {
    if (!fp || !out) return 0;
    int n = snprintf(out, out_cap,
        "Fingerprint stability report\n"
        "  composite_id : %.16s...  (confidence %u%%)\n"
        "  session_id   : %.16s...\n"
        "  tier_A signals (%u/4): serial=%s uuid=%s board_id=%s mlb=%s\n"
        "  tier_B signals (%u/6): cpu=%s model=%s mac=%s storage=%s battery_serial=%s\n"
        "  collected    : %lld  boot: %lld\n",
        fp->composite_id, fp->confidence,
        fp->session_id,
        fp->tier_a_signals,
        fp->serial_number[0]  ? "✓" : "✗",
        fp->platform_uuid[0]  ? "✓" : "✗",
        fp->board_id[0]       ? "✓" : "✗",
        fp->mlb[0]            ? "✓" : "✗",
        fp->tier_b_signals,
        fp->cpu_brand[0]      ? "✓" : "✗",
        fp->machine_model[0]  ? "✓" : "✗",
        fp->primary_mac[0]    ? "✓" : "✗",
        fp->storage_model[0]  ? "✓" : "✗",
        fp->battery_serial[0] ? "✓" : "✗",
        (long long)fp->collected_at,
        (long long)fp->boot_time);
    return n < 0 ? 0 : (size_t)n;
}
