/* ┌──────────────────────────────────────────────────────────────────────────┐
 * │  dsco_fingerprint.c — Full-Spectrum Machine Fingerprint v3              │
 * │  Platforms: macOS · Linux · Windows                                     │
 * │  Arch:      arm64 · x86_64 · x86                                        │
 * └──────────────────────────────────────────────────────────────────────────┘ */

/* Platform feature test macros MUST come before any system headers */
#if defined(_WIN32) || defined(_WIN64)
#  define DSCO_FP_WINDOWS 1
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0602   /* Windows 8+ for GetFirmwareType etc. */
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#elif defined(__APPLE__)
#  define DSCO_FP_MACOS 1
#  define _DARWIN_C_SOURCE
#elif defined(__linux__)
#  define DSCO_FP_LINUX 1
#  define _GNU_SOURCE
#endif

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
#include <time.h>
#include <unistd.h>

/* ── POSIX headers (macOS + Linux) ─────────────────────────────────────── */
#if !defined(DSCO_FP_WINDOWS)
#  include <sys/utsname.h>
#  include <sys/socket.h>
#  include <net/if.h>
#  include <ifaddrs.h>
#  include <pwd.h>
#endif

/* ── macOS-specific ─────────────────────────────────────────────────────── */
#if defined(DSCO_FP_MACOS)
#  include <sys/sysctl.h>
#  include <net/if_dl.h>
#  include <mach/mach_time.h>
#  include <CoreFoundation/CoreFoundation.h>
#  include <IOKit/IOKitLib.h>
#endif

/* ── Linux-specific ─────────────────────────────────────────────────────── */
#if defined(DSCO_FP_LINUX)
#  include <sys/sysinfo.h>
#  include <sys/ioctl.h>
#  include <linux/if.h>
#  include <dirent.h>
#endif

/* ── Windows-specific ────────────────────────────────────────────────────── */
#if defined(DSCO_FP_WINDOWS)
#  include <windows.h>
#  include <intrin.h>
#  include <iphlpapi.h>
#  include <powerbase.h>
#  include <setupapi.h>
#  include <devguid.h>
#  include <winreg.h>
#  include <sysinfoapi.h>
#  pragma comment(lib, "iphlpapi.lib")
#  pragma comment(lib, "powrprof.lib")
#  pragma comment(lib, "setupapi.lib")
/* WMI (optional) */
#  if defined(_MSC_VER) || defined(__MINGW32__)
#    include <wbemidl.h>
#    pragma comment(lib, "wbemuuid.lib")
#  endif
#endif

/* ── x86 CPUID ───────────────────────────────────────────────────────────── */
#if defined(DSCO_FP_ARCH_X64) || defined(DSCO_FP_ARCH_X86)
#  if defined(_MSC_VER)
#    include <intrin.h>
#    define dsco_cpuid(leaf, a, b, c, d) do { \
         int __r[4]; __cpuid(__r, (int)(leaf)); \
         (a)=__r[0];(b)=__r[1];(c)=__r[2];(d)=__r[3]; } while(0)
#    define dsco_cpuidex(leaf, subleaf, a, b, c, d) do { \
         int __r[4]; __cpuidex(__r,(int)(leaf),(int)(subleaf)); \
         (a)=__r[0];(b)=__r[1];(c)=__r[2];(d)=__r[3]; } while(0)
#  else
#    include <cpuid.h>
#    define dsco_cpuid(leaf, a, b, c, d) \
         __cpuid((unsigned)(leaf),(unsigned&)(a),(unsigned&)(b),(unsigned&)(c),(unsigned&)(d))
#    define dsco_cpuidex(leaf, subleaf, a, b, c, d) \
         __cpuid_count((unsigned)(leaf),(unsigned)(subleaf),(unsigned&)(a),(unsigned&)(b),(unsigned&)(c),(unsigned&)(d))
#  endif
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * Global state
 * ───────────────────────────────────────────────────────────────────────── */
static dsco_fingerprint_t g_fp;
static bool               g_collected = false;

/* ─────────────────────────────────────────────────────────────────────────
 * Tiny utilities
 * ───────────────────────────────────────────────────────────────────────── */

static void fp_trim(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src)  { dst[0] = '\0'; return; }
    while (*src && isspace((unsigned char)*src)) src++;
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n-1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool fp_nonempty(const char *s) { return s && s[0] && strcmp(s,"None") && strcmp(s,"n/a"); }

/* Run a shell command, capture first line into out[cap]. */
static bool fp_popen_str(const char *cmd, char *out, size_t cap) {
    out[0] = '\0';
#if defined(DSCO_FP_WINDOWS)
    FILE *f = _popen(cmd, "r");
#else
    FILE *f = popen(cmd, "r");
#endif
    if (!f) return false;
    char *r = fgets(out, (int)cap, f);
#if defined(DSCO_FP_WINDOWS)
    _pclose(f);
#else
    pclose(f);
#endif
    if (!r) { out[0]='\0'; return false; }
    for (size_t i = strlen(out); i > 0 && isspace((unsigned char)out[i-1]); i--)
        out[i-1] = '\0';
    return out[0] != '\0';
}

/* Read entire file into buf. */
static bool __attribute__((unused)) fp_read_file(const char *path, char *out, size_t cap) {
    out[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, out, cap-1);
    close(fd);
    if (n <= 0) return false;
    out[n] = '\0';
    for (ssize_t i = n-1; i >= 0 && isspace((unsigned char)out[i]); i--)
        out[i] = '\0';
    return out[0] != '\0';
}

/* ─────────────────────────────────────────────────────────────────────────
 * L8: Runtime intrinsics (all platforms)
 * ───────────────────────────────────────────────────────────────────────── */
static void collect_runtime(dsco_fingerprint_t *fp) {
    fp->pointer_width    = (uint32_t)(sizeof(void*) * 8);
    fp->page_size_bytes  = (uint64_t)sysconf(_SC_PAGESIZE);
    uint16_t e = 1;
    fp->is_little_endian = *(uint8_t*)&e == 1;

    /* ASLR entropy: sample stack address repeated to measure bit-width of randomization */
    uintptr_t addrs[8];
    for (int i = 0; i < 8; i++) {
        volatile char c; addrs[i] = (uintptr_t)&c;
    }
    /* rough: non-null high bits common across samples */
    fp->aslr_entropy_bits = 0;
    uintptr_t mask = ~(uintptr_t)0;
    for (int i = 0; i < 8; i++) mask &= addrs[i];
    /* count trailing zero bits as min entropy */
    while (mask && !(mask & 1)) { fp->aslr_entropy_bits++; mask >>= 1; }
}

/* ─────────────────────────────────────────────────────────────────────────
 * L1: CPU — x86 CPUID path
 * ───────────────────────────────────────────────────────────────────────── */
#if defined(DSCO_FP_ARCH_X64) || defined(DSCO_FP_ARCH_X86)
static void collect_cpu_x86(dsco_fingerprint_t *fp) {
    uint32_t a, b, c, d;

    /* Leaf 0: max leaf + vendor */
    dsco_cpuid(0, a, b, c, d);
    uint32_t max_leaf = a;
    char vendor[13] = {0};
    memcpy(vendor+0, &b, 4); memcpy(vendor+4, &d, 4); memcpy(vendor+8, &c, 4);
    fp_trim(fp->cpu_vendor, sizeof(fp->cpu_vendor), vendor);

    if (!strcmp(vendor,"GenuineIntel")) fp_trim(fp->cpu_vendor, sizeof(fp->cpu_vendor), "Intel");
    else if (!strcmp(vendor,"AuthenticAMD")) fp_trim(fp->cpu_vendor, sizeof(fp->cpu_vendor), "AMD");
    else if (!strcmp(vendor,"HygonGenuine")) fp_trim(fp->cpu_vendor, sizeof(fp->cpu_vendor), "Hygon");

    /* Leaf 1: family/model/stepping + basic features */
    if (max_leaf >= 1) {
        dsco_cpuid(1, a, b, c, d);
        fp->cpuid_family   = ((a >> 8) & 0xF) + ((a >> 20) & 0xFF);
        fp->cpuid_model    = ((a >> 4) & 0xF) + (((a >> 16) & 0xF) << 4);
        fp->cpuid_stepping = a & 0xF;
        snprintf(fp->cpu_stepping_str, sizeof(fp->cpu_stepping_str),
                 "0x%02x", fp->cpuid_stepping);

        fp->features.sse2   = (d >> 26) & 1;
        fp->features.aes    = (c >> 25) & 1;
        fp->features.avx    = (c >> 28) & 1;
        fp->features.f16c   = (c >> 29) & 1;
        fp->features.rdrand = (c >> 30) & 1;
        fp->features.popcnt = (c >> 23) & 1;
        fp->features.fma    = (c >> 12) & 1;
        fp->features.sse4_1 = (c >> 19) & 1;
        fp->features.sse4_2 = (c >> 20) & 1;
    }

    /* Leaf 7: extended features */
    if (max_leaf >= 7) {
        dsco_cpuidex(7, 0, a, b, c, d);
        fp->features.avx2      = (b >> 5)  & 1;
        fp->features.bmi2      = (b >> 8)  & 1;
        fp->features.avx512f   = (b >> 16) & 1;
        fp->features.avx512bw  = (b >> 30) & 1;
        fp->features.avx512vl  = (b >> 31) & 1;
        fp->features.sha       = (b >> 29) & 1;
        fp->features.rdseed    = (b >> 18) & 1;
        fp->features.avx512vnni= (c >> 11) & 1;
        fp->features.vaes      = (c >> 9)  & 1;

        /* Sub-leaf 1: AVX-VNNI */
        dsco_cpuidex(7, 1, a, b, c, d);
        fp->features.avx_vnni   = (a >> 4) & 1;
        fp->features.avx512bf16 = (a >> 5) & 1;
        fp->features.intel_avx10= (d >> 19) & 1;
    }

    /* Brand string from extended leaves 0x80000002-0x80000004 */
    dsco_cpuid(0x80000000, a, b, c, d);
    if (a >= 0x80000004) {
        char brand[49] = {0};
        for (int i = 0; i < 3; i++) {
            uint32_t r[4];
            dsco_cpuidex(0x80000002 + i, 0,
                         r[0], r[1], r[2], r[3]);
            memcpy(brand + i*16, r, 16);
        }
        fp_trim(fp->cpu_brand, sizeof(fp->cpu_brand), brand);
    }

    /* Hypervisor detection: leaf 0x40000000 */
    {
        dsco_cpuid(0x40000000, a, b, c, d);
        char hvstr[13] = {0};
        memcpy(hvstr+0, &b, 4); memcpy(hvstr+4, &c, 4); memcpy(hvstr+8, &d, 4);
        if (hvstr[0]) {
            fp->is_virtual = true;
            fp_trim(fp->hypervisor_vendor, sizeof(fp->hypervisor_vendor), hvstr);
            if (strstr(hvstr,"VMwareVMware"))  fp->hypervisor = DSCO_HV_VMWARE;
            else if (strstr(hvstr,"VBoxVBox")) fp->hypervisor = DSCO_HV_VIRTUALBOX;
            else if (strstr(hvstr,"Microsoft")) fp->hypervisor = DSCO_HV_HYPERV;
            else if (strstr(hvstr,"KVMKVMKVM")) fp->hypervisor = DSCO_HV_KVM;
            else if (strstr(hvstr,"prl hyperv")) fp->hypervisor = DSCO_HV_PARALLELS;
            else if (strstr(hvstr,"XenVMMXen")) fp->hypervisor = DSCO_HV_XEN;
            else if (strstr(hvstr,"bhyve")) fp->hypervisor = DSCO_HV_BHYVE;
            else fp->hypervisor = DSCO_HV_UNKNOWN;
        }
    }

    /* Logical/physical cores */
#if !defined(DSCO_FP_WINDOWS)
    fp->cores_logical  = (int)sysconf(_SC_NPROCESSORS_ONLN);
    fp->cores_physical = fp->cores_logical;
    fp->mem_total_bytes = (uint64_t)sysconf(_SC_PHYS_PAGES) *
                          (uint64_t)sysconf(_SC_PAGESIZE);
#endif
}
#endif /* x86 */

