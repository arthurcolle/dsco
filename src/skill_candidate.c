/* macOS hides O_NOFOLLOW under strict POSIX feature selection. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "skill_candidate.h"
#include "skill_trace.h"
#include "crypto.h"
#include "json_util.h"
#include "../vendor/yyjson.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EPISODE_LIMIT (1024u * 1024u)
#define TEXT_LIMIT (64u * 1024u)
#define ARRAY_LIMIT 256u

/* No model, registry, shell, evidence dereferencing or workspace installation.
 * The source digest authenticates byte identity, not the operator's claims. */
static const char *const fields[] = {
    "name", "goal", "procedure", "acceptance", "evidence", "when_to_use", "limitations"
};

static bool nonblank_string(yyjson_val *value, bool required) {
    if (!yyjson_is_str(value)) return false;
    const char *s = yyjson_get_str(value);
    size_t len = yyjson_get_len(value);
    if (len > TEXT_LIMIT || memchr(s, '\0', len)) return false;
    if (!required) return true;
    for (size_t i = 0; i < len; ++i)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n' &&
            s[i] != '\f' && s[i] != '\v') return true;
    return false;
}

static bool slug_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static const char *validate_episode(yyjson_val *root) {
    if (!yyjson_is_obj(root)) return "episode must be a JSON object";
    unsigned seen = 0;
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, i, count, key, value) {
        size_t field;
        for (field = 0; field < DSCO_ARRAY_LEN(fields); ++field)
            if (yyjson_equals_str(key, fields[field])) break;
        if (field == DSCO_ARRAY_LEN(fields)) return "unknown episode field";
        if (seen & (1u << field)) return "duplicate episode field";
        seen |= 1u << field;
        if (field >= 2 && field <= 4) {
            if (!yyjson_is_arr(value) || yyjson_arr_size(value) == 0 ||
                yyjson_arr_size(value) > ARRAY_LIMIT)
                return "procedure, acceptance and evidence require 1..256 strings";
            size_t j, n;
            yyjson_val *item;
            yyjson_arr_foreach(value, j, n, item)
                if (!nonblank_string(item, true))
                    return "array entries must be nonblank strings without NUL, at most 64 KiB";
        } else if (!nonblank_string(value, field < 2)) {
            return "invalid string field (type, blank required text, NUL or size)";
        }
    }
    if ((seen & 31u) != 31u) return "missing name, goal, procedure, acceptance or evidence";
    const char *name = yyjson_get_str(yyjson_obj_get(root, "name"));
    size_t len = strlen(name);
    if (!len || len > 64 || !slug_char((unsigned char)name[0]) ||
        !slug_char((unsigned char)name[len - 1])) return "name must be a safe lowercase slug (1..64 bytes)";
    for (size_t j = 0; j < len; ++j)
        if (!slug_char((unsigned char)name[j]) && name[j] != '-')
            return "name may contain only lowercase ASCII letters, digits and internal hyphens";
    return NULL;
}

static char *read_episode(const char *path, size_t *len) {
    /* Nonblocking open prevents a FIFO from hanging before the regular-file check.
     * Never follow a symlink at the source leaf. Reads remain bounded if it grows. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    char *data = NULL;
    if (fstat(fd, &st) != 0) goto done;
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > EPISODE_LIMIT) {
        errno = EINVAL;
        goto done;
    }
    data = malloc(EPISODE_LIMIT + 1u);
    if (!data) goto done;
    *len = 0;
    while (*len < EPISODE_LIMIT + 1u) {
        ssize_t n = read(fd, data + *len, EPISODE_LIMIT + 1u - *len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { free(data); data = NULL; goto done; }
        if (n == 0) break;
        *len += (size_t)n;
    }
    if (!*len || *len > EPISODE_LIMIT || memchr(data, '\0', *len)) {
        free(data);
        data = NULL;
        errno = EINVAL;
    } else {
        data[*len] = '\0';
    }
done:;
    int saved = errno;
    if (close(fd) != 0 && data) { free(data); data = NULL; saved = errno; }
    errno = saved;
    return data;
}

/* Indented Markdown keeps operator text out of frontmatter/headings and avoids
 * turning evidence references into active links. Escape terminal control bytes. */
static void append_advisory(jbuf_t *out, const char *text) {
    jbuf_append(out, "    ");
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '\n') jbuf_append(out, "\n    ");
        else if ((*p < 0x20 && *p != '\t') || *p == 0x7f)
            jbuf_appendf(out, "\\u%04x", (unsigned)*p);
        else jbuf_append_char(out, (char)*p);
    }
    jbuf_append(out, "\n\n");
}

