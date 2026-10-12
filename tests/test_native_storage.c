/* Exercise the real native storage collector, trapping shell regressions. */
#define popen storage_test_forbidden_popen
#define system storage_test_forbidden_system
#include "../src/fingerprint.c"
#undef popen
#undef system
#include <assert.h>

static unsigned shell_calls;
FILE *storage_test_forbidden_popen(const char *command, const char *mode) {
    (void)command;
    (void)mode;
    shell_calls++;
    errno = EPERM;
    return NULL;
}
int storage_test_forbidden_system(const char *command) {
    (void)command;
    shell_calls++;
    errno = EPERM;
    return -1;
}

#if defined(DSCO_FP_MACOS)
static void require_string_matches(CFDictionaryRef description, CFStringRef key,
                                   const char *actual) {
    CFTypeRef expected = CFDictionaryGetValue(description, key);
    if (!expected) {
        assert(actual[0] == '\0');
        return;
    }
    assert(CFGetTypeID(expected) == CFStringGetTypeID());
    CFStringRef collected = CFStringCreateWithCString(kCFAllocatorDefault, actual,
                                                     kCFStringEncodingUTF8);
    assert(collected && CFEqual(expected, collected));
    CFRelease(collected);
}

static void test_native_values(void) {
    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    assert(session);
    CFURLRef root = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR("/"),
                                                 kCFURLPOSIXPathStyle, true);
    assert(root);
    DADiskRef disk = DADiskCreateFromVolumePath(kCFAllocatorDefault, session, root);
    assert(disk);
    CFDictionaryRef description = DADiskCopyDescription(disk);
    assert(description);

    dsco_fingerprint_t fp = {0};
    mac_collect_storage(&fp);
    require_string_matches(description, kDADiskDescriptionDeviceModelKey, fp.storage_model);
    require_string_matches(description, kDADiskDescriptionDeviceProtocolKey, fp.storage_protocol);
    CFTypeRef expected_uuid = CFDictionaryGetValue(description, kDADiskDescriptionVolumeUUIDKey);
    if (expected_uuid) {
        assert(CFGetTypeID(expected_uuid) == CFUUIDGetTypeID());
        CFStringRef text = CFStringCreateWithCString(kCFAllocatorDefault, fp.primary_volume_uuid,
                                                     kCFStringEncodingUTF8);
        assert(text);
        CFUUIDRef collected_uuid = CFUUIDCreateFromString(kCFAllocatorDefault, text);
        assert(collected_uuid && CFEqual(collected_uuid, expected_uuid));
        CFRelease(collected_uuid);
        CFRelease(text);
    } else {
        assert(!fp.primary_volume_uuid[0]);
    }
    /* Stability across repeated collections must retain composite identity. */
    fp_compute_ids(&fp);
    for (int i = 0; i < 20; i++) {
        dsco_fingerprint_t repeated = {0};
        mac_collect_storage(&repeated);
        fp_compute_ids(&repeated);
        assert(strcmp(fp.storage_model, repeated.storage_model) == 0);
        assert(strcmp(fp.hw_id, repeated.hw_id) == 0);
        assert(strcmp(fp.composite_id, repeated.composite_id) == 0);
    }
    CFRelease(description);
    CFRelease(disk);
    CFRelease(root);
    CFRelease(session);
}

static void test_skip_flags(void) {
    const char *names[] = {"DSCO_FAST_FINGERPRINT", "DSCO_INTERACTIVE"};
    const char *values[] = {"1", "0", ""};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
            dsco_fingerprint_t fp = {0};
            strcpy(fp.storage_model, "existing model");
            strcpy(fp.storage_protocol, "existing protocol");
            strcpy(fp.primary_volume_uuid, "existing volume");
            assert(setenv(names[i], values[j], 1) == 0);
            mac_collect_storage(&fp);
            assert(!fp.storage_model[0] && !fp.storage_protocol[0] && !fp.primary_volume_uuid[0]);
            assert(unsetenv(names[i]) == 0);
        }
    }
}
#endif

int main(void) {
#if defined(DSCO_FP_MACOS)
    unsetenv("DSCO_FAST_FINGERPRINT");
    unsetenv("DSCO_INTERACTIVE");
    test_native_values();
    test_skip_flags();
    assert(shell_calls == 0);
    puts("PASS: native storage matches DiskArbitration; identity stable; skip flags preserved; zero shell calls");
#else
    puts("SKIP: native storage collector requires macOS");
#endif
    return 0;
}