/* ─────────────────────────────────────────────────────────────────────────
 * ██████  macOS implementation
 * ───────────────────────────────────────────────────────────────────────── */
#if defined(DSCO_FP_MACOS)

static bool mac_sysctl_int(const char *n, int *v) {
    size_t s=sizeof(int); return sysctlbyname(n,v,&s,NULL,0)==0; }
static bool mac_sysctl_u32(const char *n, uint32_t *v) {
    size_t s=sizeof(uint32_t); return sysctlbyname(n,v,&s,NULL,0)==0; }
static bool mac_sysctl_u64(const char *n, uint64_t *v) {
    size_t s=sizeof(uint64_t); return sysctlbyname(n,v,&s,NULL,0)==0; }
static bool mac_sysctl_str(const char *n, char *o, size_t c) {
    size_t s=c; if(sysctlbyname(n,o,&s,NULL,0)!=0) return false; o[c-1]='\0'; return true; }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static io_registry_entry_t mac_ioplatform(void) {
    return IORegistryEntryFromPath(kIOMasterPortDefault, "IOService:/");
}

static bool mac_iokit_str(io_registry_entry_t e, const char *key,
                           char *out, size_t cap) {
    out[0] = '\0';
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    if (!k) return false;
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    if (!v) return false;
    bool ok = false;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        ok = CFStringGetCString((CFStringRef)v, out, (CFIndex)cap, kCFStringEncodingUTF8);
    } else if (CFGetTypeID(v) == CFDataGetTypeID()) {
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

static bool mac_iokit_u32(io_registry_entry_t e, const char *key, uint32_t *out) {
    *out = 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    if (!k) return false;
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    if (!v) return false;
    bool ok = false;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        ok = CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, out);
    }
    CFRelease(v);
    return ok;
}

/* L2: IOPlatformExpertDevice identity */
static void mac_collect_identity(dsco_fingerprint_t *fp) {
    io_registry_entry_t e = mac_ioplatform();
    if (e == MACH_PORT_NULL) return;
    mac_iokit_str(e, "IOPlatformSerialNumber", fp->serial_number,   sizeof(fp->serial_number));
    mac_iokit_str(e, "IOPlatformUUID",         fp->platform_uuid,   sizeof(fp->platform_uuid));
    mac_iokit_str(e, "board-id",               fp->board_id,        sizeof(fp->board_id));
    mac_iokit_str(e, "MLB",                    fp->mlb,             sizeof(fp->mlb));
    mac_iokit_str(e, "ProvisioningUDID",       fp->provisioning_udid, sizeof(fp->provisioning_udid));
    /* efi_guid = platform_uuid for cross-platform field */
    if (fp->platform_uuid[0] && !fp->efi_guid[0])
        snprintf(fp->efi_guid, sizeof(fp->efi_guid), "%s", fp->platform_uuid);
    IOObjectRelease(e);
}

/* L5: Firmware — system-firmware-version, os-loader-version, SMC */
static void mac_collect_firmware(dsco_fingerprint_t *fp) {
    io_registry_entry_t e = mac_ioplatform();
    if (e != MACH_PORT_NULL) {
        mac_iokit_str(e, "system-firmware-version", fp->firmware_version, sizeof(fp->firmware_version));
        mac_iokit_str(e, "os-loader-version",       fp->os_loader_version, sizeof(fp->os_loader_version));
        IOObjectRelease(e);
    }
    fp->uefi_mode = true; /* Apple always UEFI */

    /* SMC */
    io_iterator_t iter = 0;
    CFMutableDictionaryRef m = IOServiceMatching("AppleSMC");
    if (m && IOServiceGetMatchingServices(kIOMainPortDefault, m, &iter) == KERN_SUCCESS) {
        io_object_t s = IOIteratorNext(iter);
        if (s) { mac_iokit_str(s,"smc-version",fp->smc_version,sizeof(fp->smc_version)); IOObjectRelease(s); }
        IOObjectRelease(iter);
    }
}

/* L6: Battery */
static void mac_collect_battery(dsco_fingerprint_t *fp) {
    io_iterator_t iter = 0;
    CFMutableDictionaryRef m = IOServiceMatching("AppleSmartBattery");
    if (!m) return;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &iter) != KERN_SUCCESS) return;
    io_object_t bat = IOIteratorNext(iter);
    if (!bat) { IOObjectRelease(iter); return; }

    mac_iokit_str(bat, "Serial", fp->battery_serial, sizeof(fp->battery_serial));
    mac_iokit_u32(bat, "CycleCount",      &fp->battery_cycle_count);
    mac_iokit_u32(bat, "DesignCapacity",  &fp->battery_design_cap_mah);

    /* IsCharging + BatteryInstalled */
    CFTypeRef v = IORegistryEntryCreateCFProperty(bat, CFSTR("ExternalConnected"),
                                                  kCFAllocatorDefault, 0);
    if (v) {
        if (CFGetTypeID(v)==CFBooleanGetTypeID())
            fp->is_on_battery = !CFBooleanGetValue((CFBooleanRef)v);
        CFRelease(v);
    }
    IOObjectRelease(bat);
    IOObjectRelease(iter);
}

/* L1 extension: GPU via AGXAccelerator */
static void mac_collect_gpu(dsco_fingerprint_t *fp) {
    io_iterator_t iter = 0;
    CFMutableDictionaryRef m = IOServiceMatching("AGXAccelerator");
    if (!m) goto try_metal;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &iter) != KERN_SUCCESS) goto try_metal;
    io_object_t gpu = IOIteratorNext(iter);
    if (gpu) {
        mac_iokit_str(gpu, "model",          fp->gpu_model,      sizeof(fp->gpu_model));
        mac_iokit_u32(gpu, "gpu-core-count", &fp->gpu_core_count);
        IOObjectRelease(gpu);
    }
    IOObjectRelease(iter);
    return;

try_metal:
    /* Fallback for older macOS / non-Apple-Silicon */
    fp_popen_str(
        "system_profiler SPDisplaysDataType 2>/dev/null | awk '/Chipset Model:/{sub(/.*Chipset Model: /,\"\"); print; exit}'",
        fp->gpu_model, sizeof(fp->gpu_model));
}

/* L4: Network — permanent hardware MAC */
static void mac_collect_network(dsco_fingerprint_t *fp) {
    struct ifaddrs *ifap = NULL, *ifa;
    if (getifaddrs(&ifap) != 0) return;

    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
        if (sdl->sdl_alen != 6) continue;
        uint8_t *m = (uint8_t *)LLADDR(sdl);
        if ((m[0] & 0x01) || (!m[0] && !m[1] && !m[2])) continue;

        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0],m[1],m[2],m[3],m[4],m[5]);

        if (strcmp(ifa->ifa_name, "en0") == 0) {
            if (!fp->wifi_mac[0]) snprintf(fp->wifi_mac,sizeof(fp->wifi_mac),"%s",mac);
            if (!fp->primary_mac[0]) {
                snprintf(fp->primary_mac,sizeof(fp->primary_mac),"%s",mac);
                snprintf(fp->iface_name,sizeof(fp->iface_name),"%s",ifa->ifa_name);
                fp->mac_addr_assign_type = 0; /* permanent */
            }
        } else if (strncmp(ifa->ifa_name,"en",2)==0 && !fp->primary_mac[0]) {
            snprintf(fp->primary_mac,sizeof(fp->primary_mac),"%s",mac);
            snprintf(fp->iface_name,sizeof(fp->iface_name),"%s",ifa->ifa_name);
            fp->mac_addr_assign_type = 0;
        }
    }
    freeifaddrs(ifap);
}

/* L3: Storage — via system_profiler JSON */
static void mac_collect_storage(dsco_fingerprint_t *fp) {
    FILE *f = popen("system_profiler SPStorageDataType -json 2>/dev/null", "r");
    if (!f) return;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf)-1, f);
    pclose(f);
    buf[n] = '\0';

    /* device_name (NVMe model) */
    const char *p = buf;
    while ((p = strstr(p, "\"device_name\"")) != NULL) {
        p += 13;
        const char *vs = strchr(p,'"'); if (!vs) break; vs++;
        const char *ve = strchr(vs,'"'); if (!ve) break;
        size_t len = (size_t)(ve-vs);
        if (len > 0 && strncmp(vs,"Disk Image",len) && strncmp(vs,"disk image",len)) {
            if (len >= sizeof(fp->storage_model)) len=sizeof(fp->storage_model)-1;
            memcpy(fp->storage_model,vs,len); fp->storage_model[len]='\0'; break;
        }
        p = ve+1;
    }
    /* protocol */
    p = buf;
    if ((p = strstr(p,"\"protocol\"")) != NULL) {
        const char *vs = strchr(p+10,'"'); if (vs) { vs++;
        const char *ve = strchr(vs,'"'); if (ve) {
            size_t len=(size_t)(ve-vs);
            if(len>=sizeof(fp->storage_protocol))len=sizeof(fp->storage_protocol)-1;
            memcpy(fp->storage_protocol,vs,len); fp->storage_protocol[len]='\0'; }}
    }
    /* volume_uuid (first real 36-char UUID) */
    p = buf;
    while ((p = strstr(p,"\"volume_uuid\"")) != NULL) {
        p += 13;
        const char *vs=strchr(p,'"'); if(!vs) break; vs++;
        const char *ve=strchr(vs,'"'); if(!ve) break;
        size_t len=(size_t)(ve-vs);
        if (len==36) {
            if(len>=sizeof(fp->primary_volume_uuid))len=sizeof(fp->primary_volume_uuid)-1;
            memcpy(fp->primary_volume_uuid,vs,len); fp->primary_volume_uuid[len]='\0'; break;
        }
        p=ve+1;
    }
}

