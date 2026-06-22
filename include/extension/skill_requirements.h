/* extension/skill_requirements.h — Skill Backend Requirements */
#ifndef DSCO_EXTENSION_SKILL_REQUIREMENTS_H
#define DSCO_EXTENSION_SKILL_REQUIREMENTS_H

#include "backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BACKEND_REQUIREMENTS 8

typedef struct skill_requirements {
    backend_requirement_t backends[MAX_BACKEND_REQUIREMENTS];
    int count;
} skill_requirements_t;

int skill_requirements_init(skill_requirements_t *req);
int skill_requirements_add(skill_requirements_t *req,
                           backend_category_t cat,
                           const char *name,
                           int mandatory);
int skill_requirements_satisfy(const skill_requirements_t *req);

#ifdef __cplusplus
}
#endif
#endif /* DSCO_EXTENSION_SKILL_REQUIREMENTS_H */