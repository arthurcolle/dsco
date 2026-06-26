/* src/extension/skill_requirements.c */
#include "extension/skill_requirements.h"
#include <string.h>

int skill_requirements_init(skill_requirements_t *req) {
    memset(req, 0, sizeof(*req));
    return 0;
}

int skill_requirements_add(skill_requirements_t *req, backend_category_t cat, const char *name,
                           int mandatory) {
    if (req->count >= MAX_BACKEND_REQUIREMENTS)
        return -1;
    req->backends[req->count].category = cat;
    req->backends[req->count].required_name = name;
    req->backends[req->count].mandatory = mandatory;
    req->count++;
    return 0;
}

int skill_requirements_satisfy(const skill_requirements_t *req) {
    for (int i = 0; i < req->count; i++) {
        const backend_requirement_t *r = &req->backends[i];
        backend_interface_t *b = backend_get(r->required_name ? r->required_name : "", r->category);
        if (!b && r->mandatory)
            return 0;
    }
    return 1;
}