/* L1: Apple Silicon sysctl-based CPU collection */
static void mac_collect_silicon(dsco_fingerprint_t *fp) {
    mac_sysctl_str("machdep.cpu.brand_string", fp->cpu_brand, sizeof(fp->cpu_brand));
    mac_sysctl_str("hw.model",                 fp->machine_model, sizeof(fp->machine_model));
    snprintf(fp->cpu_vendor, sizeof(fp->cpu_vendor), "%s",
             strstr(fp->cpu_brand,"Apple") ? "Apple" :
             strstr(fp->cpu_brand,"Intel") ? "Intel" : "Unknown");

    mac_sysctl_u64("hw.memsize",             &fp->mem_total_bytes);
    {
        int v=0;
        mac_sysctl_int("hw.logicalcpu",            &fp->cores_logical);
        mac_sysctl_int("hw.physicalcpu",           &fp->cores_physical);
        mac_sysctl_int("hw.perflevel0.logicalcpu", &fp->perf_cores);
        mac_sysctl_int("hw.perflevel1.logicalcpu", &fp->efficiency_cores);

        /* Cache topology */
        { uint64_t _v=0; if(mac_sysctl_u64("hw.l1dcachesize",&_v))  fp->cache.l1d_bytes=(uint32_t)_v; }
        { uint64_t _v=0; if(mac_sysctl_u64("hw.l1icachesize",&_v))  fp->cache.l1i_bytes=(uint32_t)_v; }
        { uint64_t _v=0; if(mac_sysctl_u64("hw.l2cachesize",&_v))   fp->cache.l2_bytes=(uint32_t)_v; }
        { uint64_t _v=0; if(mac_sysctl_u64("hw.cachelinesize",&_v)) fp->cache.cache_line_bytes=(uint32_t)_v; }
        mac_sysctl_u32("hw.perflevel0.l2cachesize", &fp->cache.perf_l2_bytes);
        mac_sysctl_u32("hw.perflevel1.l2cachesize", &fp->cache.eff_l2_bytes);

        /* ARM feature flags */
        if(mac_sysctl_int("hw.optional.arm.FEAT_BF16",  &v)) fp->features.bf16      = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_FP16",  &v)) fp->features.fp16      = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_I8MM",  &v)) fp->features.i8mm      = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_SHA512",&v)) fp->features.sha512    = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_SHA3",  &v)) fp->features.sha3      = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_LSE",   &v)) fp->features.lse       = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_LSE2",  &v)) fp->features.lse2      = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_RDM",   &v)) fp->features.dotprod   = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_LRCPC", &v)) fp->features.lrcpc     = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_LRCPC2",&v)) fp->features.lrcpc     |= (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_SVE",   &v)) fp->features.sve       = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_SVE2",  &v)) fp->features.sve2      = (v!=0);
        if(mac_sysctl_int("hw.optional.amx_version",    &v)) fp->features.amx       = (v!=0);
        if(mac_sysctl_int("hw.optional.arm.FEAT_PAUTHQARMA5",&v)) fp->features.pauth= (v!=0);

        fp->features.neon          = true; /* all Apple Silicon */
        fp->features.metal         = true;
        fp->features.neural_engine = (strstr(fp->cpu_brand,"Apple") != NULL);
        fp->features.secure_enclave= (strstr(fp->cpu_brand,"Apple") != NULL);
    }
}

/* macOS top-level collector */
static void collect_macos(dsco_fingerprint_t *fp) {
    snprintf(fp->platform_tag, sizeof(fp->platform_tag), "macos-%s",
#if defined(DSCO_FP_ARCH_ARM64)
             "arm64"
#elif defined(DSCO_FP_ARCH_X64)
             "x64"
#else
             "x86"
#endif
    );

#if defined(DSCO_FP_ARCH_ARM64)
    mac_collect_silicon(fp);
#else
    collect_cpu_x86(fp);
    /* fill model from sysctl even on Intel Mac */
    mac_sysctl_str("hw.model", fp->machine_model, sizeof(fp->machine_model));
    mac_sysctl_u64("hw.memsize", &fp->mem_total_bytes);
    /* Cache */
    mac_sysctl_u32("hw.l1dcachesize", &fp->cache.l1d_bytes);
    mac_sysctl_u32("hw.l1icachesize", &fp->cache.l1i_bytes);
    mac_sysctl_u32("hw.l2cachesize",  &fp->cache.l2_bytes);
    mac_sysctl_u32("hw.cachelinesize",&fp->cache.cache_line_bytes);
#endif

    mac_collect_identity(fp);
    mac_collect_network(fp);
    mac_collect_storage(fp);
    mac_collect_firmware(fp);
    mac_collect_battery(fp);
    mac_collect_gpu(fp);

    /* NUMA */
    fp->numa_nodes = 1; /* macOS unified memory — always 1 */

    /* Secure boot — check csr-active-config (SIP proxy) */
    {
        /*(void)csr;*/
        /* kern.bootargs existence unused */ /* existence check */
        /* If we could read board_id from IOKit, SIP is off = privileged env */
        fp->secure_boot_enabled = !fp->board_id[0]; /* board_id SIP-gated on AS */
    }
}

#pragma clang diagnostic pop
#endif /* DSCO_FP_MACOS */

/* ─────────────────────────────────────────────────────────────────────────
 * ██████  Linux implementation
 * ───────────────────────────────────────────────────────────────────────── */
#if defined(DSCO_FP_LINUX)

/* Read from a sysfs file */
static bool linux_sysfs(const char *path, char *out, size_t cap) {
    return fp_read_file(path, out, cap);
}

/* /proc/cpuinfo field */
static void linux_cpuinfo(const char *key, char *out, size_t cap) {
    out[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo","r");
    if (!f) return;
    char line[1024];
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0) continue;
        char *col = strchr(line,':');
        if (!col) continue;
        fp_trim(out, cap, col+1);
        break;
    }
    fclose(f);
}

/* L1: CPU on Linux */
static void linux_collect_cpu(dsco_fingerprint_t *fp) {
    linux_cpuinfo("model name",  fp->cpu_brand,   sizeof(fp->cpu_brand));
    if (!fp->cpu_brand[0])
        linux_cpuinfo("Processor",   fp->cpu_brand,   sizeof(fp->cpu_brand));
    if (!fp->cpu_brand[0])
        linux_cpuinfo("Model name",  fp->cpu_brand,   sizeof(fp->cpu_brand));

    /* Vendor */
    linux_cpuinfo("vendor_id",   fp->cpu_vendor,  sizeof(fp->cpu_vendor));
    if (!fp->cpu_vendor[0]) linux_cpuinfo("CPU implementer", fp->cpu_vendor, sizeof(fp->cpu_vendor));

    /* CPU flags */
    char flags[4096] = {0};
    linux_cpuinfo("Features", flags, sizeof(flags));
    if (!flags[0]) linux_cpuinfo("flags", flags, sizeof(flags));

    fp->features.neon    = !!strstr(flags,"neon") || !!strstr(flags,"asimd");
    fp->features.sve     = !!strstr(flags,"sve");
    fp->features.sve2    = !!strstr(flags,"sve2");
    fp->features.fp16    = !!strstr(flags,"fphp");
    fp->features.bf16    = !!strstr(flags,"bf16");
    fp->features.i8mm    = !!strstr(flags,"i8mm");
    fp->features.dotprod = !!strstr(flags,"asimddp");
    fp->features.sha512  = !!strstr(flags,"sha512");
    fp->features.sha3    = !!strstr(flags,"sha3");
    fp->features.lse     = !!strstr(flags,"atomics");
    fp->features.mte     = !!strstr(flags,"mte");
    fp->features.pauth   = !!strstr(flags,"paca");
    fp->features.lrcpc   = !!strstr(flags,"lrcpc");
    fp->features.sse2    = !!strstr(flags,"sse2");
    fp->features.sse4_1  = !!strstr(flags,"sse4_1");
    fp->features.sse4_2  = !!strstr(flags,"sse4_2");
    fp->features.avx     = !!strstr(flags,"avx");
    fp->features.avx2    = !!strstr(flags,"avx2");
    fp->features.avx512f = !!strstr(flags,"avx512f");
    fp->features.avx512bw= !!strstr(flags,"avx512bw");
    fp->features.avx512vl= !!strstr(flags,"avx512vl");
    fp->features.avx512vnni=!!strstr(flags,"avx512_vnni");
    fp->features.fma     = !!strstr(flags,"fma");
    fp->features.aes     = !!strstr(flags,"aes");
    fp->features.sha     = !!strstr(flags,"sha_ni");
    fp->features.rdrand  = !!strstr(flags,"rdrand");
    fp->features.rdseed  = !!strstr(flags,"rdseed");
    fp->features.popcnt  = !!strstr(flags,"popcnt");
    fp->features.vaes    = !!strstr(flags,"vaes");
    fp->features.f16c    = !!strstr(flags,"f16c");
    fp->features.bmi2    = !!strstr(flags,"bmi2");

    fp->cores_logical   = (int)sysconf(_SC_NPROCESSORS_ONLN);
    fp->cores_physical  = fp->cores_logical;
    /* Try to get physical from /sys */
    {
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/topology/core_id","r");
        if (f) fclose(f);
        /* count unique core_ids */
        DIR *d = opendir("/sys/devices/system/cpu");
        if (d) {
            struct dirent *de;
            int phys = 0, seen[2048] = {0};
            while ((de=readdir(d))) {
                if (strncmp(de->d_name,"cpu",3)!=0 || !isdigit(de->d_name[3])) continue;
                char p[128];
                snprintf(p,sizeof(p),"/sys/devices/system/cpu/%s/topology/core_id",de->d_name);
                char val[8]={0}; fp_read_file(p,val,sizeof(val));
                if (val[0]) { int id=atoi(val); if(id<2048&&!seen[id]){seen[id]=1;phys++;} }
            }
            closedir(d);
            if (phys > 0) fp->cores_physical = phys;
        }
    }

    /* Cache from /sys */
    {
        char v[32];
        if (linux_sysfs("/sys/devices/system/cpu/cpu0/cache/index0/size", v, sizeof(v)))
            fp->cache.l1d_bytes = (uint32_t)atoi(v) * 1024;
        if (linux_sysfs("/sys/devices/system/cpu/cpu0/cache/index1/size", v, sizeof(v)))
            fp->cache.l1i_bytes = (uint32_t)atoi(v) * 1024;
        if (linux_sysfs("/sys/devices/system/cpu/cpu0/cache/index2/size", v, sizeof(v)))
            fp->cache.l2_bytes  = (uint32_t)atoi(v) * 1024;
        if (linux_sysfs("/sys/devices/system/cpu/cpu0/cache/index3/size", v, sizeof(v)))
            fp->cache.l3_bytes  = (uint32_t)atoi(v) * 1024;
        if (linux_sysfs("/sys/devices/system/cpu/cpu0/cache/coherency_line_size", v, sizeof(v)))
            fp->cache.cache_line_bytes = (uint32_t)atoi(v);
    }

    /* Memory */
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            fp->mem_total_bytes     = (uint64_t)si.totalram * si.mem_unit;
            fp->mem_available_bytes = (uint64_t)si.freeram  * si.mem_unit;
        }
    }

    /* NUMA nodes */
    {
        DIR *d = opendir("/sys/devices/system/node");
        if (d) {
            struct dirent *de; fp->numa_nodes = 0;
            while ((de=readdir(d)))
                if (strncmp(de->d_name,"node",4)==0 && isdigit(de->d_name[4]))
                    fp->numa_nodes++;
            closedir(d);
        }
        if (fp->numa_nodes == 0) fp->numa_nodes = 1;
    }
}

