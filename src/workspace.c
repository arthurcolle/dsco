#include "workspace.h"
#include "config.h"
#include "json_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define WORKSPACE_FILE_LIMIT 32768

static char *s_workspace_prompt = NULL;
static bool s_workspace_prompt_loaded = false;
static char *s_skill_prompt = NULL;

static const char *k_identity_template =
    "# Identity\n\n"
    "## Name\n"
    "DSCO Claw\n\n"
    "## Description\n"
    "A local-first agentic CLI with tools, sessions, MCP integration, and sub-agents.\n\n"
    "## Purpose\n"
    "- Help the user operate on code, documents, and local systems.\n"
    "- Prefer direct action, concrete outputs, and verifiable changes.\n"
    "- Preserve user control through transparent tool use and local state.\n";

static const char *k_user_template =
    "# User\n\n"
    "Information about the user belongs here.\n\n"
    "## Preferences\n"
    "- Communication style:\n"
    "- Language:\n"
    "- Timezone:\n\n"
    "## Important Context\n"
    "- Project preferences:\n"
    "- Tooling preferences:\n"
    "- Constraints to respect:\n";

static const char *k_soul_template =
    "# Soul\n\n"
    "I am dsco operating as a claw: pragmatic, concise, and action-oriented.\n\n"
    "## Personality\n"
    "- Clear\n"
    "- Direct\n"
    "- Helpful\n"
    "- Skeptical of vague claims\n\n"
    "## Values\n"
    "- Accuracy over theater\n"
    "- Action over narration\n"
    "- User control over hidden automation\n";

static const char *k_memory_template =
    "# Long-term Memory\n\n"
    "Persist useful information across sessions.\n\n"
    "## User Facts\n"
    "- \n\n"
    "## Preferences\n"
    "- \n\n"
    "## Ongoing Work\n"
    "- \n";

static const char *k_skills_readme_template =
    "# Skills\n\n"
    "Each skill lives in its own directory and exposes a `SKILL.md` file.\n\n"
    "Example layout:\n\n"
    "- `skills/<name>/SKILL.md`\n"
    "- optional `references/`\n"
    "- optional `scripts/`\n";

static const char *k_c_workflow_skill =
    "# C Workflow\n\n"
    "Use this skill for C repositories with Makefiles, headers, and multi-module changes.\n\n"
    "## Workflow\n"
    "- Inspect both `include/` and `src/` before editing behavior.\n"
    "- Prefer focused patches over broad rewrites.\n"
    "- Run `make test` after changing behavior.\n"
    "- When changing interfaces, update the matching header and implementation together.\n";

static const char *k_review_skill =
    "# Review Checklist\n\n"
    "Use this skill when reviewing code or validating a patch.\n\n"
    "## Workflow\n"
    "- Prioritize correctness, regressions, and missing tests.\n"
    "- List findings before summaries.\n"
    "- Reference concrete files and behavior.\n"
    "- Call out unverified paths when tests were not run.\n";

static const char *k_soul_curator_skill =
    "# SOUL Curator\n\n"
    "Use this skill to evolve SOUL.md safely and intentionally.\n\n"
    "## Workflow\n"
    "- Read the current SOUL.md before changing it.\n"
    "- Keep tone/values coherent across edits.\n"
    "- Prefer additive improvements over broad rewrites.\n"
    "- Add evidence in the file only when it improves future model behavior.\n";

static const char *k_soul_guardian_skill =
    "# SOUL Guardian\n\n"
    "Use this skill before each SOUL.md mutation.\n\n"
    "## Policy\n"
    "- Validate that changes are internally consistent.\n"
    "- Preserve personality continuity with earlier direction.\n"
    "- Keep operational constraints explicit and testable.\n"
    "- Avoid irreversible edits; prefer incremental updates.\n";

