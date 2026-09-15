#include "lingo_origin.h"
#ifdef HAVE_LUAJIT
#include "crypto.h"
#include "json_util.h"
#include <lua.h>
#include <lauxlib.h>
#include <stdbool.h>
#include <string.h>

static char origins_key;
static char dump_key;
static int fingerprint(lua_State *L) {
    size_t size;
    const char *text = luaL_checklstring(L, 1, &size);
    if (size > 256u * 1024u)
        return luaL_error(L, "lingo: fingerprint input exceeds 256 KiB");
    char hash[65];
    sha256_hex((const unsigned char *)text, size, hash);
    lua_pushstring(L, hash);
    return 1;
}
static int source(lua_State *L, const lua_Debug *debug) {
    lua_pushlightuserdata(L, &origins_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, debug->source ? debug->source : "");
    if (!lua_istable(L, -1))
        return luaL_error(L, "lingo: declaration has no registered source identity");
    /* Return a copy; internal source identities cannot be rebound by callers. */
    lua_newtable(L);
    lua_getfield(L, -2, "module"); lua_setfield(L, -2, "module");
    lua_getfield(L, -2, "sha256"); lua_setfield(L, -2, "sha256");
    lua_pushinteger(L, debug->linedefined); lua_setfield(L, -2, "line");
    lua_pushinteger(L, debug->lastlinedefined); lua_setfield(L, -2, "last_line");
    return 1;
}
static int caller_source(lua_State *L) {
    lua_Debug debug;
    /* C host -> world:define -> actual defining program/library. */
    if (!lua_getstack(L, 2, &debug) || !lua_getinfo(L, "S", &debug))
        return luaL_error(L, "lingo: cannot identify declaration source");
    return source(L, &debug);
}
static int function_source(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (lua_iscfunction(L, 1))
        return luaL_error(L, "lingo: derived native functions need an explicit kernel binding");
    /* lua_dump uses hardened randomized table order. LuaJIT's documented
     * string.dump mode 'd' canonicalizes it for reproducible identities.
     * Keep the original function private; programs cannot dump/load bytecode. */
    lua_pushlightuserdata(L, &dump_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 1);
    lua_pushliteral(L, "d");
    lua_call(L, 2, 1);
    size_t length = 0;
    const char *bytes = lua_tolstring(L, -1, &length);
    char hash[65];
    if (!bytes || length > 256u * 1024u)
        return luaL_error(L, "lingo: derived function prototype exceeds identity limit");
    sha256_hex((const unsigned char *)bytes, length, hash);
    lua_pop(L, 1);
    lua_Debug debug;
    lua_pushvalue(L, 1);
    if (!lua_getinfo(L, ">S", &debug))
        return luaL_error(L, "lingo: cannot identify derived function source");
    source(L, &debug);
    lua_pushstring(L, hash); lua_setfield(L, -2, "prototype_sha256");
    return 1;
}
void lingo_origin_install(lua_State *L, const lingo_source_t *sources, size_t count) {
    lua_pushlightuserdata(L, &dump_key);
    lua_getglobal(L, "string");
    lua_getfield(L, -1, "dump");
    lua_remove(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(L, &origins_key);
    lua_newtable(L);
    for (size_t i = 0; i < count; ++i) {
        char hash[65];
        sha256_hex(sources[i].text, sources[i].length, hash);
        lua_newtable(L);
        lua_pushstring(L, sources[i].name); lua_setfield(L, -2, "module");
        lua_pushstring(L, hash); lua_setfield(L, -2, "sha256");
        lua_setfield(L, -2, sources[i].label);
    }
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushcfunction(L, caller_source); lua_setfield(L, -2, "caller_source");
    lua_pushcfunction(L, function_source); lua_setfield(L, -2, "function_source");
    lua_pushcfunction(L, fingerprint); lua_setfield(L, -2, "fingerprint");
}
#endif