/* L2: Identity on Linux — DMI + machine-id */
static void linux_collect_identity(dsco_fingerprint_t *fp) {
    linux_sysfs("/sys/class/dmi/id/product_name",    fp->product_name,     sizeof(fp->product_name));
    linux_sysfs("/sys/class/dmi/id/product_serial",  fp->product_serial,   sizeof(fp->product_serial));
    linux_sysfs("/sys/class/dmi/id/board_serial",    fp->baseboard_serial, sizeof(fp->baseboard_serial));
    linux_sysfs("/sys/class/dmi/id/chassis_serial",  fp->chassis_serial,   sizeof(fp->chassis_serial));
    linux_sysfs("/sys/class/dmi/id/bios_date",       fp->bios_release_date,sizeof(fp->bios_release_date));
    linux_sysfs("/sys/class/dmi/id/sys_vendor",      fp->cpu_vendor,       sizeof(fp->cpu_vendor));
    /* machine-id */
    linux_sysfs("/etc/machine-id", fp->machine_id, sizeof(fp->machine_id));
    /* dbus fallback */
    if (!fp->machine_id[0])
        linux_sysfs("/var/lib/dbus/machine-id", fp->machine_id, sizeof(fp->machine_id));
    /* efi_guid */
    linux_sysfs("/sys/firmware/efi/efivars/PlatformUUID-8be4df61-93ca-11d2-aa0d-00e098032b8c",
                fp->efi_guid, sizeof(fp->efi_guid));
    if (!fp->efi_guid[0] && fp->machine_id[0])
        snprintf(fp->efi_guid, sizeof(fp->efi_guid), "%s", fp->machine_id);
    /* cross-platform aliases */
    if (fp->product_serial[0] && !fp->serial_number[0])
        snprintf(fp->serial_number, sizeof(fp->serial_number), "%s", fp->product_serial);
    if (fp->machine_id[0] && !fp->platform_uuid[0])
        snprintf(fp->platform_uuid, sizeof(fp->platform_uuid), "%s", fp->machine_id);
    if (fp->product_name[0] && !fp->machine_model[0])
        snprintf(fp->machine_model, sizeof(fp->machine_model), "%s", fp->product_name);
    if (fp->baseboard_serial[0] && !fp->mlb[0])
        snprintf(fp->mlb, sizeof(fp->mlb), "%s", fp->baseboard_serial);
}

/* L5: Firmware on Linux */
static void linux_collect_firmware(dsco_fingerprint_t *fp) {
    linux_sysfs("/sys/class/dmi/id/bios_version",  fp->bios_version, sizeof(fp->bios_version));
    linux_sysfs("/sys/class/dmi/id/bios_vendor",   fp->bios_vendor,  sizeof(fp->bios_vendor));
    fp->uefi_mode = (access("/sys/firmware/efi", F_OK) == 0);
    /* Secure boot */
    char sb[8] = {0};
    linux_sysfs("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c",
                sb, sizeof(sb));
    fp->secure_boot_enabled = (sb[4] == 1); /* attribute bytes precede value */
    if (!fp->firmware_version[0])
        snprintf(fp->firmware_version, sizeof(fp->firmware_version), "%s", fp->bios_version);
}

/* L4: Network — permanent MAC, prefer addr_assign_type=0 */
static void linux_collect_network(dsco_fingerprint_t *fp) {
    struct ifaddrs *ifap = NULL, *ifa;
    if (getifaddrs(&ifap) != 0) return;
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_PACKET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        /* Check addr_assign_type via sysfs */
        char atpath[128], atval[8]={0};
        snprintf(atpath,sizeof(atpath),"/sys/class/net/%s/addr_assign_type",ifa->ifa_name);
        fp_read_file(atpath, atval, sizeof(atval));
        int at = atval[0] ? atoi(atval) : -1;
        fp->mac_addr_assign_type = (uint32_t)(at >= 0 ? at : 0);

        struct sockaddr *sa = ifa->ifa_addr;
        /* AF_PACKET: sll_addr at offset 10 */
        uint8_t *m = (uint8_t*)sa + 10;
        if (!m[0] && !m[1] && !m[2]) continue;

        char mac[18];
        snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0],m[1],m[2],m[3],m[4],m[5]);

        bool is_physical = (at == 0); /* 0 = permanent */
        if (is_physical && !fp->primary_mac[0]) {
            snprintf(fp->primary_mac, sizeof(fp->primary_mac), "%s", mac);
            snprintf(fp->iface_name,  sizeof(fp->iface_name),  "%s", ifa->ifa_name);
        }
        if (!fp->primary_mac[0]) {
            snprintf(fp->primary_mac, sizeof(fp->primary_mac), "%s", mac);
            snprintf(fp->iface_name,  sizeof(fp->iface_name),  "%s", ifa->ifa_name);
        }
    }
    freeifaddrs(ifap);
}

/* L3: Storage on Linux — /sys/block */
static void linux_collect_storage(dsco_fingerprint_t *fp) {
    /* Find first real NVMe or SATA disk */
    const char *preferred[] = {"nvme0","sda","sdb","vda","hda",NULL};
    char base[64] = {0};
    for (int i = 0; preferred[i]; i++) {
        char p[128];
        snprintf(p,sizeof(p),"/sys/block/%s",preferred[i]);
        if (access(p,F_OK)==0) { snprintf(base,sizeof(base),"%s",preferred[i]); break; }
    }
    if (!base[0]) return;

    char p[256];
    snprintf(p,sizeof(p),"/sys/block/%s/device/model",base);
    linux_sysfs(p, fp->storage_model, sizeof(fp->storage_model));

    snprintf(p,sizeof(p),"/sys/block/%s/device/serial",base);
    char ser[64]={0};
    linux_sysfs(p,ser,sizeof(ser));
    /* strip spaces (SCSI serials have padding) */
    for (char *c=ser;*c;c++) if(!isspace((unsigned char)*c)||c>ser)
        fp->storage_serial[strlen(fp->storage_serial)] = *c;
    fp->storage_serial[sizeof(fp->storage_serial)-1]='\0';

    snprintf(p,sizeof(p),"/sys/block/%s/device/firmware_rev",base);
    linux_sysfs(p, fp->storage_firmware, sizeof(fp->storage_firmware));

    /* protocol */
    if (strncmp(base,"nvme",4)==0)
        snprintf(fp->storage_protocol,sizeof(fp->storage_protocol),"NVMe");
    else
        snprintf(fp->storage_protocol,sizeof(fp->storage_protocol),"SATA");

    /* capacity */
    snprintf(p,sizeof(p),"/sys/block/%s/size",base);
    char szbuf[32]={0};
    if (fp_read_file(p,szbuf,sizeof(szbuf)))
        fp->storage_capacity_bytes = (uint64_t)atoll(szbuf) * 512;

    /* primary volume UUID from /etc/fstab or blkid */
    fp_popen_str("blkid -s UUID -o value / 2>/dev/null | head -1",
                 fp->primary_volume_uuid, sizeof(fp->primary_volume_uuid));
}

/* L6: Battery on Linux */
static void linux_collect_battery(dsco_fingerprint_t *fp) {
    /* Find first battery in /sys/class/power_supply */
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *de;
    char bat[64] = {0};
    while ((de=readdir(d))) {
        if (strncmp(de->d_name,"BAT",3)==0 || strncmp(de->d_name,"battery",7)==0) {
            snprintf(bat,sizeof(bat),"%s",de->d_name); break;
        }
    }
    closedir(d);
    if (!bat[0]) return;

    char p[128];
    snprintf(p,sizeof(p),"/sys/class/power_supply/%s/serial_number",bat);
    linux_sysfs(p, fp->battery_serial, sizeof(fp->battery_serial));

    snprintf(p,sizeof(p),"/sys/class/power_supply/%s/cycle_count",bat);
    char v[16]={0}; if(fp_read_file(p,v,sizeof(v))) fp->battery_cycle_count=(uint32_t)atoi(v);

    snprintf(p,sizeof(p),"/sys/class/power_supply/%s/charge_full_design",bat);
    memset(v,0,sizeof(v)); if(fp_read_file(p,v,sizeof(v)))
        fp->battery_design_cap_mah=(uint32_t)(atoi(v)/1000); /* µAh → mAh */

    snprintf(p,sizeof(p),"/sys/class/power_supply/%s/status",bat);
    memset(v,0,sizeof(v)); fp_read_file(p,v,sizeof(v));
    fp->is_on_battery = (strncmp(v,"Discharging",11)==0);
}

