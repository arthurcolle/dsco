#ifdef __APPLE__

#define _DARWIN_C_SOURCE 1

#include "se_store.h"
#include "audit_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonHMAC.h>
#include <IOKit/IOKitLib.h>

/* ── constants ──────────────────────────────────────────────────────────── */

#define SE_KEY_LABEL   "systems.distributed.dsco.master"
#define SE_KEY_TAG     "systems.distributed.dsco.master.v1"
#define KC_SERVICE     "systems.distributed.dsco.secure-store.v2"
#define KC_ACCOUNT     "master.v2"
#define ECDH_KEY_SIZE  32

static inline void zero_buf(void *p, size_t n) {
    volatile uint8_t *q = (volatile uint8_t *)p;
    for (size_t i = 0; i < n; i++) q[i] = 0;
}

static bool env_truthy(const char *value) {
    return value && (value[0] == '1' || value[0] == 't' || value[0] == 'T' ||
                     value[0] == 'y' || value[0] == 'Y');
}

static CFStringRef auth_ui_policy(void) {
    return env_truthy(getenv("DSCO_SECURE_STORE_AUTH_UI"))
        ? kSecUseAuthenticationUIAllow
        : kSecUseAuthenticationUIFail;
}

/* ── module state ───────────────────────────────────────────────────────── */

typedef enum { TIER_NONE, TIER_SE, TIER_KEYCHAIN, TIER_IOKIT } se_tier_t;

static se_tier_t s_tier      = TIER_NONE;
static SecKeyRef s_se_private = NULL;   /* Tier 1 only; freed on wipe */

/* ── helpers ────────────────────────────────────────────────────────────── */

static CFDataRef make_tag(void) {
    return CFDataCreate(NULL, (const uint8_t *)SE_KEY_TAG, sizeof(SE_KEY_TAG) - 1);
}

static void log_cferr(const char *ctx, CFErrorRef err) {
    if (!err) return;
    CFStringRef desc = CFErrorCopyDescription(err);
    char buf[512] = {0};
    CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(desc);
    char msg[600];
    snprintf(msg, sizeof(msg), "%s: %s", ctx, buf);
    audit_log("se_store", msg);
}

static void log_secstatus(const char *ctx, OSStatus status) {
    CFStringRef desc = SecCopyErrorMessageString(status, NULL);
    char text[256] = {0};
    if (desc) {
        CFStringGetCString(desc, text, sizeof(text), kCFStringEncodingUTF8);
        CFRelease(desc);
    }
    char msg[420];
    snprintf(msg, sizeof(msg), "%s: OSStatus %d%s%s",
             ctx, (int)status, text[0] ? " - " : "", text);
    audit_log("se_store", msg);
}

/* ── Tier 1: Secure Enclave ─────────────────────────────────────────────── */