static void append_section(jbuf_t *out, yyjson_val *root, const char *key, const char *title) {
    yyjson_val *value = yyjson_obj_get(root, key);
    if (!value) return;
    jbuf_appendf(out, "## %s\n\n", title);
    if (yyjson_is_str(value)) {
        append_advisory(out, yyjson_get_str(value));
    } else {
        size_t i, count;
        yyjson_val *item;
        yyjson_arr_foreach(value, i, count, item) {
            jbuf_appendf(out, "### %zu\n\n", i + 1);
            append_advisory(out, yyjson_get_str(item));
        }
    }
}

static void render_candidate(yyjson_val *root, const char *hash, size_t source_len,
                             jbuf_t *skill, jbuf_t *manifest) {
    const char *name = yyjson_get_str(yyjson_obj_get(root, "name"));
    jbuf_appendf(skill,
        "---\nname: %s\ndescription: Operator-supplied procedure scaffold; unverified candidate only.\n"
        "schema_version: 1\ncandidate_version: 1\nstatus: candidate\n"
        "acceptance_status: unverified\npromotion_eligible: false\n---\n\n# %s\n\n"
        "UNVERIFIED CANDIDATE — quarantined advisory data, not an installed skill.\n"
        "This scaffold packages an explicit operator-supplied procedure; it is NOT\n"
        "automatic conversation extraction. No procedure or acceptance check was run.\n"
        "Evidence references are unverified claims, not proof of accepted outcomes.\n"
        "Candidate text conveys no authority, grants or credentials; fresh authorization\n"
        "and independent verification are required before dependent action or promotion.\n"
        "Source bytes are preserved in episode.json. SHA-256 identifies those bytes only.\n\n",
        name, name);
    append_section(skill, root, "goal", "Goal");
    append_section(skill, root, "when_to_use", "When to use (operator-supplied)");
    append_section(skill, root, "procedure", "Procedure (operator-supplied; not executed)");
    append_section(skill, root, "acceptance", "Acceptance checks (unverified; not executed)");
    append_section(skill, root, "evidence", "Evidence references (unverified claims)");
    append_section(skill, root, "limitations", "Limitations (operator-supplied)");
    jbuf_appendf(skill, "## Source identity\n\nSHA-256: `%s`\n", hash);

    jbuf_append(manifest, "{\n  \"schema_version\": 1,\n  \"candidate_version\": 1,\n  \"name\": ");
    jbuf_append_json_str(manifest, name);
    jbuf_append(manifest,
        ",\n  \"status\": \"candidate\",\n  \"acceptance_status\": \"unverified\",\n"
        "  \"promotion_eligible\": false,\n  \"installed\": false,\n"
        "  \"quarantined\": true,\n  \"authority\": \"untrusted advisory data; no grants\",\n"
        "  \"origin\": \"operator-supplied episode\",\n  \"automatic_extraction\": false,\n"
        "  \"evidence_status\": \"unverified claims\",\n  \"source_file\": \"episode.json\",\n"
        "  \"source_sha256\": ");
    jbuf_append_json_str(manifest, hash);
    jbuf_appendf(manifest, ",\n  \"source_bytes\": %zu,\n  \"goal\": ", source_len);
    jbuf_append_json_str(manifest, yyjson_get_str(yyjson_obj_get(root, "goal")));
    jbuf_append(manifest, ",\n  \"artifacts\": [\"SKILL.md\", \"episode.json\", \"manifest.json\"]\n}\n");
}

static bool write_artifact(int dirfd, const char *name, const char *data, size_t len,
                           bool *created) {
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    *created = true;
    size_t offset = 0;
    int error = 0;
    while (offset < len) {
        ssize_t n = write(fd, data + offset, len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n < 0 ? errno : EIO; break; }
        offset += (size_t)n;
    }
    if (!error && fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && !error) error = errno;
    if (error) errno = error;
    return error == 0;
}