/* L7: Hypervisor on Linux */
static void linux_collect_hypervisor(dsco_fingerprint_t *fp) {
    /* /proc/cpuinfo hypervisor flag */
    char flags[4096]={0};
    linux_cpuinfo("flags",flags,sizeof(flags));
    if (!strstr(flags,"hypervisor")) {
        linux_cpuinfo("Features",flags,sizeof(flags));
    }
    if (strstr(flags,"hypervisor")) fp->is_virtual = true;

    /* DMI product name */
    char prod[64]={0};
    linux_sysfs("/sys/class/dmi/id/product_name",prod,sizeof(prod));
    if (strstr(prod,"VMware"))        { fp->is_virtual=true; fp->hypervisor=DSCO_HV_VMWARE; }
    else if (strstr(prod,"VirtualBox")){ fp->is_virtual=true; fp->hypervisor=DSCO_HV_VIRTUALBOX; }
    else if (strstr(prod,"KVM"))      { fp->is_virtual=true; fp->hypervisor=DSCO_HV_KVM; }
    else if (strstr(prod,"HVM"))      { fp->is_virtual=true; fp->hypervisor=DSCO_HV_XEN; }
    else if (strstr(prod,"QEMU"))     { fp->is_virtual=true; fp->hypervisor=DSCO_HV_QEMU; }

    /* /proc/xen */
    if (access("/proc/xen",F_OK)==0)  { fp->is_virtual=true; fp->hypervisor=DSCO_HV_XEN; }

    /* x86 CPUID if available */
#if defined(DSCO_FP_ARCH_X64) || defined(DSCO_FP_ARCH_X86)
    {
        uint32_t a,b,c,d;
        dsco_cpuid(1,a,b,c,d);
        if ((c>>31)&1) fp->is_virtual = true; /* hypervisor present bit */
        if (fp->is_virtual) {
            dsco_cpuid(0x40000000,a,b,c,d);
            char hvs[13]={0};
            memcpy(hvs+0,&b,4); memcpy(hvs+4,&c,4); memcpy(hvs+8,&d,4);
            fp_trim(fp->hypervisor_vendor,sizeof(fp->hypervisor_vendor),hvs);
        }
    }
#endif

    /* TPM */
    fp->tpm_present = (access("/sys/class/tpm/tpm0",F_OK)==0);
    if (fp->tpm_present) {
        char tver[8]={0};
        linux_sysfs("/sys/class/tpm/tpm0/tpm_version_major",tver,sizeof(tver));
        if (!tver[0]) linux_sysfs("/sys/class/tpm/tpm0/device/description",tver,sizeof(tver));
        snprintf(fp->tpm_version,sizeof(fp->tpm_version),"2.0"); /* default modern */
        linux_sysfs("/sys/class/tpm/tpm0/device/manufacturer",
                    fp->tpm_manufacturer,sizeof(fp->tpm_manufacturer));
    }
}

/* Linux top-level */
static void collect_linux(dsco_fingerprint_t *fp) {
    snprintf(fp->platform_tag, sizeof(fp->platform_tag), "linux-%s",
#if defined(DSCO_FP_ARCH_ARM64)
             "arm64"
#elif defined(DSCO_FP_ARCH_X64)
             "x64"
#else
             "x86"
#endif
    );
    linux_collect_cpu(fp);
    linux_collect_identity(fp);
    linux_collect_network(fp);
    linux_collect_storage(fp);
    linux_collect_firmware(fp);
    linux_collect_battery(fp);
    linux_collect_hypervisor(fp);
#if defined(DSCO_FP_ARCH_X64) || defined(DSCO_FP_ARCH_X86)
    /* supplement with CPUID if we have x86 silicon info */
    if (!fp->cpu_brand[0]) collect_cpu_x86(fp);
#endif
}

#endif /* DSCO_FP_LINUX */

/* ─────────────────────────────────────────────────────────────────────────
 * ██████  Windows implementation
 * ───────────────────────────────────────────────────────────────────────── */
#if defined(DSCO_FP_WINDOWS)

/* Read a REG_SZ registry value into out[cap]. */
static bool win_reg_str(HKEY root, const wchar_t *subkey, const wchar_t *name,
                        char *out, size_t cap) {
    out[0] = '\0';
    HKEY hk;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    wchar_t buf[512] = {0};
    DWORD sz = sizeof(buf);
    DWORD type;
    bool ok = (RegQueryValueExW(hk, name, NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS &&
               type == REG_SZ);
    RegCloseKey(hk);
    if (ok) WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, (int)cap, NULL, NULL);
    return ok;
}

/* Read a DWORD */
static bool win_reg_dword(HKEY root, const wchar_t *subkey, const wchar_t *name, DWORD *out) {
    *out = 0;
    HKEY hk;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    DWORD sz = sizeof(DWORD), type;
    bool ok = (RegQueryValueExW(hk, name, NULL, &type, (LPBYTE)out, &sz) == ERROR_SUCCESS &&
               type == REG_DWORD);
    RegCloseKey(hk);
    return ok;
}

/* L2: Identity on Windows */
static void win_collect_identity(dsco_fingerprint_t *fp) {
    /* MachineGuid — unique per OS install, very stable */
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Cryptography",
                L"MachineGuid",
                fp->windows_machine_guid, sizeof(fp->windows_machine_guid));

    /* ProductId */
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                L"ProductId",
                fp->windows_product_id, sizeof(fp->windows_product_id));

    /* Hardware Profile GUID */
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001",
                L"HwProfileGuid",
                fp->windows_hwprofile_guid, sizeof(fp->windows_hwprofile_guid));

    /* InstallDate as proxy */
    DWORD idate = 0;
    if (win_reg_dword(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      L"InstallDate", &idate))
        snprintf(fp->windows_install_date, sizeof(fp->windows_install_date),
                 "%lu", (unsigned long)idate);

    /* Cross-platform aliases */
    if (fp->windows_machine_guid[0]) {
        snprintf(fp->efi_guid,       sizeof(fp->efi_guid),       "%s", fp->windows_machine_guid);
        snprintf(fp->platform_uuid,  sizeof(fp->platform_uuid),  "%s", fp->windows_machine_guid);
    }
    if (fp->windows_install_date[0] && !fp->serial_number[0])
        snprintf(fp->serial_number, sizeof(fp->serial_number), "%s", fp->windows_product_id);

    /* Baseboard serial via registry (requires SYSTEM or WMI) */
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\BIOS",
                L"BaseBoardProduct",
                fp->machine_model, sizeof(fp->machine_model));
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\BIOS",
                L"SystemProductName",
                fp->product_name, sizeof(fp->product_name));
    if (!fp->machine_model[0] && fp->product_name[0])
        snprintf(fp->machine_model, sizeof(fp->machine_model), "%s", fp->product_name);
}

/* L1: CPU on Windows */
static void win_collect_cpu(dsco_fingerprint_t *fp) {
    /* Brand string from registry (always present) */
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                L"ProcessorNameString",
                fp->cpu_brand, sizeof(fp->cpu_brand));
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                L"VendorIdentifier",
                fp->cpu_vendor, sizeof(fp->cpu_vendor));

    /* Logical cores */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    fp->cores_logical  = (int)si.dwNumberOfProcessors;
    fp->cores_physical = fp->cores_logical;
    fp->page_size_bytes= (uint64_t)si.dwPageSize;

    /* Physical cores via GetLogicalProcessorInformation */
    {
        DWORD sz = 0;
        GetLogicalProcessorInformation(NULL, &sz);
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)malloc(sz);
        if (buf && GetLogicalProcessorInformation(buf, &sz)) {
            DWORD n = sz / sizeof(*buf);
            int phys = 0;
            for (DWORD i = 0; i < n; i++)
                if (buf[i].Relationship == RelationProcessorCore) phys++;
            fp->cores_physical = phys;
        }
        free(buf);
    }

    /* Memory */
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        fp->mem_total_bytes     = (uint64_t)ms.ullTotalPhys;
        fp->mem_available_bytes = (uint64_t)ms.ullAvailPhys;
    }

    /* Cache topology */
    {
        DWORD sz = 0;
        GetLogicalProcessorInformationEx(RelationCache, NULL, &sz);
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)malloc(sz);
        if (buf && GetLogicalProcessorInformationEx(RelationCache, buf, &sz)) {
            uint8_t *p = (uint8_t*)buf;
            uint8_t *end = p + sz;
            while (p < end) {
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e =
                    (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)p;
                if (e->Relationship == RelationCache) {
                    CACHE_RELATIONSHIP *cr = &e->Cache;
                    switch(cr->Level) {
                    case 1:
                        if (cr->Type == CacheData)
                            fp->cache.l1d_bytes = cr->CacheSize;
                        else if (cr->Type == CacheInstruction)
                            fp->cache.l1i_bytes = cr->CacheSize;
                        fp->cache.cache_line_bytes = cr->LineSize;
                        break;
                    case 2: fp->cache.l2_bytes = cr->CacheSize; break;
                    case 3: fp->cache.l3_bytes = cr->CacheSize; break;
                    }
                }
                p += e->Size;
            }
        }
        free(buf);
    }

    /* CPUID features */
#if defined(DSCO_FP_ARCH_X64) || defined(DSCO_FP_ARCH_X86)
    collect_cpu_x86(fp);
#elif defined(DSCO_FP_ARCH_ARM64)
    /* Windows ARM64: use IsProcessorFeaturePresent */
    fp->features.neon = IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE);
    fp->features.aes  = IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE);
    fp->features.sha  = fp->features.aes;
#endif
}

/* L4: Network on Windows */
static void win_collect_network(dsco_fingerprint_t *fp) {
    ULONG sz = 0;
    GetAdaptersAddresses(AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
        NULL, NULL, &sz);
    IP_ADAPTER_ADDRESSES *buf = (IP_ADAPTER_ADDRESSES*)malloc(sz);
    if (!buf) return;
    if (GetAdaptersAddresses(AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,
        NULL, buf, &sz) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = buf; a; a = a->Next) {
            if (a->PhysicalAddressLength != 6) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (a->IfType == IF_TYPE_TUNNEL) continue;
            /* Prefer physical ethernet/wifi over virtual */
            uint8_t *m = a->PhysicalAddress;
            if (!m[0]&&!m[1]&&!m[2]) continue;
            snprintf(fp->primary_mac, sizeof(fp->primary_mac),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     m[0],m[1],m[2],m[3],m[4],m[5]);
            WideCharToMultiByte(CP_UTF8,0,a->FriendlyName,-1,
                                fp->iface_name,sizeof(fp->iface_name),NULL,NULL);
            fp->mac_addr_assign_type = 0;
            break;
        }
    }
    free(buf);
}

