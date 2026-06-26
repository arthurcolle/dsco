#ifndef DSCO_CODEX_APP_DIRECTORY_H
#define DSCO_CODEX_APP_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "integration_fabric.h"

#define DSCO_CODEX_APP_FIELD_MAX 256

typedef struct {
    char id[DSCO_CODEX_APP_FIELD_MAX];
    char connector_id[DSCO_CODEX_APP_FIELD_MAX];
    char display_name[DSCO_CODEX_APP_FIELD_MAX];
    char distribution_channel[64];
    char categories[DSCO_CODEX_APP_FIELD_MAX];
    char labels[DSCO_CODEX_APP_FIELD_MAX];
    char scope[64];
    char catalog_status[64];
    bool is_enabled;
    bool is_accessible;
    bool interactive;
    bool consequential;
    bool retrievable;
    bool sync;
    bool stale;
    unsigned action_flags;
} codex_app_directory_entry_t;

typedef struct {
    codex_app_directory_entry_t *entries;
    size_t count;
    size_t cap;
    size_t total_seen;
    size_t enabled_count;
    size_t accessible_count;
    size_t interactive_count;
    size_t consequential_count;
    size_t retrievable_count;
    size_t sync_count;
    size_t stale_count;
    char source_path[1024];
} codex_app_directory_t;

void codex_app_directory_init(codex_app_directory_t *dir);
void codex_app_directory_free(codex_app_directory_t *dir);
bool codex_app_directory_load_file(codex_app_directory_t *dir, const char *path, char *err, size_t err_len);
const codex_app_directory_entry_t *codex_app_directory_find(const codex_app_directory_t *dir,
                                                            const char *id_or_name);
unsigned codex_app_directory_actions_for_labels(bool retrievable, bool sync,
                                                bool consequential, bool interactive);
const char *codex_app_directory_default_path(char *buf, size_t buf_len);

#endif /* DSCO_CODEX_APP_DIRECTORY_H */
