/* harden.c — runtime anti-reverse-engineering hardening.
 *
 * Protective bodies compile in only under -DDSCO_HARDENED. Without it, every
 * entry point is a no-op so `make` stays fully debuggable. See harden.h.
 */
/* BSD types (u_int/u_char/u_short) used by <sys/sysctl.h>/<sys/proc.h> are
 * hidden under strict _POSIX_C_SOURCE; re-enable them on Apple. Must precede
 * every system header. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "harden.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef DSCO_HARDENED

#include <stdint.h>
#include <stdio.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
/* ptrace(PT_DENY_ATTACH) — declared in <sys/ptrace.h> but the constant is not
 * always exposed; declare the prototype ourselves to avoid header drift. */
extern int ptrace(int request, pid_t pid, caddr_t addr, int data);
#ifndef PT_DENY_ATTACH
#define PT_DENY_ATTACH 31
#endif
/* csops() is private but stable; used to read the process code-signing flags. */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_VALID 0x0000001  /* dynamically valid */
#define CS_HARD 0x0000100   /* don't load invalid pages */
#define CS_KILL 0x0000200   /* kill process if it becomes invalid */
#elif defined(__linux__)
#include <sys/ptrace.h>
#include <fcntl.h>
#include <ctype.h>
#endif

/* ── Termination: die without handing the attacker a signal to hook on ──────
 * No message, no distinctive abort — just a clean exit that looks like normal
 * completion, so an attacker can't grep logs or set a breakpoint on abort(). */
static void dsco_harden_die(void)
{
    /* Best-effort scrub of a scratch page so a core dump reveals little. */
    volatile char pad[64];
    for (size_t i = 0; i < sizeof(pad); i++)
        pad[i] = 0;
    _exit(0);
}

/* ── Layer 1: environment scrub ─────────────────────────────────────────────
 * Injectors and allocator-logging shims announce themselves via env. Wipe them
 * before any dependent library reads them. */
static const char *const kToxicEnv[] = {
    "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH",
    "DYLD_FORCE_FLAT_NAMESPACE", "DYLD_IMAGE_SUFFIX", "DYLD_PRINT_TO_FILE",
    "LD_PRELOAD", "LD_AUDIT", "LD_LIBRARY_PATH", "LD_PROFILE",
    "MallocStackLogging", "MallocStackLoggingNoCompact", "NSZombieEnabled",
    "MallocScribble", "DYLD_PRINT_LIBRARIES", "GODEBUG", NULL,
};

static void dsco_harden_scrub_env(void)
{
    for (int i = 0; kToxicEnv[i]; i++) {
        if (getenv(kToxicEnv[i]))
            unsetenv(kToxicEnv[i]);
    }
}

/* ── Layer 2: anti-debug ────────────────────────────────────────────────────*/
static void dsco_harden_deny_debugger(void)
{
#if defined(__APPLE__)
    /* Refuse all future debugger attaches for the life of the process. */
    ptrace(PT_DENY_ATTACH, 0, 0, 0);
#elif defined(__linux__)
    /* If a debugger is already attached, TRACEME fails; if not, it makes any
     * later attach fail. Either way the presence signal is actionable. */
    if (ptrace(PTRACE_TRACEME, 0, 0, 0) == -1)
        dsco_harden_die();
#endif
}

static bool dsco_harden_being_traced(void)
{
#if defined(__APPLE__)
    struct kinfo_proc info;
    size_t size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    memset(&info, 0, sizeof(info));
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0)
        return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#elif defined(__linux__)
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return false;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    const char *p = strstr(buf, "TracerPid:");
    if (!p)
        return false;
    p += 10;
    while (*p == ' ' || *p == '\t')
        p++;
    return (*p != '0');
#else
    return false;
#endif
}

/* ── Layer 3: anti-instrumentation (Frida / substrate / injected dylibs) ─────*/
static bool dsco_harden_instrumented(void)
{
#if defined(__APPLE__)
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name)
            continue;
        if (strstr(name, "frida") || strstr(name, "FridaGadget") ||
            strstr(name, "cynject") || strstr(name, "libsubstrate") ||
            strstr(name, "substitute") || strstr(name, "MobileSubstrate"))
            return true;
    }
    return false;
#elif defined(__linux__)
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0)
        return false;
    char buf[8192];
    ssize_t n;
    bool hit = false;
    /* Streaming scan; a boundary-split token is an acceptable miss. */
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (strstr(buf, "frida") || strstr(buf, "gum-js") ||
            strstr(buf, "gadget") || strstr(buf, "/tmp/re.")) {
            hit = true;
            break;
        }
    }
    close(fd);
    return hit;
#else
    return false;
#endif
}

/* ── Layer 4: code-signature self-check (macOS) ─────────────────────────────
 * A patched or re-signed binary loses CS_VALID; an unsigned one lacks the hard
 * flags. This catches in-place binary edits that anti-debug alone would miss. */
static bool dsco_harden_signature_bad(void)
{
#if defined(__APPLE__)
    uint32_t flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) != 0)
        return false; /* csops unavailable — do not false-positive */
    return (flags & CS_VALID) == 0;
#else
    return false;
#endif
}

void dsco_harden_init(void)
{
    dsco_harden_scrub_env();
    dsco_harden_deny_debugger();
    if (dsco_harden_being_traced() || dsco_harden_instrumented() ||
        dsco_harden_signature_bad())
        dsco_harden_die();
}

bool dsco_harden_checkpoint(void)
{
    if (dsco_harden_being_traced() || dsco_harden_instrumented())
        dsco_harden_die();
    return true;
}

bool dsco_harden_enabled(void) { return true; }

#else /* !DSCO_HARDENED — no-op stubs keep dev builds debuggable */

void dsco_harden_init(void) {}
bool dsco_harden_checkpoint(void) { return true; }
bool dsco_harden_enabled(void) { return false; }

#endif /* DSCO_HARDENED */
