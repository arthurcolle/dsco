#include "hotswap.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* hotswap.c — atomic, reversible runtime module swapping. See hotswap.h.
 * Orchestrates plugin.c's load/unload; computes/verifies file hashes; keeps a
 * last-known-good snapshot for one-call rollback. Writes a receipt per swap. */

bool hotswap_file_sha256(const char *path, char out_hex[HOTSWAP_HASH_HEX]) {
    if (!path || !out_hex) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    /* Read the whole file; modules are small. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long n = ftell(f);
    if (n < 0 || n > 64L * 1024 * 1024) { fclose(f); return false; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return false; }
    sha256_hex(buf, (size_t)n, out_hex);
    free(buf);
    return true;
}

static plugin_t *find(plugin_registry_t *reg, const char *name) {
    if (!reg || !name) return NULL;
    for (int i = 0; i < reg->count; i++)
        if (reg->plugins[i].loaded && reg->plugins[i].name &&
            strcmp(reg->plugins[i].name, name) == 0)
            return &reg->plugins[i];
    return NULL;
}

bool hotswap_snapshot(plugin_registry_t *reg, const char *name,
                      hotswap_snapshot_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    plugin_t *p = find(reg, name);
    if (!p) return false;
    snprintf(out->name, sizeof(out->name), "%s", p->name ? p->name : "");
    snprintf(out->version, sizeof(out->version), "%s", p->version ? p->version : "");
    snprintf(out->path, sizeof(out->path), "%s", p->path);
    if (!hotswap_file_sha256(p->path, out->file_sha256)) return false;
    out->valid = true;
    return true;
}

bool hotswap_verify_candidate(const char *candidate_path,
                              const char *expected_hash_hex,
                              bool require_signed,
                              char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!candidate_path || !candidate_path[0]) {
        if (err) snprintf(err, err_len, "empty candidate path");
        return false;
    }
    if (expected_hash_hex && expected_hash_hex[0]) {
        char actual[HOTSWAP_HASH_HEX];
        if (!hotswap_file_sha256(candidate_path, actual)) {
            if (err) snprintf(err, err_len, "cannot read candidate for hashing");
            return false;
        }
        if (strcmp(actual, expected_hash_hex) != 0) {
            if (err) snprintf(err, err_len, "hash mismatch: candidate does not match manifest");
            return false;
        }
    } else if (require_signed) {
        if (err) snprintf(err, err_len, "signed/verified candidate required but no hash provided");
        return false;
    }
    return true;
}

static bool finish(char *receipt, size_t len, const char *status, const char *detail) {
    if (receipt && len)
        snprintf(receipt, len,
            "{\"schema\":\"dsco.hotswap.v1\",\"status\":\"%s\",\"detail\":\"%s\",\"applied\":false}",
            status, detail ? detail : "");
    return false;
}

bool hotswap_module(plugin_registry_t *reg, const char *name,
                    const char *candidate_path, const char *expected_hash,
                    bool require_signed,
                    hotswap_snapshot_t *snapshot_out,
                    char *receipt, size_t receipt_len) {
    if (!reg || !name || !candidate_path || !snapshot_out || !receipt || receipt_len < 256)
        return finish(receipt, receipt_len, "invalid_input", "missing registry/name/candidate/output");
    if (!find(reg, name))
        return finish(receipt, receipt_len, "not_loaded", name);

    /* 1. SNAPSHOT before any mutation. */
    if (!hotswap_snapshot(reg, name, snapshot_out))
        return finish(receipt, receipt_len, "snapshot_failed", name);

    /* 2. VERIFY the candidate BEFORE touching the loaded module. */
    char verr[160];
    if (!hotswap_verify_candidate(candidate_path, expected_hash, require_signed, verr, sizeof(verr)))
        return finish(receipt, receipt_len, "verify_failed", verr);

    /* 3. LOAD the candidate first. If it fails, the old module stays put. */
    if (!plugin_load(reg, candidate_path))
        return finish(receipt, receipt_len, "candidate_load_failed", candidate_path);

    /* 4. Only now unload the old module. */
    if (!plugin_unload(reg, name)) {
        /* Candidate loaded but old won't unload: not fatal for the swap, but
         * record it — two modules of this name may now be present. */
        int w = snprintf(receipt, receipt_len,
            "{\"schema\":\"dsco.hotswap.v1\",\"status\":\"swapped_unload_warning\","
            "\"module\":\"%s\",\"candidate\":\"%s\",\"applied\":true}", name, candidate_path);
        (void)w;
        return true;
    }

    snprintf(receipt, receipt_len,
        "{\"schema\":\"dsco.hotswap.v1\",\"status\":\"swapped\",\"module\":\"%s\","
        "\"candidate\":\"%s\",\"rollback_available\":true,\"applied\":true}",
        name, candidate_path);
    return true;
}

bool hotswap_rollback(plugin_registry_t *reg, const hotswap_snapshot_t *snapshot,
                      char *receipt, size_t receipt_len) {
    if (!reg || !snapshot || !snapshot->valid || !receipt || receipt_len < 256)
        return finish(receipt, receipt_len, "invalid_input", "missing registry/snapshot");
    /* Re-load the snapshot's file (the last-known-good). */
    if (!plugin_load(reg, snapshot->path))
        return finish(receipt, receipt_len, "rollback_load_failed", snapshot->path);
    /* Unload the current (misbehaving) module of the same name, if present and
     * it is not the snapshot we just loaded. */
    (void)plugin_unload(reg, snapshot->name);
    snprintf(receipt, receipt_len,
        "{\"schema\":\"dsco.hotswap.v1\",\"status\":\"rolled_back\",\"module\":\"%s\","
        "\"restored_version\":\"%s\",\"applied\":true}",
        snapshot->name, snapshot->version);
    return true;
}