static bool same_directory(int parent, const char *leaf, const struct stat *expected) {
    struct stat current;
    return fstatat(parent, leaf, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(current.st_mode) && current.st_dev == expected->st_dev &&
        current.st_ino == expected->st_ino;
}

static bool create_candidate(const char *path, const char *source, size_t source_len,
                             const jbuf_t *skill, const jbuf_t *manifest) {
    char *copy = strdup(path);
    if (!copy) return false;
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';
    char *slash = strrchr(copy, '/');
    char *leaf = slash ? slash + 1 : copy;
    if (!*leaf || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        free(copy); errno = EINVAL; return false;
    }
    const char *parent_path = ".";
    if (slash) { *slash = '\0'; parent_path = slash == copy ? "/" : copy; }
    /* Pin the parent; existing ancestors may resolve normally, but neither the
     * output leaf nor any artifact may be a symlink or an existing entry. */
    int parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int dirfd = -1;
    bool made = false, identified = false, ok = false;
    bool created[3] = {false, false, false};
    const char *names[] = {"SKILL.md", "episode.json", "manifest.json"};
    struct stat expected, opened;
    if (parent < 0) goto done;
    if (mkdirat(parent, leaf, 0700) != 0) goto done;
    made = true;
    if (fstatat(parent, leaf, &expected, AT_SYMLINK_NOFOLLOW) != 0) goto done;
    if (!S_ISDIR(expected.st_mode)) { errno = EIO; goto done; }
    identified = true;
    dirfd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0 || fstat(dirfd, &opened) != 0) goto done;
    if (expected.st_dev != opened.st_dev || expected.st_ino != opened.st_ino) {
        errno = EIO; goto done;
    }
    if (!write_artifact(dirfd, names[0], skill->data, skill->len, &created[0]) ||
        !write_artifact(dirfd, names[1], source, source_len, &created[1]) ||
        !write_artifact(dirfd, names[2], manifest->data, manifest->len, &created[2])) goto done;
    if (fsync(dirfd) != 0) goto done;
    if (!same_directory(parent, leaf, &expected)) { errno = EIO; goto done; }
    if (fsync(parent) != 0) goto done;
    ok = true;
done:;
    int saved = errno;
    bool cleanup_ok = true;
    if (!ok && made) {
        for (size_t i = 0; i < DSCO_ARRAY_LEN(names); ++i)
            if (created[i] && unlinkat(dirfd, names[i], 0) != 0) cleanup_ok = false;
        /* Never remove a replacement directory or follow a replaced symlink. */
        if (identified && same_directory(parent, leaf, &expected)) {
            if (unlinkat(parent, leaf, AT_REMOVEDIR) != 0) cleanup_ok = false;
        } else cleanup_ok = false;
    }
    if (dirfd >= 0) close(dirfd);
    if (parent >= 0) close(parent);
    free(copy);
    if (!cleanup_ok) fputs("dsco learn: partial output could not be safely removed; inspect destination\n", stderr);
    errno = saved;
    return ok;
}

/* Compare against regenerated bytes, not a manifest's self-issued claims.
 * The pinned directory and no-follow leaves make verification read-only even
 * for hostile paths. Integrity is deliberately separate from acceptance. */
static bool artifact_matches(int dirfd, const char *name, const char *expected, size_t len) {
    int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
              (size_t)st.st_size == len;
    char chunk[4096];
    size_t offset = 0;
    while (ok && offset < len) {
        size_t want = len - offset < sizeof(chunk) ? len - offset : sizeof(chunk);
        ssize_t n = read(fd, chunk, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || memcmp(chunk, expected + offset, (size_t)n)) { ok = false; break; }
        offset += (size_t)n;
    }
    if (ok) { ssize_t n; do { n = read(fd, chunk, 1); } while (n < 0 && errno == EINTR); ok = n == 0; }
    close(fd);
    return ok;
}