/* L3: Storage on Windows — GetVolumeInformation for C: */
static void win_collect_storage(dsco_fingerprint_t *fp) {
    WCHAR volname[MAX_PATH], fsname[MAX_PATH];
    DWORD serial_no, maxlen, flags;
    if (GetVolumeInformationW(L"C:\\", volname, MAX_PATH, &serial_no,
                              &maxlen, &flags, fsname, MAX_PATH)) {
        snprintf(fp->primary_volume_uuid, sizeof(fp->primary_volume_uuid),
                 "%08lX", (unsigned long)serial_no);
        WideCharToMultiByte(CP_UTF8,0,fsname,-1,
                            fp->storage_protocol,sizeof(fp->storage_protocol),NULL,NULL);
    }
    /* Disk model via DeviceIoControl — IOCTL_STORAGE_QUERY_PROPERTY */
    HANDLE h = CreateFileW(L"\\\\.\\PhysicalDrive0", 0,
                           FILE_SHARE_READ|FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        struct { STORAGE_PROPERTY_QUERY q; } req = {
            .q = {StorageDeviceProperty, PropertyStandardQuery}
        };
        BYTE resp[512] = {0};
        DWORD ret;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &req, sizeof(req), resp, sizeof(resp), &ret, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR*)resp;
            if (desc->ProductIdOffset)
                fp_trim(fp->storage_model,sizeof(fp->storage_model),
                        (char*)resp + desc->ProductIdOffset);
            if (desc->SerialNumberOffset)
                fp_trim(fp->storage_serial,sizeof(fp->storage_serial),
                        (char*)resp + desc->SerialNumberOffset);
            if (desc->ProductRevisionOffset)
                fp_trim(fp->storage_firmware,sizeof(fp->storage_firmware),
                        (char*)resp + desc->ProductRevisionOffset);
        }
        CloseHandle(h);
    }
    /* Capacity */
    ULARGE_INTEGER total, avail, free_b;
    if (GetDiskFreeSpaceExW(L"C:\\", &avail, &total, &free_b))
        fp->storage_capacity_bytes = total.QuadPart;
}

/* L5: Firmware on Windows */
static void win_collect_firmware(dsco_fingerprint_t *fp) {
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\BIOS",
                L"BIOSVersion",
                fp->bios_version, sizeof(fp->bios_version));
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\BIOS",
                L"BIOSReleaseDate",
                fp->bios_release_date, sizeof(fp->bios_release_date));
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\BIOS",
                L"BIOSVendor",
                fp->bios_vendor, sizeof(fp->bios_vendor));
    if (!fp->firmware_version[0])
        snprintf(fp->firmware_version, sizeof(fp->firmware_version),
                 "%s", fp->bios_version);
    /* UEFI vs legacy */
    {
        FIRMWARE_TYPE ft; BOOL ok;
        /* GetFirmwareType requires Windows 8+ */
        typedef BOOL (WINAPI *PFN)(PFIRMWARE_TYPE);
        PFN fn = (PFN)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetFirmwareType");
        if (fn && fn(&ft)) fp->uefi_mode = (ft == FirmwareTypeUefi);
    }
    /* Secure boot */
    DWORD sb = 0;
    if (win_reg_dword(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                      L"UEFISecureBootEnabled", &sb))
        fp->secure_boot_enabled = (sb != 0);
}

/* L6: Battery on Windows */
static void win_collect_battery(dsco_fingerprint_t *fp) {
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        fp->is_on_battery = (ps.ACLineStatus == 0);
        if (ps.BatteryLifePercent <= 100)
            fp->battery_health_pct = (float)ps.BatteryLifePercent;
    }
    /* Cycle count via WMI — too heavy for fingerprinting; skip */
}

/* L7: Hypervisor on Windows */
static void win_collect_hypervisor(dsco_fingerprint_t *fp) {
#if defined(DSCO_FP_ARCH_X64) || defined(DSCO_FP_ARCH_X86)
    uint32_t a,b,c,d;
    dsco_cpuid(1,a,b,c,d);
    if ((c>>31)&1) {
        fp->is_virtual = true;
        dsco_cpuid(0x40000000,a,b,c,d);
        char hvs[13]={0};
        memcpy(hvs+0,&b,4); memcpy(hvs+4,&c,4); memcpy(hvs+8,&d,4);
        fp_trim(fp->hypervisor_vendor,sizeof(fp->hypervisor_vendor),hvs);
        if (strstr(hvs,"VMwareVMware"))  fp->hypervisor=DSCO_HV_VMWARE;
        else if (strstr(hvs,"VBoxVBox")) fp->hypervisor=DSCO_HV_VIRTUALBOX;
        else if (strstr(hvs,"Microsoft"))fp->hypervisor=DSCO_HV_HYPERV;
        else if (strstr(hvs,"KVMKVMKVM"))fp->hypervisor=DSCO_HV_KVM;
        else if (strstr(hvs,"prl hyperv"))fp->hypervisor=DSCO_HV_PARALLELS;
        else fp->hypervisor=DSCO_HV_UNKNOWN;
    }
#endif
    /* TPM */
    {
        typedef BOOL (WINAPI *PFN_TISI)(VOID);
        PFN_TISI fn=(PFN_TISI)GetProcAddress(
            GetModuleHandleW(L"tbs.dll"),"Tbsi_Is_TPM_Present");
        if (!fn) {
            HMODULE h=LoadLibraryW(L"tbs.dll");
            if (h) fn=(PFN_TISI)GetProcAddress(h,"Tbsi_Is_TPM_Present");
        }
        if (fn) fp->tpm_present = fn();
        if (fp->tpm_present)
            snprintf(fp->tpm_version,sizeof(fp->tpm_version),"2.0");
    }
}

/* L8: OS info on Windows */
static void win_collect_os(dsco_fingerprint_t *fp) {
    snprintf(fp->os_name, sizeof(fp->os_name), "windows");
    /* Version from registry */
    char build[16]={0};
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                L"CurrentBuildNumber", build, sizeof(build));
    snprintf(fp->os_build, sizeof(fp->os_build), "%s", build);

    char prod[64]={0};
    win_reg_str(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                L"ProductName", prod, sizeof(prod));
    snprintf(fp->os_edition, sizeof(fp->os_edition), "%s", prod);
    snprintf(fp->os_version, sizeof(fp->os_version), "%s (build %s)", prod, build);

    /* Hostname */
    WCHAR host[MAX_COMPUTERNAME_LENGTH+1]={0};
    DWORD hsz=MAX_COMPUTERNAME_LENGTH+1;
    if (GetComputerNameW(host,&hsz))
        WideCharToMultiByte(CP_UTF8,0,host,-1,fp->hostname,sizeof(fp->hostname),NULL,NULL);

    /* Username */
    WCHAR user[256]={0}; DWORD usz=256;
    if (GetUserNameW(user,&usz))
        WideCharToMultiByte(CP_UTF8,0,user,-1,fp->username,sizeof(fp->username),NULL,NULL);
}

/* Windows top-level */
static void collect_windows(dsco_fingerprint_t *fp) {
    snprintf(fp->platform_tag, sizeof(fp->platform_tag), "win-%s",
#if defined(DSCO_FP_ARCH_ARM64)
             "arm64"
#elif defined(DSCO_FP_ARCH_X64)
             "x64"
#else
             "x86"
#endif
    );
    win_collect_os(fp);
    win_collect_cpu(fp);
    win_collect_identity(fp);
    win_collect_network(fp);
    win_collect_storage(fp);
    win_collect_firmware(fp);
    win_collect_battery(fp);
    win_collect_hypervisor(fp);
}

#endif /* DSCO_FP_WINDOWS */

/* ─────────────────────────────────────────────────────────────────────────
 * Boot time — cross-platform
 * ───────────────────────────────────────────────────────────────────────── */
static int64_t fp_boot_time(void) {
#if defined(DSCO_FP_MACOS)
    struct timeval tv = {0,0};
    size_t sz = sizeof(tv);
    if (sysctlbyname("kern.boottime",&tv,&sz,NULL,0)==0) return (int64_t)tv.tv_sec;
#elif defined(DSCO_FP_LINUX)
    struct sysinfo si;
    if (sysinfo(&si)==0) return (int64_t)time(NULL)-(int64_t)si.uptime;
#elif defined(DSCO_FP_WINDOWS)
    return (int64_t)(time(NULL) - GetTickCount64()/1000);
#endif
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * OS / hostname / username (POSIX path)
 * ───────────────────────────────────────────────────────────────────────── */
#if !defined(DSCO_FP_WINDOWS)
static void fp_collect_posix_os(dsco_fingerprint_t *fp) {
    struct utsname u;
    if (uname(&u)==0) {
        fp_trim(fp->os_name,     sizeof(fp->os_name),     u.sysname);
        fp_trim(fp->os_version,  sizeof(fp->os_version),  u.release);
        fp_trim(fp->kernel_build,sizeof(fp->kernel_build), u.version);
        fp_trim(fp->hostname,    sizeof(fp->hostname),     u.nodename);
        fp_trim(fp->arch,        sizeof(fp->arch),         u.machine);
        for (char *p=fp->os_name;*p;p++) *p=(char)tolower((unsigned char)*p);
    }
    /* username */
    struct passwd *pw = getpwuid(getuid());
    if (pw) fp_trim(fp->username,sizeof(fp->username),pw->pw_name);
}
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * Signal bitmask update
 * ───────────────────────────────────────────────────────────────────────── */
static void fp_update_signals(dsco_fingerprint_t *fp) {
    dsco_fp_signals_t s = 0;
    if (fp_nonempty(fp->serial_number))         s |= DSCO_FPS_SERIAL;
    if (fp_nonempty(fp->platform_uuid) ||
        fp_nonempty(fp->machine_id))            s |= DSCO_FPS_UUID;
    if (fp_nonempty(fp->board_id))              s |= DSCO_FPS_BOARD_ID;
    if (fp_nonempty(fp->mlb)||
        fp_nonempty(fp->baseboard_serial))      s |= DSCO_FPS_MLB;
    if (fp_nonempty(fp->efi_guid)||
        fp_nonempty(fp->windows_machine_guid))  s |= DSCO_FPS_EFI_GUID;
    if (fp_nonempty(fp->cpu_brand))             s |= DSCO_FPS_CPU;
    if (fp_nonempty(fp->machine_model))         s |= DSCO_FPS_MODEL;
    if (fp_nonempty(fp->primary_mac))           s |= DSCO_FPS_MAC;
    if (fp_nonempty(fp->storage_model)||
        fp_nonempty(fp->storage_serial))        s |= DSCO_FPS_STORAGE;
    if (fp_nonempty(fp->battery_serial))        s |= DSCO_FPS_BATTERY;
    if (fp->tpm_present)                        s |= DSCO_FPS_TPM;
    if (fp_nonempty(fp->gpu_model)||
        fp->gpu_core_count > 0)                 s |= DSCO_FPS_GPU;
    if (fp->is_virtual)                         s |= DSCO_FPS_HYPERVISOR;
    if (fp_nonempty(fp->bios_version)||
        fp_nonempty(fp->firmware_version))      s |= DSCO_FPS_BIOS;
    if (fp_nonempty(fp->primary_volume_uuid))   s |= DSCO_FPS_VOLUME_UUID;
    fp->signals_present = s;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Composite ID computation
 * ───────────────────────────────────────────────────────────────────────── */
static void fp_compute_ids(dsco_fingerprint_t *fp) {
    sha256_ctx_t s;

    /* hw_id = SHA-256(TIER_A only) — max stability */
    sha256_init(&s);
    uint8_t ta = 0;
#define HA(F) do { if(fp_nonempty(fp->F)){ \
    sha256_update(&s,(const uint8_t*)fp->F,strlen(fp->F)); \
    sha256_update(&s,(const uint8_t*)"|",1); ta++; } } while(0)
    HA(serial_number); HA(platform_uuid); HA(board_id); HA(mlb);
    HA(efi_guid); HA(machine_id); HA(windows_machine_guid);
    /* TPM EK hash if available */
    if (fp_nonempty(fp->tpm_ekpub_hash)) { HA(tpm_ekpub_hash); }
#undef HA
    fp->tier_a_count = ta;
    uint8_t h[32]; sha256_final(&s, h); hex_encode(h,32,fp->hw_id); fp->hw_id[64]='\0';

    /* composite_id = SHA-256(TIER_A||TIER_B) */
    sha256_init(&s);
    /* re-hash tier_a */
#define HA2(F) do { if(fp_nonempty(fp->F)){ \
    sha256_update(&s,(const uint8_t*)fp->F,strlen(fp->F)); \
    sha256_update(&s,(const uint8_t*)"|",1); } } while(0)
    HA2(serial_number); HA2(platform_uuid); HA2(board_id); HA2(mlb);
    HA2(efi_guid); HA2(machine_id); HA2(windows_machine_guid);
    if (fp_nonempty(fp->tpm_ekpub_hash)) HA2(tpm_ekpub_hash);
