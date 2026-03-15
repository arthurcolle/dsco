#ifndef DSCO_WORKSPACE_H
#define DSCO_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int installed_skills;
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
int dsco_workspace_read_doc(const char *name, char *out, size_t out_len);
void dsco_workspace_doc_path(const char *name, char *out, size_t out_len);
const char *dsco_workspace_prompt(void);
const char *dsco_workspace_skill_prompt(const char *name);
void dsco_workspace_prompt_invalidate(void);

#endif