static void sbuf_append(char *out, size_t out_len, size_t *pos, const char *text) {
    if (!out || out_len == 0 || !pos || !text || *pos >= out_len) return;
    size_t remain = out_len - *pos;
    size_t need = strlen(text);
    if (need >= remain) need = remain - 1;
    memcpy(out + *pos, text, need);
    *pos += need;
    out[*pos] = '\0';
}

static bool mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool ensure_parent_dir(const char *path) {
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    if (tmp[0] == '\0') return true;
    return mkdir_p(tmp);
}

static bool read_file_limit(const char *path, size_t limit, char **out) {
    *out = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > limit) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    char *buf = safe_malloc((size_t)sz + 1);
    size_t nr = fread(buf, 1, (size_t)sz, f);
    buf[nr] = '\0';
    fclose(f);
    *out = buf;
    return true;
}

static bool write_if_missing(const char *path, const char *content, int *created) {
    if (access(path, F_OK) == 0) return true;
    if (!ensure_parent_dir(path)) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(content, f);
    fclose(f);
    if (created) (*created)++;
    return true;
}

static bool is_valid_skill_name(const char *name) {
    if (!name || !*name) return false;
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) return false;
    }
    return strstr(name, "..") == NULL;
}

static void workspace_path(char *out, size_t out_len, const char *suffix) {
    const char *root = dsco_workspace_root();
    if (suffix && *suffix) snprintf(out, out_len, "%s/%s", root, suffix);
    else snprintf(out, out_len, "%s", root);
}

void dsco_workspace_doc_path(const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    if (!name || !*name) return;
    if (strcmp(name, "identity") == 0) {
        workspace_path(out, out_len, "IDENTITY.md");
    } else if (strcmp(name, "user") == 0) {
        workspace_path(out, out_len, "USER.md");
    } else if (strcmp(name, "soul") == 0) {
        workspace_path(out, out_len, "SOUL.md");
    } else if (strcmp(name, "memory") == 0) {
        workspace_path(out, out_len, "memory/MEMORY.md");
    }
}