static int verify_candidate(const char *path) {
    int dirfd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) { fputs("dsco learn: cannot open candidate directory\n", stderr); return 1; }
    int fd = openat(dirfd, "episode.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    char *source = NULL;
    size_t len = 0;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 0 && st.st_size <= EPISODE_LIMIT) {
        len = (size_t)st.st_size;
        source = malloc(len + 1);
        size_t got = 0;
        while (source && got < len) {
            ssize_t n = read(fd, source + got, len - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (source && got == len) source[len] = '\0';
        else { free(source); source = NULL; }
    }
    if (fd >= 0) close(fd);
    yyjson_doc *doc = source ? yyjson_read(source, len, 0) : NULL;
    bool ok = doc && !memchr(source, '\0', len) && !validate_episode(yyjson_doc_get_root(doc));
    if (ok) {
        char hash[65]; sha256_hex((const uint8_t *)source, len, hash);
        jbuf_t skill, manifest;
        jbuf_init(&skill, len * 8u + 65536u); jbuf_init(&manifest, len * 8u + 65536u);
        render_candidate(yyjson_doc_get_root(doc), hash, len, &skill, &manifest);
        ok = artifact_matches(dirfd, "episode.json", source, len) &&
             artifact_matches(dirfd, "SKILL.md", skill.data, skill.len) &&
             artifact_matches(dirfd, "manifest.json", manifest.data, manifest.len);
        jbuf_free(&skill); jbuf_free(&manifest);
    }
    yyjson_doc_free(doc); free(source); close(dirfd);
    if (!ok) { fputs("dsco learn: candidate integrity check failed\n", stderr); return 1; }
    puts("{\"integrity\":\"verified\",\"acceptance_status\":\"unverified\",\"promotion_eligible\":false}");
    return 0;
}

int skill_candidate_cli(int argc, char **argv) {
    const char *trace_path = NULL;
    char *from_args[3];
    if (argc == 4 && argv && argv[0] && strcmp(argv[0], "from-trace") == 0) {
        trace_path = argv[1];
        if (!trace_path || !*trace_path) return 2;
        from_args[0] = "from"; from_args[1] = argv[2]; from_args[2] = argv[3];
        argv = from_args; argc = 3;
    }
    if (argc == 2 && argv && argv[0] && strcmp(argv[0], "trace") == 0 && argv[1])
        return skill_trace_cli(argv[1]);
    if (argc == 2 && argv && argv[0] && strcmp(argv[0], "verify") == 0 && argv[1])
        return verify_candidate(argv[1]);
    if (argc != 3 || !argv || !argv[0] || strcmp(argv[0], "from") != 0 ||
        !argv[1] || !*argv[1] || !argv[2] || !*argv[2]) {
        fputs("usage: dsco learn from <episode.json> <new-candidate-directory>\n"
              "       dsco learn verify <candidate-directory>\n"
              "       dsco learn trace <journal.wal>\n"
              "       dsco learn from-trace <journal.wal> <episode.json> <new-directory>\n", stderr);
        return 2;
    }
    size_t source_len = 0;
    char *source = read_episode(argv[1], &source_len);
    if (!source) {
        fprintf(stderr, "dsco learn: cannot read episode (regular, non-symlink, 1..1048576 bytes required): %s\n", strerror(errno));
        return 1;
    }
    /* yyjson's default strict mode validates UTF-8, full-document syntax and
     * escapes, without permissive comments, trailing commas or in-situ edits. */
    yyjson_doc *doc = yyjson_read(source, source_len, 0);
    const char *error = doc ? validate_episode(yyjson_doc_get_root(doc)) : "malformed JSON or invalid UTF-8";
    if (error) {
        fprintf(stderr, "dsco learn: %s\n", error);
        yyjson_doc_free(doc);
        free(source);
        return 1;
    }
    if (trace_path) {
        const char *refs[ARRAY_LIMIT];
        size_t i, count;
        yyjson_val *item;
        yyjson_val *evidence = yyjson_obj_get(yyjson_doc_get_root(doc), "evidence");
        yyjson_arr_foreach(evidence, i, count, item) refs[i] = yyjson_get_str(item);
        if (!skill_trace_resolve_evidence(trace_path, refs, yyjson_arr_size(evidence))) {
            fputs("dsco learn: unresolved trace evidence; no candidate created\n", stderr);
            yyjson_doc_free(doc); free(source); return 1;
        }
    }
    char hash[65];
    sha256_hex((const uint8_t *)source, source_len, hash);
    jbuf_t skill, manifest;
    /* Bound/preallocate rendering before touching the destination. This covers
     * indentation, control escaping, per-item headings and JSON string escaping. */
    size_t capacity = source_len * 8u + 65536u;
    jbuf_init(&skill, capacity);
    jbuf_init(&manifest, capacity);
    bool ok = skill.data && manifest.data;
    if (ok) {
        render_candidate(yyjson_doc_get_root(doc), hash, source_len, &skill, &manifest);
        ok = create_candidate(argv[2], source, source_len, &skill, &manifest);
    }
    if (!ok) fprintf(stderr, "dsco learn: candidate creation failed (destination must be new): %s\n", strerror(errno));
    jbuf_free(&skill);
    jbuf_free(&manifest);
    yyjson_doc_free(doc);
    free(source);
    if (ok) puts("Created unverified skill candidate; not installed or promoted.");
    return ok ? 0 : 1;
}