static SecKeyRef retrieve_se_key(void) {
    CFDataRef tag = make_tag();
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFDictionarySetValue(q, kSecClass,              kSecClassKey);
    CFDictionarySetValue(q, kSecAttrKeyClass,       kSecAttrKeyClassPrivate);
    CFDictionarySetValue(q, kSecAttrApplicationTag, tag);
    CFDictionarySetValue(q, kSecAttrKeyType,        kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(q, kSecReturnRef,          kCFBooleanTrue);
    CFDictionarySetValue(q, kSecUseAuthenticationUI, auth_ui_policy());

    SecKeyRef key = NULL;
    OSStatus status = SecItemCopyMatching(q, (CFTypeRef *)&key);

    CFRelease(q);
    CFRelease(tag);

    if (status == errSecSuccess && key) return key;
    return NULL;
}

static SecAccessControlRef create_se_acl(void) {
    CFErrorRef err = NULL;
    SecAccessControlRef acl = SecAccessControlCreateWithFlags(
        NULL,
        kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
        kSecAccessControlPrivateKeyUsage,
        &err);
    if (!acl) {
        if (err) CFRelease(err);
        return NULL;
    }
    return acl;
}

static SecKeyRef create_ephemeral_se_key(void) {
    CFErrorRef err = NULL;
    SecAccessControlRef acl = create_se_acl();
    if (!acl) return NULL;

    CFNumberRef key_size = CFNumberCreate(NULL, kCFNumberIntType, &(int){256});

    CFMutableDictionaryRef params = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(params, kSecAttrKeyType,       kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(params, kSecAttrKeySizeInBits, key_size);
    CFDictionarySetValue(params, kSecAttrTokenID,       kSecAttrTokenIDSecureEnclave);
    CFDictionarySetValue(params, kSecAttrAccessControl, acl);

    SecKeyRef private_key = SecKeyCreateRandomKey(params, &err);
    CFRelease(params);
    CFRelease(key_size);
    CFRelease(acl);

    if (!private_key) {
        log_cferr("SecKeyCreateRandomKey (ephemeral SE)", err);
        if (err) CFRelease(err);
        return NULL;
    }

    audit_log("se_store", "SE ephemeral P-256 key created");
    return private_key;
}

static SecKeyRef create_persistent_se_key(void) {
    CFErrorRef err = NULL;
    CFDataRef  tag = make_tag();
    SecAccessControlRef acl = create_se_acl();
    if (!acl) {
        CFRelease(tag);
        return NULL;
    }

    CFNumberRef key_size = CFNumberCreate(NULL, kCFNumberIntType, &(int){256});

    CFMutableDictionaryRef params = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(params, kSecAttrKeyType,          kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(params, kSecAttrKeySizeInBits,    key_size);
    CFDictionarySetValue(params, kSecAttrTokenID,          kSecAttrTokenIDSecureEnclave);
    CFDictionarySetValue(params, kSecAttrIsPermanent,      kCFBooleanTrue);
    CFDictionarySetValue(params, kSecAttrApplicationTag,   tag);
    CFDictionarySetValue(params, kSecAttrAccessControl,    acl);

    err = NULL;
    SecKeyRef private_key = SecKeyCreateRandomKey(params, &err);
    CFRelease(params);

    if (!private_key) {
        log_cferr("SecKeyCreateRandomKey (persistent SE)", err);
        if (err) CFRelease(err);
        CFRelease(key_size); CFRelease(acl); CFRelease(tag);
        return NULL;
    }

    audit_log("se_store", "SE persistent P-256 key created");
    CFRelease(key_size);
    CFRelease(acl);
    CFRelease(tag);
    return private_key;
}

static bool derive_se_master_key(SecKeyRef se_private, uint8_t out[ECDH_KEY_SIZE]) {
    CFErrorRef err = NULL;

    CFMutableDictionaryRef eph_params = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef eph_size = CFNumberCreate(NULL, kCFNumberIntType, &(int){256});
    CFDictionarySetValue(eph_params, kSecAttrKeyType,       kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(eph_params, kSecAttrKeySizeInBits, eph_size);

    SecKeyRef eph_private = SecKeyCreateRandomKey(eph_params, &err);
    CFRelease(eph_params);
    CFRelease(eph_size);

    if (!eph_private) {
        log_cferr("SecKeyCreateRandomKey (eph)", err);
        if (err) CFRelease(err);
        return false;
    }
    SecKeyRef eph_public = SecKeyCopyPublicKey(eph_private);

    CFMutableDictionaryRef kdf = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef req = CFNumberCreate(NULL, kCFNumberIntType, &(int){ECDH_KEY_SIZE});
    CFDataRef   info = CFDataCreate(NULL,
        (const uint8_t *)SE_KEY_TAG, sizeof(SE_KEY_TAG) - 1);

    CFDictionarySetValue(kdf, kSecKeyKeyExchangeParameterRequestedSize, req);
    CFDictionarySetValue(kdf, kSecKeyKeyExchangeParameterSharedInfo,    info);

    err = NULL;
    CFDataRef shared = SecKeyCopyKeyExchangeResult(
        se_private,
        kSecKeyAlgorithmECDHKeyExchangeStandardX963SHA256,
        eph_public, kdf, &err);

    CFRelease(req); CFRelease(info); CFRelease(kdf);
    CFRelease(eph_public); CFRelease(eph_private);

    if (!shared) {
        log_cferr("SecKeyCopyKeyExchangeResult", err);
        if (err) CFRelease(err);
        return false;
    }

    size_t len = (size_t)CFDataGetLength(shared);
    if (len < ECDH_KEY_SIZE) { CFRelease(shared); return false; }
    memcpy(out, CFDataGetBytePtr(shared), ECDH_KEY_SIZE);
    CFRelease(shared);
    return true;
}

static bool try_se_tier(uint8_t out[32]) {
    if (env_truthy(getenv("DSCO_SE_PERSISTENT"))) {
        s_se_private = retrieve_se_key();
        if (!s_se_private) s_se_private = create_persistent_se_key();
    } else {
        s_se_private = create_ephemeral_se_key();
    }
    if (!s_se_private) return false;
    if (!derive_se_master_key(s_se_private, out)) {
        CFRelease(s_se_private);
        s_se_private = NULL;
        return false;
    }
    return true;
}

/* ── Tier 2: macOS Keychain generic password ────────────────────────────── */

static bool try_keychain_tier(uint8_t out[32]) {
    /* Try to load existing 32-byte key */
    CFStringRef svc = CFSTR(KC_SERVICE);
    CFStringRef acc = CFSTR(KC_ACCOUNT);

    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass,       kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, svc);
    CFDictionarySetValue(q, kSecAttrAccount, acc);
    CFDictionarySetValue(q, kSecReturnData,  kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit,  kSecMatchLimitOne);
    CFDictionarySetValue(q, kSecUseAuthenticationUI, auth_ui_policy());

    CFDataRef data = NULL;
    OSStatus st = SecItemCopyMatching(q, (CFTypeRef *)&data);
    CFRelease(q);

    if (st == errSecSuccess && data && CFDataGetLength(data) == 32) {
        memcpy(out, CFDataGetBytePtr(data), 32);
        CFRelease(data);
        return true;
    }
    if (st != errSecSuccess && st != errSecItemNotFound) {
        log_secstatus("keychain lookup failed", st);
    }
    if (data) CFRelease(data);

    /* First run: generate and persist */
    uint8_t key[32];
    arc4random_buf(key, sizeof(key));

    CFDataRef key_data = CFDataCreate(NULL, key, 32);
    CFMutableDictionaryRef item = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(item, kSecClass,          kSecClassGenericPassword);
    CFDictionarySetValue(item, kSecAttrService,    svc);
    CFDictionarySetValue(item, kSecAttrAccount,    acc);
    CFDictionarySetValue(item, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    CFDictionarySetValue(item, kSecValueData,      key_data);

    st = SecItemAdd(item, NULL);
    CFRelease(item);
    CFRelease(key_data);

    if (st == errSecSuccess || st == errSecDuplicateItem) {
        memcpy(out, key, 32);
        zero_buf(key, 32);
        return true;
    }

    log_secstatus("keychain add failed", st);
    zero_buf(key, 32);
    return false;
}

/* ── Tier 3: IOKit machine UUID → HKDF-SHA256 ──────────────────────────── */

static bool try_iokit_tier(uint8_t out[32]) {
    io_service_t expert = IOServiceGetMatchingService(
        kIOMainPortDefault,
        IOServiceMatching("IOPlatformExpertDevice"));
    if (!expert) return false;

    CFStringRef uuid_cf = IORegistryEntryCreateCFProperty(
        expert, CFSTR("IOPlatformUUID"), kCFAllocatorDefault, 0);
    IOObjectRelease(expert);
    if (!uuid_cf) return false;

    char uuid[64] = {0};
    CFStringGetCString(uuid_cf, uuid, sizeof(uuid), kCFStringEncodingUTF8);
    CFRelease(uuid_cf);
    if (!uuid[0]) return false;

    /* HKDF-SHA256 (two-step with CommonCrypto HMAC-SHA256)
     * Extract: PRK = HMAC-SHA256(salt="dsco.master.v1", IKM=uuid)
     * Expand:  OKM = HMAC-SHA256(PRK, info="machine-bound-key" || 0x01) */
    static const char salt[] = "dsco.master.v1";
    static const char info[] = "machine-bound-key\x01";

    uint8_t prk[CC_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256,
           salt, sizeof(salt) - 1,
           uuid, strlen(uuid),
           prk);

    CCHmac(kCCHmacAlgSHA256,
           prk,  sizeof(prk),
           info, sizeof(info) - 1,
           out);

    zero_buf(prk, sizeof(prk));
    return true;
}

/* ── public API ─────────────────────────────────────────────────────────── */

bool se_store_init(uint8_t out_key[32]) {
    bool require_se = env_truthy(getenv("DSCO_REQUIRE_SECURE_ENCLAVE"));
    bool no_prompt  = env_truthy(getenv("DSCO_SECURE_STORE_NO_PROMPT"));
    bool try_se = require_se ||
                  env_truthy(getenv("DSCO_TRY_SECURE_ENCLAVE")) ||
                  env_truthy(getenv("DSCO_SE_PERSISTENT"));

    if (try_se) {
        if (try_se_tier(out_key)) {
            s_tier = TIER_SE;
            audit_log("se_store", "tier=SE ECDH master key derived (hardware-rooted)");
            return true;
        }

        if (require_se) {
            audit_log("se_store", "Secure Enclave required but unavailable");
            return false;
        }
    }

    /* No-prompt mode: the login-keychain tier shows the macOS "dsco wants to
     * use confidential information stored in your keychain" dialog whenever the
     * binary's signature changes (i.e. every dev rebuild). Skip it and derive a
     * stable device-local key from hardware identifiers instead. Safe because
     * sealed_store is in-memory and re-bootstrapped from env on every start, so
     * the master key only needs to be stable within a single run. */
    if (no_prompt && !require_se) {
        if (try_iokit_tier(out_key)) {
            s_tier = TIER_IOKIT;
            audit_log("se_store", "tier=iokit master key derived (no-prompt mode)");
            return true;
        }
        audit_log("se_store", "no-prompt mode requested but iokit tier unavailable");
        return false;
    }

    if (try_keychain_tier(out_key)) {
        s_tier = TIER_KEYCHAIN;
        audit_log("se_store", "tier=keychain master key loaded (login-keychain-protected)");
        return true;
    }

    if (env_truthy(getenv("DSCO_ALLOW_MACHINE_UUID_STORE")) && try_iokit_tier(out_key)) {
        s_tier = TIER_IOKIT;
        audit_log("se_store", "tier=iokit master key derived (explicit weak fallback)");
        return true;
    }

    audit_log("se_store", "all secure tiers failed");
    return false;
}

void se_store_wipe(void) {
    s_tier = TIER_NONE;
    if (s_se_private) {
        CFRelease(s_se_private);
        s_se_private = NULL;
    }
}

bool se_store_available(void) { return s_tier != TIER_NONE; }

int se_store_sign(const uint8_t *data, size_t data_len,
                  uint8_t *sig_buf, size_t sig_buf_len) {
    if (s_tier != TIER_SE || !s_se_private || !data || !sig_buf) return -1;

    CFErrorRef err  = NULL;
    CFDataRef  msg  = CFDataCreate(NULL, data, (CFIndex)data_len);
    CFDataRef  sig  = SecKeyCreateSignature(
        s_se_private,
        kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
        msg, &err);
    CFRelease(msg);

    if (!sig) {
        log_cferr("SecKeyCreateSignature", err);
        if (err) CFRelease(err);
        return -1;
    }
    size_t sig_len = (size_t)CFDataGetLength(sig);
    if (sig_len > sig_buf_len) { CFRelease(sig); return -1; }
    memcpy(sig_buf, CFDataGetBytePtr(sig), sig_len);
    CFRelease(sig);
    return (int)sig_len;
}

bool se_store_pubkey(uint8_t out[65]) {
    if (s_tier != TIER_SE || !s_se_private) return false;

    CFErrorRef err = NULL;
    SecKeyRef pub  = SecKeyCopyPublicKey(s_se_private);
    if (!pub) return false;

    CFDataRef ext = SecKeyCopyExternalRepresentation(pub, &err);
    CFRelease(pub);
    if (!ext) {
        log_cferr("SecKeyCopyExternalRepresentation", err);
        if (err) CFRelease(err);
        return false;
    }
    size_t len = (size_t)CFDataGetLength(ext);
    if (len != 65) { CFRelease(ext); return false; }
    memcpy(out, CFDataGetBytePtr(ext), 65);
    CFRelease(ext);
    return true;
}

#else  /* !__APPLE__ — stubs */

#include "se_store.h"
#include <string.h>

bool se_store_init(uint8_t out_key[32]) { (void)out_key; return false; }
void se_store_wipe(void) {}
bool se_store_available(void) { return false; }
int  se_store_sign(const uint8_t *d, size_t dl, uint8_t *s, size_t sl)
     { (void)d;(void)dl;(void)s;(void)sl; return -1; }
bool se_store_pubkey(uint8_t out[65]) { (void)out; return false; }

#endif /* __APPLE__ */