#undef HA2
    /* tier_b */
    uint8_t tb = 0;
#define HB(F) do { if(fp_nonempty(fp->F)){ \
    sha256_update(&s,(const uint8_t*)fp->F,strlen(fp->F)); \
    sha256_update(&s,(const uint8_t*)"||",2); tb++; } } while(0)
    HB(cpu_brand); HB(cpu_vendor); HB(machine_model); HB(arch);
    HB(primary_mac); HB(storage_model); HB(storage_serial); HB(battery_serial);
    HB(windows_product_id); HB(windows_install_date);
#undef HB
    fp->tier_b_count = tb;
    sha256_final(&s, h); hex_encode(h,32,fp->composite_id); fp->composite_id[64]='\0';
    memcpy(fp->fingerprint_id, fp->composite_id, 65);

    /* session_id = SHA-256(composite_id || boot_time) */
    char bstr[24]; snprintf(bstr,sizeof(bstr),"%lld",(long long)fp->boot_time);
    sha256_init(&s);
    sha256_update(&s,(const uint8_t*)fp->composite_id,64);
    sha256_update(&s,(const uint8_t*)"|",1);
    sha256_update(&s,(const uint8_t*)bstr,strlen(bstr));
    sha256_final(&s,h); hex_encode(h,32,fp->session_id); fp->session_id[64]='\0';

    /* Confidence */
    uint8_t conf = 0;
    if (ta >= 4) conf = 65; else if (ta >= 2) conf = 45;
    else if (ta >= 1) conf = 25; else conf = 0;
    if (tb >= 5) conf += 35; else if (tb >= 3) conf += 25;
    else if (tb >= 1) conf += 10;
    if (conf > 100) conf = 100;
    fp->confidence = conf;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Public API
 * ───────────────────────────────────────────────────────────────────────── */
int dsco_fingerprint_refresh(void) {
    memset(&g_fp, 0, sizeof(g_fp));
    g_fp.collected_at = (int64_t)time(NULL);
    g_fp.boot_time    = fp_boot_time();

#if defined(DSCO_FP_MACOS)
    fp_collect_posix_os(&g_fp);
    collect_macos(&g_fp);
#elif defined(DSCO_FP_LINUX)
    fp_collect_posix_os(&g_fp);
    collect_linux(&g_fp);
#elif defined(DSCO_FP_WINDOWS)
    snprintf(g_fp.arch, sizeof(g_fp.arch),
#if defined(DSCO_FP_ARCH_ARM64)
             "arm64"
#elif defined(DSCO_FP_ARCH_X64)
             "x86_64"
#else
             "x86"
#endif
    );
    collect_windows(&g_fp);
#else
    /* Generic POSIX fallback */
    fp_collect_posix_os(&g_fp);
    g_fp.cores_logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
    g_fp.mem_total_bytes = (uint64_t)sysconf(_SC_PHYS_PAGES) *
                           (uint64_t)sysconf(_SC_PAGESIZE);
    snprintf(g_fp.platform_tag, sizeof(g_fp.platform_tag), "posix-generic");
#endif

    collect_runtime(&g_fp);
    fp_update_signals(&g_fp);
    fp_compute_ids(&g_fp);
    g_collected = true;
    return 0;
}

const dsco_fingerprint_t *dsco_fingerprint_get(void) {
    if (!g_collected) dsco_fingerprint_refresh();
    return &g_fp;
}

const char *dsco_hv_name(dsco_hv_type_t hv) {
    switch(hv) {
    case DSCO_HV_VMWARE:    return "VMware";
    case DSCO_HV_VIRTUALBOX:return "VirtualBox";
    case DSCO_HV_HYPERV:    return "Hyper-V";
    case DSCO_HV_KVM:       return "KVM";
    case DSCO_HV_QEMU:      return "QEMU";
    case DSCO_HV_XEN:       return "Xen";
    case DSCO_HV_APPLE_HV:  return "Apple Hypervisor";
    case DSCO_HV_PARALLELS: return "Parallels";
    case DSCO_HV_BHYVE:     return "bhyve";
    case DSCO_HV_UNKNOWN:   return "unknown";
    default:                return "none";
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * JSON serialisation
 * ───────────────────────────────────────────────────────────────────────── */
static int fp_json_escape(const char *s, char *out, size_t cap) {
    if (!s) { if(cap) out[0]='\0'; return 0; }
    size_t i=0;
    while (*s && i<cap-2) {
        unsigned char c=(unsigned char)*s++;
        if(c=='"'||c=='\\'){out[i++]='\\';out[i++]=(char)c;}
        else if(c=='\n'){out[i++]='\\';out[i++]='n';}
        else if(c=='\r'){out[i++]='\\';out[i++]='r';}
        else if(c=='\t'){out[i++]='\\';out[i++]='t';}
        else if(c<0x20){/* skip */ }
        else out[i++]=(char)c;
    }
    if(i<cap) out[i]='\0';
    return (int)i;
}
#define _JS(K,V) do { char _e[512]; fp_json_escape((V),_e,sizeof(_e)); \
    int _n=snprintf(p,end-p,"%s\"%s\":\"%s\"",first?"":",",K,_e); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)
#define _JI(K,V) do { int _n=snprintf(p,end-p,"%s\"%s\":%lld",first?"":",",K,(long long)(V)); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)
#define _JU(K,V) do { int _n=snprintf(p,end-p,"%s\"%s\":%llu",first?"":",",K,(unsigned long long)(V)); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)
#define _JB(K,V) do { int _n=snprintf(p,end-p,"%s\"%s\":%s",first?"":",",K,(V)?"true":"false"); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)
#define _JF(K,V) do { int _n=snprintf(p,end-p,"%s\"%s\":%.2f",first?"":",",K,(double)(V)); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)
#define _SEP(S) do { int _n=snprintf(p,end-p,"%s",S); if(_n<0||_n>=end-p) goto done; p+=_n; } while(0)

size_t dsco_fingerprint_to_json(const dsco_fingerprint_t *fp,
                                bool include_pii, bool include_all,
                                char *out, size_t out_cap) {
    if (!fp||!out||out_cap<16) return 0;
    char *p=out, *end=out+out_cap;
    bool first=true;
    _SEP("{");

    /* ── Always included ── */
    _JS("composite_id",    fp->composite_id);
    _JS("session_id",      fp->session_id);
    _JS("hw_id",           fp->hw_id);
    _JS("platform",        fp->platform_tag);
    _JS("os",              fp->os_name);
    _JS("os_version",      fp->os_version);
    _JS("arch",            fp->arch);
    _JS("cpu_brand",       fp->cpu_brand);
    _JS("cpu_vendor",      fp->cpu_vendor);
    _JS("machine_model",   fp->machine_model);
    _JI("cores_logical",   fp->cores_logical);
    _JI("cores_physical",  fp->cores_physical);
    _JI("perf_cores",      fp->perf_cores);
    _JI("eff_cores",       fp->efficiency_cores);
    _JI("numa_nodes",      fp->numa_nodes);
    _JU("mem_bytes",       fp->mem_total_bytes);
    _JU("page_bytes",      fp->page_size_bytes);
    _JI("ptr_bits",        fp->pointer_width);
    _JB("little_endian",   fp->is_little_endian);
    _JI("aslr_bits",       fp->aslr_entropy_bits);
    _JS("gpu_model",       fp->gpu_model);
    _JI("gpu_cores",       fp->gpu_core_count);
    _JB("is_virtual",      fp->is_virtual);
    _JS("hypervisor",      fp->is_virtual ? dsco_hv_name(fp->hypervisor) : "none");
    _JB("uefi",            fp->uefi_mode);
    _JB("secure_boot",     fp->secure_boot_enabled);
    _JB("tpm",             fp->tpm_present);
    _JS("tpm_version",     fp->tpm_present ? fp->tpm_version : "");
    _JI("confidence",      fp->confidence);
    _JI("tier_a",          fp->tier_a_count);
    _JI("tier_b",          fp->tier_b_count);
    _JU("signals",         (unsigned long long)fp->signals_present);
    _JI("ts",              fp->collected_at);
    _JI("boot_time",       fp->boot_time);

    /* CPU features array */
    {
        _SEP(",\"features\":["); bool ff=true;
#define _F(NAME,COND) do { if(COND){ int _n=snprintf(p,end-p,"%s\"%s\"",ff?"":",",NAME); \
    if(_n<0||_n>=end-p) goto done; p+=_n; ff=false; } } while(0)
        _F("neon",        fp->features.neon);
        _F("sve",         fp->features.sve);
        _F("sve2",        fp->features.sve2);
        _F("amx",         fp->features.amx);
        _F("bf16",        fp->features.bf16);
        _F("fp16",        fp->features.fp16);
        _F("i8mm",        fp->features.i8mm);
        _F("dotprod",     fp->features.dotprod);
        _F("sha512",      fp->features.sha512);
        _F("sha3",        fp->features.sha3);
        _F("lse",         fp->features.lse);
        _F("lse2",        fp->features.lse2);
        _F("lrcpc",       fp->features.lrcpc);
        _F("mte",         fp->features.mte);
        _F("pauth",       fp->features.pauth);
        _F("sse2",        fp->features.sse2);
        _F("sse4.1",      fp->features.sse4_1);
        _F("sse4.2",      fp->features.sse4_2);
        _F("avx",         fp->features.avx);
        _F("avx2",        fp->features.avx2);
        _F("avx512f",     fp->features.avx512f);
        _F("avx512bw",    fp->features.avx512bw);
        _F("avx512vl",    fp->features.avx512vl);
        _F("avx512vnni",  fp->features.avx512vnni);
        _F("avx512bf16",  fp->features.avx512bf16);
        _F("avx_vnni",    fp->features.avx_vnni);
        _F("fma",         fp->features.fma);
        _F("aes",         fp->features.aes);
        _F("sha",         fp->features.sha);
        _F("rdrand",      fp->features.rdrand);
        _F("rdseed",      fp->features.rdseed);
        _F("popcnt",      fp->features.popcnt);
        _F("vaes",        fp->features.vaes);
        _F("f16c",        fp->features.f16c);
        _F("bmi2",        fp->features.bmi2);
        _F("metal",       fp->features.metal);
        _F("neural_engine",fp->features.neural_engine);
        _F("secure_enclave",fp->features.secure_enclave);
        _F("touchid",     fp->features.touchid);
        _F("avx10",       fp->features.intel_avx10);
#undef _F
        _SEP("]");
    }

    /* Cache — use local shadow of 'first' so sub-object commas are independent */
    {
        bool _cf = true;
#undef  _JU
#define _JU(K,V) do { int _n=snprintf(p,end-p,"%s\"%s\":%llu",_cf?"":",",K,(unsigned long long)(V)); \
    if(_n<0||_n>=end-p) goto done; p+=_n; _cf=false; } while(0)
        _SEP(",\"cache\":{");
        _JU("l1d",       fp->cache.l1d_bytes);
        _JU("l1i",       fp->cache.l1i_bytes);
        _JU("l2",        fp->cache.l2_bytes);
        _JU("l3",        fp->cache.l3_bytes);
        _JU("line",      fp->cache.cache_line_bytes);
        if (fp->cache.perf_l2_bytes) _JU("perf_l2", fp->cache.perf_l2_bytes);
        if (fp->cache.eff_l2_bytes)  _JU("eff_l2",  fp->cache.eff_l2_bytes);
        _SEP("}");
