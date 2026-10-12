#ifndef DSCO_WORKSPACE_H
#define DSCO_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int installed_skills;
    int active_doctrines;
    int agents_files;
    bool has_identity;
    bool has_user;
    bool has_soul;
    bool has_memory;
    bool has_legacy_prompt;
} dsco_workspace_status_t;

const char *dsco_workspace_root(void);
int dsco_workspace_bootstrap(char *summary, size_t summary_len);
int dsco_workspace_status(dsco_workspace_status_t *status, char *summary, size_t summary_len);
int dsco_workspace_list_skills(char *out, size_t out_len);
int dsco_workspace_show_skill(const char *name, char *out, size_t out_len);
bool dsco_workspace_skill_exists(const char *name);
int dsco_workspace_create_skill(const char *name, const char *body, bool overwrite);
int dsco_workspace_delete_skill(const char *name, bool force);
int dsco_workspace_count_auto_skills(void);
/* Fill `names` with the directory names of marker-verified auto-generated
 * skills (max entries of 128 bytes each). Returns the count filled. */
int dsco_workspace_list_auto_skills(char (*names)[128], int max);
int dsco_workspace_read_doc(const char *name, char *out, size_t out_len);
void dsco_workspace_doc_path(const char *name, char *out, size_t out_len);
const char *dsco_workspace_prompt(void);
/* Lean prompt view for self-contained direct answers. It retains explicit
 * legacy/project instructions and persistent directives while omitting
 * identity/history/calibration documents that do not drive tool-free work. */
const char *dsco_workspace_direct_prompt(void);
const char *dsco_workspace_skill_prompt(const char *name);
/* Owned request-scoped context; changed/missing/unpinned versions withhold the
 * instructions and return a visible reselection notice. NULL = no selection. */
char *dsco_workspace_skill_context(const char *name, const char *expected_sha256,
                                  const char *selected_provider, const char *request_provider);
void dsco_workspace_prompt_invalidate(void);

#endif