static void trim_line(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) {
        s[--n] = '\0';
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static void markdown_summary_line(const char *text, char *out, size_t out_len) {
    out[0] = '\0';
    if (!text || !*text) return;
    char *copy = safe_strdup(text);
    char *line = strtok(copy, "\n");
    while (line) {
        trim_line(line);
        if (*line && line[0] != '#') {
            snprintf(out, out_len, "%s", line);
            break;
        }
        line = strtok(NULL, "\n");
    }
    free(copy);
}

static int count_installed_skills(void) {
    char skills_dir[PATH_MAX];
    workspace_path(skills_dir, sizeof(skills_dir), "skills");
    DIR *d = opendir(skills_dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char skill_md[PATH_MAX];
        snprintf(skill_md, sizeof(skill_md), "%s/%s/SKILL.md", skills_dir, ent->d_name);
        if (access(skill_md, R_OK) == 0) count++;
    }
    closedir(d);
    return count;
}

const char *dsco_workspace_root(void) {
    static char s_workspace_root[PATH_MAX];
    const char *home = getenv("HOME");
    if (home && *home) snprintf(s_workspace_root, sizeof(s_workspace_root), "%s/.dsco/workspace", home);
    else snprintf(s_workspace_root, sizeof(s_workspace_root), ".dsco/workspace");
    return s_workspace_root;
}

int dsco_workspace_bootstrap(char *summary, size_t summary_len) {
    int created = 0;
    char path[PATH_MAX];

    workspace_path(path, sizeof(path), "");
    if (!mkdir_p(path)) {
        snprintf(summary, summary_len, "workspace bootstrap failed: cannot create %s", path);
        return -1;
    }

    dsco_workspace_doc_path("identity", path, sizeof(path));
    if (!write_if_missing(path, k_identity_template, &created)) goto fail;
    dsco_workspace_doc_path("user", path, sizeof(path));
    if (!write_if_missing(path, k_user_template, &created)) goto fail;
    dsco_workspace_doc_path("soul", path, sizeof(path));
    if (!write_if_missing(path, k_soul_template, &created)) goto fail;
    dsco_workspace_doc_path("memory", path, sizeof(path));
    if (!write_if_missing(path, k_memory_template, &created)) goto fail;
    workspace_path(path, sizeof(path), "skills/README.md");
    if (!write_if_missing(path, k_skills_readme_template, &created)) goto fail;
    workspace_path(path, sizeof(path), "skills/c-workflow/SKILL.md");
    if (!write_if_missing(path, k_c_workflow_skill, &created)) goto fail;
    workspace_path(path, sizeof(path), "skills/review-checklist/SKILL.md");
    if (!write_if_missing(path, k_review_skill, &created)) goto fail;
    workspace_path(path, sizeof(path), "skills/soul-curator/SKILL.md");
    if (!write_if_missing(path, k_soul_curator_skill, &created)) goto fail;
    workspace_path(path, sizeof(path), "skills/soul-guardian/SKILL.md");
    if (!write_if_missing(path, k_soul_guardian_skill, &created)) goto fail;

    dsco_workspace_prompt_invalidate();
    snprintf(summary, summary_len, created > 0
        ? "workspace ready at %s (%d files created)"
        : "workspace ready at %s", dsco_workspace_root(), created);
    return created;

fail:
    snprintf(summary, summary_len, "workspace bootstrap failed while writing %s", path);
    return -1;
}

int dsco_workspace_status(dsco_workspace_status_t *status, char *summary, size_t summary_len) {
    dsco_workspace_status_t st;
    memset(&st, 0, sizeof(st));
    char path[PATH_MAX];

    dsco_workspace_doc_path("identity", path, sizeof(path));
    st.has_identity = access(path, R_OK) == 0;
    dsco_workspace_doc_path("user", path, sizeof(path));
    st.has_user = access(path, R_OK) == 0;
    dsco_workspace_doc_path("soul", path, sizeof(path));
    st.has_soul = access(path, R_OK) == 0;
    dsco_workspace_doc_path("memory", path, sizeof(path));
    st.has_memory = access(path, R_OK) == 0;

    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(path, sizeof(path), "%s/.dsco/system_prompt.txt", home);
        st.has_legacy_prompt = access(path, R_OK) == 0;
    }
    st.installed_skills = count_installed_skills();
    if (status) *status = st;
    if (summary && summary_len > 0) {
        snprintf(summary, summary_len,
                 "workspace=%s identity=%s user=%s soul=%s memory=%s skills=%d legacy_prompt=%s",
                 dsco_workspace_root(),
                 st.has_identity ? "yes" : "no",
                 st.has_user ? "yes" : "no",
                 st.has_soul ? "yes" : "no",
                 st.has_memory ? "yes" : "no",
                 st.installed_skills,
                 st.has_legacy_prompt ? "yes" : "no");
    }
    return 0;
}

int dsco_workspace_read_doc(const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (!name) return -1;
    char path[PATH_MAX];
    dsco_workspace_doc_path(name, path, sizeof(path));
    if (path[0] == '\0') return -1;

    char *text = NULL;
    if (!read_file_limit(path, WORKSPACE_FILE_LIMIT, &text)) {
        snprintf(out, out_len, "missing workspace document: %s", name);
        return -1;
    }
    snprintf(out, out_len, "%s", text);
    free(text);
    return 0;
}

