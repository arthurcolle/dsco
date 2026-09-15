#ifndef DSCO_LINGO_ORIGIN_H
#define DSCO_LINGO_ORIGIN_H
#include <stddef.h>
typedef struct lua_State lua_State;
typedef struct {
    const char *label;
    const char *name;
    const unsigned char *text;
    size_t length;
} lingo_source_t;
/* Populate private host functions; the host table is at the top of the stack. */
void lingo_origin_install(lua_State *L, const lingo_source_t *sources, size_t count);
#endif