#undef  _JU
#define _JU(K,V) do { int _n=snprintf(p,end-p,"%s\"%s\":%llu",first?"":",",K,(unsigned long long)(V)); \
    if(_n<0||_n>=end-p) goto done; p+=_n; first=false; } while(0)
    }

    /* PII block */
    if (include_pii || include_all) {
        _JS("serial_number",         fp->serial_number);
        _JS("platform_uuid",         fp->platform_uuid);
        _JS("board_id",              fp->board_id);
        _JS("mlb",                   fp->mlb);
        _JS("efi_guid",              fp->efi_guid);
        _JS("machine_id",            fp->machine_id);
        _JS("provisioning_udid",     fp->provisioning_udid);
        _JS("windows_machine_guid",  fp->windows_machine_guid);
        _JS("windows_product_id",    fp->windows_product_id);
        _JS("windows_hwprofile_guid",fp->windows_hwprofile_guid);
        _JS("primary_mac",           fp->primary_mac);
        _JS("wifi_mac",              fp->wifi_mac);
        _JS("iface_name",            fp->iface_name);
        _JI("mac_assign_type",       fp->mac_addr_assign_type);
        _JS("battery_serial",        fp->battery_serial);
    }

    /* Full detail block */
    if (include_all) {
        _JS("cpu_microarch",         fp->cpu_microarch);
        _JS("cpu_stepping",          fp->cpu_stepping_str);
        _JI("cpuid_family",          fp->cpuid_family);
        _JI("cpuid_model",           fp->cpuid_model);
        _JS("storage_model",         fp->storage_model);
        _JS("storage_serial",        fp->storage_serial);
        _JS("storage_protocol",      fp->storage_protocol);
        _JS("storage_firmware",      fp->storage_firmware);
        _JS("primary_volume_uuid",   fp->primary_volume_uuid);
        _JU("storage_capacity",      fp->storage_capacity_bytes);
        _JS("firmware_version",      fp->firmware_version);
        _JS("bios_vendor",           fp->bios_vendor);
        _JS("bios_version",          fp->bios_version);
        _JS("os_loader_version",     fp->os_loader_version);
        _JS("smc_version",           fp->smc_version);
        _JI("battery_cycles",        fp->battery_cycle_count);
        _JI("battery_design_mah",    fp->battery_design_cap_mah);
        _JF("battery_health_pct",    fp->battery_health_pct);
        _JB("on_battery",            fp->is_on_battery);
        _JS("hypervisor_vendor",     fp->hypervisor_vendor);
        _JS("hypervisor_uuid",       fp->hypervisor_uuid);
        _JS("tpm_manufacturer",      fp->tpm_manufacturer);
        _JU("gpu_vram_bytes",        fp->gpu_vram_bytes);
        _JS("kernel_build",          fp->kernel_build);
        _JS("os_build",              fp->os_build);
        _JS("os_edition",            fp->os_edition);
        _JS("hostname",              fp->hostname);
        _JS("username",              fp->username);
        _JI("display_count",         fp->display_count);
        _JB("nx_enabled",            fp->nx_enabled);
    }

    if(p+2<end){*p++='}';*p='\0';}
done:
    return (size_t)(p-out);
}
#undef _JS
#undef _JI
#undef _JU
#undef _JB
#undef _JF
#undef _SEP

/* ─────────────────────────────────────────────────────────────────────────
 * Summary + stability report
 * ───────────────────────────────────────────────────────────────────────── */
size_t dsco_fingerprint_summary(const dsco_fingerprint_t *fp, char *out, size_t cap) {
    if (!fp||!out) return 0;
    int n = snprintf(out, cap,
        "%s [%s] · %s · %dP+%dE cores · %.1f GB RAM · %s"
        " · conf:%u%% · id:%.12s",
        fp->cpu_brand[0]      ? fp->cpu_brand      : "?",
        fp->arch[0]           ? fp->arch            : "?",
        fp->machine_model[0]  ? fp->machine_model   : "?",
        fp->perf_cores ? fp->perf_cores : fp->cores_logical,
        fp->efficiency_cores,
        (double)fp->mem_total_bytes/(1024.0*1024.0*1024.0),
        fp->is_virtual ? dsco_hv_name(fp->hypervisor) : "bare-metal",
        fp->confidence,
        fp->composite_id);
    return (n<0) ? 0 : (size_t)n;
}

size_t dsco_fingerprint_stability_report(const dsco_fingerprint_t *fp,
                                          char *out, size_t cap) {
    if (!fp||!out) return 0;
    dsco_fp_signals_t s = fp->signals_present;
    int n = snprintf(out, cap,
        "Fingerprint Stability Report [%s]\n"
        "  composite_id : %.20s...  (conf %u%%)\n"
        "  session_id   : %.20s...\n"
        "  hw_id        : %.20s...\n"
        "\n"
        "  TIER_A (%u signals)\n"
        "    serial_number      : %s\n"
        "    platform_uuid/mid  : %s\n"
        "    board_id           : %s\n"
        "    mlb/baseboard_ser  : %s\n"
        "    efi_guid/win_guid  : %s\n"
        "    tpm_ekpub_hash     : %s\n"
        "\n"
        "  TIER_B (%u signals)\n"
        "    cpu_brand          : %s\n"
        "    machine_model      : %s\n"
        "    primary_mac        : %s\n"
        "    storage_model      : %s\n"
        "    storage_serial     : %s\n"
        "    battery_serial     : %s\n"
        "\n"
        "  Signals bitmask      : 0x%04X\n"
        "    hypervisor         : %s%s\n"
        "    TPM                : %s\n"
        "    UEFI               : %s\n"
        "    secure_boot        : %s\n"
        "  Collected: %lld  Boot: %lld\n",
        fp->platform_tag,
        fp->composite_id, fp->confidence,
        fp->session_id,
        fp->hw_id,
        fp->tier_a_count,
        (s&DSCO_FPS_SERIAL)    ? fp->serial_number : "✗",
        (s&DSCO_FPS_UUID)      ? (fp->platform_uuid[0]?fp->platform_uuid:fp->machine_id) : "✗",
        (s&DSCO_FPS_BOARD_ID)  ? fp->board_id : "✗",
        (s&DSCO_FPS_MLB)       ? (fp->mlb[0]?fp->mlb:fp->baseboard_serial) : "✗",
        (s&DSCO_FPS_EFI_GUID)  ? fp->efi_guid : "✗",
        fp_nonempty(fp->tpm_ekpub_hash) ? fp->tpm_ekpub_hash : "✗ (not collected)",
        fp->tier_b_count,
        fp_nonempty(fp->cpu_brand)      ? fp->cpu_brand      : "✗",
        fp_nonempty(fp->machine_model)  ? fp->machine_model  : "✗",
        fp_nonempty(fp->primary_mac)    ? fp->primary_mac    : "✗",
        fp_nonempty(fp->storage_model)  ? fp->storage_model  : "✗",
        fp_nonempty(fp->storage_serial) ? fp->storage_serial : "✗",
        fp_nonempty(fp->battery_serial) ? fp->battery_serial : "✗",
        (unsigned)fp->signals_present,
        fp->is_virtual ? dsco_hv_name(fp->hypervisor) : "none",
        fp->is_virtual ? "" : "",
        fp->tpm_present ? fp->tpm_version : "absent",
        fp->uefi_mode   ? "yes" : "legacy",
        fp->secure_boot_enabled ? "on" : "off",
        (long long)fp->collected_at,
        (long long)fp->boot_time);
    return (n<0) ? 0 : (size_t)n;
}