int dsco_workspace_show_skill(const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    if (!is_valid_skill_name(name)) {
        snprintf(out, out_len, "invalid skill name");
        return -1;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/skills/%s/SKILL.md", dsco_workspace_root(), name);
    char *text = NULL;
    if (!read_file_limit(path, WORKSPACE_FILE_LIMIT, &text)) {
        snprintf(out, out_len, "skill not found: %s", name);
        return -1;
    }
    snprintf(out, out_len, "%s", text);
    free(text);
    return 0;
}

int dsco_workspace_list_skills(char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    char skills_dir[PATH_MAX];
    workspace_path(skills_dir, sizeof(skills_dir), "skills");
    DIR *d = opendir(skills_dir);
    if (!d) {
        snprintf(out, out_len, "no skills directory at %s", skills_dir);
        return -1;
    }

    size_t pos = 0;
    sbuf_append(out, out_len, &pos, "Installed skills:\n");
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s/SKILL.md", skills_dir, ent->d_name);
        char *text = NULL;
        if (!read_file_limit(path, 8192, &text)) continue;
        char summary[256];
        markdown_summary_line(text, summary, sizeof(summary));
        free(text);
        char line[512];
        snprintf(line, sizeof(line), "- %s%s%s\n", ent->d_name,
                 summary[0] ? ": " : "", summary);
        sbuf_append(out, out_len, &pos, line);
        count++;
    }
    closedir(d);
    if (count == 0) {
        snprintf(out, out_len, "No installed skills in %s", skills_dir);
        return 0;
    }
    return count;
}

static void append_prompt_section(char *dst, size_t dst_len, size_t *pos,
                                  const char *title, const char *body) {
    if (!body || !*body) return;
    sbuf_append(dst, dst_len, pos, "\n\n[");
    sbuf_append(dst, dst_len, pos, title);
    sbuf_append(dst, dst_len, pos, "]\n");
    sbuf_append(dst, dst_len, pos, body);
}

const char *dsco_workspace_prompt(void) {
    if (s_workspace_prompt_loaded) return s_workspace_prompt;
    s_workspace_prompt_loaded = true;

    char *buf = safe_malloc(131072);
    size_t pos = 0;
    buf[0] = '\0';

    const char *home = getenv("HOME");
    if (home && *home) {
        char legacy[PATH_MAX];
        snprintf(legacy, sizeof(legacy), "%s/.dsco/system_prompt.txt", home);
        char *text = NULL;
        if (read_file_limit(legacy, WORKSPACE_FILE_LIMIT, &text)) {
            append_prompt_section(buf, 131072, &pos, "Legacy System Prompt", text);
            free(text);
        }
    }

    const char *docs[][2] = {
        { "Identity", "IDENTITY.md" },
        { "User", "USER.md" },
        { "Soul", "SOUL.md" },
        { "Long-term Memory", "memory/MEMORY.md" },
    };
    for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        char path[PATH_MAX];
        workspace_path(path, sizeof(path), docs[i][1]);
        char *text = NULL;
        if (read_file_limit(path, WORKSPACE_FILE_LIMIT, &text)) {
            append_prompt_section(buf, 131072, &pos, docs[i][0], text);
            free(text);
        }
    }

    char skills[16384];
    if (dsco_workspace_list_skills(skills, sizeof(skills)) > 0) {
        append_prompt_section(buf, 131072, &pos, "Installed Skills Catalog", skills);
        sbuf_append(buf, 131072, &pos,
                    "\n\n[Skill Policy]\nInstalled skills live under ~/.dsco/workspace/skills/<name>/SKILL.md. "
                    "If the user names a skill explicitly, follow it. Otherwise use skills only when relevant.\n");
    }

    s_workspace_prompt = buf;
    return s_workspace_prompt && s_workspace_prompt[0] ? s_workspace_prompt : NULL;
}

const char *dsco_workspace_skill_prompt(const char *name) {
    free(s_skill_prompt);
    s_skill_prompt = NULL;
    if (!is_valid_skill_name(name)) return NULL;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/skills/%s/SKILL.md", dsco_workspace_root(), name);
    if (!read_file_limit(path, WORKSPACE_FILE_LIMIT, &s_skill_prompt)) return NULL;
    return s_skill_prompt;
}

void dsco_workspace_prompt_invalidate(void) {
    free(s_workspace_prompt);
    s_workspace_prompt = NULL;
    s_workspace_prompt_loaded = false;
    free(s_skill_prompt);
    s_skill_prompt = NULL;
}
