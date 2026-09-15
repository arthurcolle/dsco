#include "lingo.h"
#include "tools.h"
#include "json_util.h"
#include "crypto.h"
#include "lingo_origin.h"
#include "lingo_session.h"
#include "lingo_workbench.h"
#include "event_stream.h"
#include "chronicle.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

const char lingo_tool_schema[] = "{\"type\":\"object\",\"properties\":{"
                                 "\"source\":{\"type\":\"string\",\"description\":\"Lua 5.1 "
                                 "source; mutually exclusive with path\"},"
                                 "\"path\":{\"type\":\"string\",\"description\":\"Local .lingo "
                                 "source file; mutually exclusive with source\"},"
                                 "\"args\":{\"type\":\"object\"},\"check\":{\"type\":\"boolean\","
                                 "\"description\":\"Syntax check only\"}},"
                                 "\"additionalProperties\":false,\"oneOf\":[{\"required\":["
                                 "\"source\"]},{\"required\":[\"path\"]}]}";

static bool failure(char *out, size_t cap, const char *message) {
    jbuf_t b;
    jbuf_init(&b, 128);
    jbuf_append(&b, "{\"error\":");
    jbuf_append_json_str(&b, message);
    jbuf_append(&b, "}");
    if (cap)
        snprintf(out, cap, "%s", b.data);
    jbuf_free(&b);
    return false;
}

#ifdef HAVE_LUAJIT
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luajit.h>
#include "lingo_runtime.gen.h"
#include "lingo_operator.gen.h"
#include "lingo_dsco.gen.h"
#include "lingo_autobot.gen.h"
#include "lingo_chimera.gen.h"
#include "lingo_world_io.gen.h"
#include "lingo_platform.gen.h"
#include "lingo_workspace.gen.h"
#include "lingo_view.gen.h"
#include "lingo_workflow.gen.h"
#define SOURCE_LIMIT (256u * 1024u)
#define JSON_LIMIT (256u * 1024u)
#define MEMORY_LIMIT (32u * 1024u * 1024u)
#define INSTRUCTION_LIMIT 5000000u
static char null_key, array_key, context_key;
static _Thread_local unsigned lingo_depth;
typedef struct {
    size_t used, json_bytes, result_limit;
    unsigned instructions, calls;
    bool exhausted, check, host_calls_disabled, capture_failed;
    int view_ref;
    const char *source, *label, *tier;
    char program_label[96];
    size_t source_len;
    yyjson_val *arguments;
    yyjson_val *restore_view;
    yyjson_doc *decoded;
    yyjson_mut_doc *encoded;
    char *serialized;
    char tool_result[JSON_LIMIT];
    jbuf_t output;
} lingo_context;

struct lingo_vm {
    lua_State *state;
    lingo_context *context;
    yyjson_doc *request;
    char *owned_source;
};

static void *limited_alloc(void *ud, void *ptr, size_t old, size_t size) {
    lingo_context *c = ud;
    if (!ptr)
        old = 0;
    if (!size) {
        free(ptr);
        c->used -= old;
        return NULL;
    }
    if (size > old && size - old > MEMORY_LIMIT - c->used) {
        c->exhausted = true;
        return NULL;
    }
    void *p = realloc(ptr, size);
    if (p)
        c->used = c->used - old + size;
    return p;
}
static lingo_context *context(lua_State *L) {
    lua_pushlightuserdata(L, &context_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lingo_context *c = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return c;
}
static int budget(lua_State *L) {
    if (context(L)->capture_failed)
        return luaL_error(L, "lingo: event capture failed; execution stopped");
    if (context(L)->exhausted)
        return luaL_error(L, "lingo: execution budget exhausted");
    return 0;
}
static void tick(lua_State *L, lua_Debug *ar) {
    (void)ar;
    lingo_context *c = context(L);
    c->instructions += 1000;
    if (c->instructions >= INSTRUCTION_LIMIT)
        c->exhausted = true;
    budget(L);
}
static int absolute(lua_State *L, int i) {
    return i < 0 ? lua_gettop(L) + i + 1 : i;
}
static bool is_array(lua_State *L, int i) {
    if (!lua_getmetatable(L, i))
        return false;
    lua_pushlightuserdata(L, &array_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    bool yes = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return yes;
}
static int array(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    lua_pushlightuserdata(L, &array_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, 1);
    return 1;
}
static int array_test(lua_State *L) {
    lua_pushboolean(L, is_array(L, 1));
    return 1;
}
static void push_json(lua_State *L, yyjson_val *v, int depth) {
    if (depth > 64 || !lua_checkstack(L, 8))
        luaL_error(L, "lingo: JSON nesting limit");
    if (!v || yyjson_is_null(v))
        lua_pushlightuserdata(L, &null_key);
    else if (yyjson_is_bool(v))
        lua_pushboolean(L, yyjson_get_bool(v));
    else if (yyjson_is_num(v)) {
        double x = yyjson_get_num(v);
        if ((yyjson_is_int(v) && fabs(x) > 9007199254740991.0) || !isfinite(x))
            luaL_error(L, "lingo: JSON number outside exact integer/finite range; use string");
        lua_pushnumber(L, x);
    } else if (yyjson_is_str(v))
        lua_pushlstring(L, yyjson_get_str(v), yyjson_get_len(v));
    else {
        lua_newtable(L);
        size_t i, n;
        yyjson_val *key, *value;
        if (yyjson_is_arr(v)) {
            yyjson_arr_foreach(v, i, n, value) {
                push_json(L, value, depth + 1);
                lua_rawseti(L, -2, (int)i + 1);
            }
            lua_pushlightuserdata(L, &array_key);
            lua_rawget(L, LUA_REGISTRYINDEX);
            lua_setmetatable(L, -2);
        } else {
            yyjson_obj_foreach(v, i, n, key, value) {
                lua_pushlstring(L, yyjson_get_str(key), yyjson_get_len(key));
                lua_pushvalue(L, -1);
                lua_rawget(L, -3);
                if (!lua_isnil(L, -1))
                    luaL_error(L, "lingo: duplicate JSON key");
                lua_pop(L, 1);
                push_json(L, value, depth + 1);
                lua_rawset(L, -3);
            }
        }
    }
}
static yyjson_mut_val *to_json(lua_State *L, int at, yyjson_mut_doc *doc, int depth,
                               size_t *nodes) {
    if (depth > 64 || ++*nodes > 16384 || !lua_checkstack(L, 8))
        luaL_error(L, "lingo: JSON structure limit");
    lingo_context *c = context(L);
    c->json_bytes += 8;
    if (c->json_bytes > JSON_LIMIT)
        luaL_error(L, "lingo: JSON size limit");
    at = absolute(L, at);
    if (lua_isnil(L, at) || (lua_islightuserdata(L, at) && lua_touserdata(L, at) == &null_key))
        return yyjson_mut_null(doc);
    switch (lua_type(L, at)) {
        case LUA_TBOOLEAN:
            return yyjson_mut_bool(doc, lua_toboolean(L, at));
        case LUA_TNUMBER: {
            double x = lua_tonumber(L, at);
            if (!isfinite(x) || (floor(x) == x && fabs(x) > 9007199254740991.0))
                luaL_error(L, "lingo: JSON number outside exact integer/finite range");
            /* LuaJIT has one numeric value type. Preserve mathematical
             * integers as JSON integer tokens for existing typed tools such
             * as buffer pagination; strings are never coerced to numbers.
             * Keep IEEE negative zero when callers explicitly produce it. */
            if (floor(x) == x && (x != 0.0 || !signbit(x)))
                return yyjson_mut_sint(doc, (int64_t)x);
            return yyjson_mut_real(doc, x);
        }
        case LUA_TSTRING: {
            size_t n;
            const char *s = lua_tolstring(L, at, &n);
            c->json_bytes += n;
            if (c->json_bytes > JSON_LIMIT)
                luaL_error(L, "lingo: JSON size limit");
            return yyjson_mut_strncpy(doc, s, n);
        }
        case LUA_TTABLE: {
            size_t count = 0, length = lua_objlen(L, at);
            bool numeric = true;
            lua_pushnil(L);
            while (lua_next(L, at)) {
                if (lua_type(L, -2) != LUA_TNUMBER || lua_tonumber(L, -2) < 1 ||
                    lua_tonumber(L, -2) > (double)length ||
                    floor(lua_tonumber(L, -2)) != lua_tonumber(L, -2))
                    numeric = false;
                count++;
                lua_pop(L, 1);
            }
            bool arr = is_array(L, at) || (count && numeric);
            if (arr && (!numeric || count != length))
                luaL_error(L, "lingo: JSON array must be dense");
            yyjson_mut_val *out = arr ? yyjson_mut_arr(doc) : yyjson_mut_obj(doc);
            if (arr) {
                for (size_t i = 1; i <= length; i++) {
                    lua_rawgeti(L, at, (int)i);
                    yyjson_mut_arr_add_val(out, to_json(L, -1, doc, depth + 1, nodes));
                    lua_pop(L, 1);
                }
            } else {
                lua_pushnil(L);
                while (lua_next(L, at)) {
                    if (lua_type(L, -2) != LUA_TSTRING)
                        luaL_error(L, "lingo: JSON record keys must be strings");
                    size_t n;
                    const char *s = lua_tolstring(L, -2, &n);
                    c->json_bytes += n;
                    if (c->json_bytes > JSON_LIMIT)
                        luaL_error(L, "lingo: JSON size limit");
                    yyjson_mut_val *key = yyjson_mut_strncpy(doc, s, n);
                    yyjson_mut_obj_add(out, key, to_json(L, -1, doc, depth + 1, nodes));
                    lua_pop(L, 1);
                }
            }
            return out;
        }
        default:
            luaL_error(L, "lingo: value is not JSON; describe object references explicitly");
            return NULL;
    }
}
static int encode(lua_State *L) {
    lingo_context *c = context(L);
    if (c->encoded)
        yyjson_mut_doc_free(c->encoded);
    free(c->serialized);
    c->serialized = NULL;
    c->encoded = yyjson_mut_doc_new(NULL);
    if (!c->encoded)
        return luaL_error(L, "lingo: JSON allocation failed");
    c->json_bytes = 0;
    size_t nodes = 0, size = 0;
    yyjson_mut_doc_set_root(c->encoded, to_json(L, 1, c->encoded, 0, &nodes));
    c->serialized = yyjson_mut_write(c->encoded, 0, &size);
    if (!c->serialized || size > JSON_LIMIT)
        return luaL_error(L, "lingo: JSON size limit");
    if (c->result_limit && size > c->result_limit)
        return luaL_error(L, "lingo: session result exceeds caller capacity");
    lua_pushlstring(L, c->serialized, size);
    yyjson_mut_doc_free(c->encoded);
    c->encoded = NULL;
    free(c->serialized);
    c->serialized = NULL;
    return 1;
}
static int decode(lua_State *L) {
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    lingo_context *c = context(L);
    if (n > JSON_LIMIT)
        return luaL_error(L, "lingo: JSON size limit");
    if (c->decoded)
        yyjson_doc_free(c->decoded);
    c->decoded = yyjson_read(s, n, 0);
    if (!c->decoded)
        return luaL_error(L, "lingo: invalid JSON");
    push_json(L, yyjson_doc_get_root(c->decoded), 0);
    yyjson_doc_free(c->decoded);
    c->decoded = NULL;
    return 1;
}
static int observe(lua_State *L) {
    if (!event_stream_active()) return 0;
    lingo_context *c = context(L);
    budget(L);
    const char *event = luaL_checkstring(L, 1);
    lua_pushcfunction(L, encode);
    lua_pushvalue(L, 2);
    int status = lua_pcall(L, 1, 1, 0);
    if (status || !event_stream_emit("lingo", event, lua_tostring(L, -1))) {
        if (status) event_stream_fail("lingo_event_encoding_failed");
        c->capture_failed = c->exhausted = true;
        return luaL_error(L, "lingo: event capture failed; execution stopped");
    }
    return 0;
}
static int call(lua_State *L) {
    lingo_context *c = context(L);
    budget(L);
    if (c->host_calls_disabled)
        return luaL_error(L, "lingo: host calls are disabled during session inspection or restoration");
    size_t size;
    const char *name = luaL_checklstring(L, 1, &size);
    if (!size || size > 128 || strlen(name) != size)
        return luaL_error(L, "lingo: invalid tool name");
    luaL_checktype(L, 2, LUA_TTABLE);
    if (++c->calls > 100) {
        c->exhausted = true;
        return budget(L);
    }
    lua_pushcfunction(L, encode);
    lua_pushvalue(L, 2);
    lua_call(L, 1, 1);
    const char *input = lua_tostring(L, -1);
    if (input[0] != '{')
        return luaL_error(L, "lingo: tool arguments must be a record");
    c->tool_result[0] = 0;
    bool ok = tools_execute_for_tier(name, input, tools_execution_tier(),
                                     c->tool_result, sizeof(c->tool_result));
    lua_newtable(L);
    lua_pushboolean(L, ok);
    lua_setfield(L, -2, "ok");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "tool");
    lua_pushstring(L, c->tool_result);
    lua_setfield(L, -2, "result");
    lua_pushinteger(L, c->calls);
    lua_setfield(L, -2, "sequence");
    return 1;
}
static int output(lua_State *L) {
    lingo_context *c = context(L);
    for (int i = 1, n = lua_gettop(L); i <= n; i++) {
        size_t len;
        const char *s = lua_tolstring(L, i, &len);
        if (!s) {
            s = lua_toboolean(L, i)
                    ? (lua_isboolean(L, i) ? "true" : lua_typename(L, lua_type(L, i)))
                    : (lua_isnil(L, i) ? "nil" : "false");
            len = strlen(s);
        }
        if (c->output.len + len + 2 > 65536)
            return luaL_error(L, "lingo: print output limit");
        if (i > 1)
            jbuf_append_char(&c->output, '\t');
        jbuf_append_len(&c->output, s, len);
    }
    jbuf_append_char(&c->output, '\n');
    return 0;
}
static int require_lingo(lua_State *L) {
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    if (!n || n > 64 || strlen(s) != n)
        return luaL_error(L, "lingo: invalid module name");
    lua_pushvalue(L, 1);
    lua_rawget(L, lua_upvalueindex(1));
    if (lua_isnil(L, -1))
        return luaL_error(L, "lingo: available modules are lingo, lingo.dsco, lingo.graphsub, "
                            "lingo.operator, lingo.autobot, lingo.chimera, lingo.platform and lingo.workspace");
    return 1;
}
static void load_module(lua_State *L, int api, int modules, const char *name,
                        const unsigned char *source, size_t length, const char *label) {
    if (luaL_loadbufferx(L, (const char *)source, length, label, "t"))
        lua_error(L);
    lua_call(L, 0, 1);
    lua_pushvalue(L, api);
    lua_call(L, 1, 1);
    lua_setfield(L, modules, name);
}
static void function_field(lua_State *L, const char *name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}
static void hash_field(lua_State *L, const char *name, const unsigned char *text, size_t length) {
    char hash[65];
    sha256_hex(text, length, hash);
    lua_pushstring(L, hash); lua_setfield(L, -2, name);
}
static int register_view(lua_State *L) {
    lingo_context *c = context(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (c->view_ref != LUA_NOREF)
        return luaL_error(L, "lingo: one view may be registered per program");
    lua_pushvalue(L, 1);
    c->view_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}
static int run(lua_State *L) {
    lingo_context *c = context(L);
    luaL_openlibs(L);
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    lua_pushlightuserdata(L, &array_key);
    lua_newtable(L);
    lua_pushliteral(L, "Lingo array");
    lua_setfield(L, -2, "__metatable");
    lua_rawset(L, LUA_REGISTRYINDEX);
    if (luaL_loadbufferx(L, (const char *)lingo_runtime, sizeof(lingo_runtime) - 1,
                         "@lingo/runtime.lua", "t"))
        return lua_error(L);
    lua_call(L, 0, 1);
    lua_newtable(L);
    lingo_source_t sources[] = {
        {"@lingo/runtime.lua", "lingo", lingo_runtime, sizeof(lingo_runtime)-1},
        {"@lingo/world_io.lua", "lingo.world_io", lingo_world_io, sizeof(lingo_world_io)-1},
        {"@lingo/operator.lua", "lingo.graphsub", lingo_operator, sizeof(lingo_operator)-1},
        {"@lingo/dsco.lua", "lingo.dsco", lingo_dsco, sizeof(lingo_dsco)-1},
        {"@lingo/autobot.lua", "lingo.autobot", lingo_autobot, sizeof(lingo_autobot)-1},
        {"@lingo/chimera.lua", "lingo.chimera", lingo_chimera, sizeof(lingo_chimera)-1},
        {"@lingo/platform.lua", "lingo.platform", lingo_platform, sizeof(lingo_platform)-1},
        {"@lingo/workspace.lua", "lingo.workspace", lingo_workspace, sizeof(lingo_workspace)-1},
        {"@lingo/view.lua", "lingo.view", lingo_view, sizeof(lingo_view)-1},
        {"@lingo/workflow.lua", "lingo.workflow", lingo_workflow, sizeof(lingo_workflow)-1},
        {c->program_label, "program", (const unsigned char *)c->source, c->source_len},
    };
    lingo_origin_install(L, sources, sizeof(sources)/sizeof(*sources));
    lua_newtable(L);
    lua_pushliteral(L, "0.2"); lua_setfield(L, -2, "api");
    lua_pushliteral(L, LUAJIT_VERSION); lua_setfield(L, -2, "compiler");
    hash_field(L, "runtime_sha256", lingo_runtime, sizeof(lingo_runtime)-1);
    hash_field(L, "world_io_sha256", lingo_world_io, sizeof(lingo_world_io)-1);
    lua_setfield(L, -2, "runtime_identity");
    if (luaL_loadbufferx(L, (const char *)lingo_world_io, sizeof(lingo_world_io)-1,
                        "@lingo/world_io.lua", "t")) return lua_error(L);
    lua_call(L, 0, 1); lua_setfield(L, -2, "world_io");
    if (luaL_loadbufferx(L, (const char *)lingo_view, sizeof(lingo_view)-1,
                        "@lingo/view.lua", "t")) return lua_error(L);
    lua_call(L, 0, 1); lua_setfield(L, -2, "view");
    function_field(L, "register_view", register_view);
    if (c->restore_view) {
        push_json(L, c->restore_view, 0);
        lua_setfield(L, -2, "view_restore");
    }
    function_field(L, "encode", encode);
    function_field(L, "decode", decode);
    function_field(L, "call", call);
    if (event_stream_active()) function_field(L, "observe", observe);
    function_field(L, "check_budget", budget);
    function_field(L, "array", array);
    function_field(L, "is_array", array_test);
    lua_pushlightuserdata(L, &null_key);
    lua_setfield(L, -2, "null");
    lua_call(L, 1, 1); /* private runtime API */
    int api = lua_gettop(L);
    lua_newtable(L);
    int modules = lua_gettop(L);
    lua_pushvalue(L, api);
    lua_setfield(L, modules, "lingo");
    load_module(L, api, modules, "lingo.operator", lingo_operator,
                sizeof(lingo_operator) - 1, "@lingo/operator.lua");
    lua_getfield(L, modules, "lingo.operator");
    lua_setfield(L, modules, "lingo.graphsub");
    load_module(L, api, modules, "lingo.dsco", lingo_dsco,
                sizeof(lingo_dsco) - 1, "@lingo/dsco.lua");
    load_module(L, api, modules, "lingo.autobot", lingo_autobot,
                sizeof(lingo_autobot) - 1, "@lingo/autobot.lua");
    load_module(L, api, modules, "lingo.chimera", lingo_chimera,
                sizeof(lingo_chimera) - 1, "@lingo/chimera.lua");
    load_module(L, api, modules, "lingo.platform", lingo_platform,
                sizeof(lingo_platform) - 1, "@lingo/platform.lua");
    load_module(L, api, modules, "lingo.workspace", lingo_workspace,
                sizeof(lingo_workspace) - 1, "@lingo/workspace.lua");
    load_module(L, api, modules, "lingo.workflow", lingo_workflow,
                sizeof(lingo_workflow) - 1, "@lingo/workflow.lua");
    lua_newtable(L);
    int env = lua_gettop(L);
    const char *globals[] = {"assert",   "error",    "ipairs", "pairs",  "next", "select",
                             "tonumber", "tostring", "type",   "unpack", NULL};
    for (int i = 0; globals[i]; i++) {
        lua_getglobal(L, globals[i]);
        lua_setfield(L, env, globals[i]);
    }
    const char *libraries[] = {"string", "table", "math", "bit", NULL};
    for (int i = 0; libraries[i]; i++) {
        lua_getglobal(L, libraries[i]);
        if (!strcmp(libraries[i], "string")) {
            lua_pushnil(L);
            lua_setfield(L, -2, "dump");
        }
        if (!strcmp(libraries[i], "math")) {
            lua_pushnil(L);
            lua_setfield(L, -2, "random");
            lua_pushnil(L);
            lua_setfield(L, -2, "randomseed");
        }
        /* Separate tables prevent scripts replacing runtime helpers such as table.sort. */
        lua_newtable(L);
        lua_pushnil(L);
        while (lua_next(L, -3)) {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, -5);
            lua_pop(L, 1);
        }
        lua_setfield(L, env, libraries[i]);
        lua_pop(L, 1);
    }
    lua_pushvalue(L, modules);
    lua_pushcclosure(L, require_lingo, 1);
    lua_setfield(L, env, "require");
    function_field(L, "print", output);
    if (c->arguments)
        push_json(L, c->arguments, 0);
    else
        lua_newtable(L);
    lua_setfield(L, env, "args");
    if (luaL_loadbufferx(L, c->source, c->source_len, c->program_label, "t"))
        return lua_error(L);
    lua_pushvalue(L, env);
    lua_setfenv(L, -2);
    if (c->check) {
        lua_pop(L, 1);
        lua_pushliteral(L, "{\"syntax_valid\":true}");
        return 1;
    }
    lua_sethook(L, tick, LUA_MASKCOUNT, 1000);
    lua_call(L, 0, 1);
    budget(L);
    lua_pushcfunction(L, encode);
    lua_pushvalue(L, -2);
    lua_call(L, 1, 1);
    return 1;
}
#endif

static bool execute_internal(const char *input, const char *restore_view,
                              lingo_vm **retained, char *result, size_t capacity) {
#ifndef HAVE_LUAJIT
    (void)input;
    (void)restore_view;
    (void)retained;
    return failure(result, capacity,
                   "Lingo requires LuaJIT; rebuild with pkg-config luajit available");
#else
    if (lingo_depth)
        return failure(result, capacity, "recursive Lingo execution is disabled");
    yyjson_doc *request = yyjson_read(input, strlen(input), 0);
    yyjson_val *root = request ? yyjson_doc_get_root(request) : NULL;
    yyjson_val *sv = yyjson_obj_get(root, "source"), *pv = yyjson_obj_get(root, "path");
    yyjson_val *av = yyjson_obj_get(root, "args"), *cv = yyjson_obj_get(root, "check");
    bool valid = yyjson_is_obj(root) && (!!sv != !!pv) && (!sv || yyjson_is_str(sv)) &&
                 (!pv || yyjson_is_str(pv)) && (!av || yyjson_is_obj(av)) &&
                 (!cv || yyjson_is_bool(cv));
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, i, n, key, value) {
        const char *k = yyjson_get_str(key);
        if (strlen(k) != yyjson_get_len(key) || yyjson_obj_get(root, k) != value ||
            (strcmp(k, "source") && strcmp(k, "path") && strcmp(k, "args") && strcmp(k, "check")))
            valid = false;
    }
    if (!valid) {
        yyjson_doc_free(request);
        return failure(
            result, capacity,
            "expected exactly one of source/path, optional args object and check boolean");
    }
    char *owned = NULL;
    const char *source = yyjson_get_str(sv);
    size_t length = yyjson_get_len(sv);
    const char *path = yyjson_get_str(pv);
    if (path) {
        if (!*path || strlen(path) != yyjson_get_len(pv)) {
            yyjson_doc_free(request);
            return failure(result, capacity, "invalid source path");
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            yyjson_doc_free(request);
            return failure(result, capacity, "cannot open Lingo source path");
        }
        owned = malloc(SOURCE_LIMIT + 1);
        if (!owned) {
            fclose(f);
            yyjson_doc_free(request);
            return failure(result, capacity, "source allocation failed");
        }
        length = fread(owned, 1, SOURCE_LIMIT + 1, f);
        bool failed = ferror(f);
        fclose(f);
        source = owned;
        if (failed) {
            free(owned);
            yyjson_doc_free(request);
            return failure(result, capacity, "cannot read Lingo source");
        }
    }
    if (length > SOURCE_LIMIT || memchr(source, 0, length)) {
        free(owned);
        yyjson_doc_free(request);
        return failure(result, capacity, "source must be text, at most 256 KiB, without NUL bytes");
    }
    lingo_context *c = calloc(1, sizeof(*c));
    if (!c) {
        free(owned);
        yyjson_doc_free(request);
        return failure(result, capacity, "runtime allocation failed");
    }
    c->source = source;
    c->view_ref = LUA_NOREF;
    c->source_len = length;
    c->label = path ? path : "=lingo";
    char program_hash[65];
    sha256_hex((const unsigned char *)source, length, program_hash);
    snprintf(c->program_label, sizeof(c->program_label), "@lingo/program/%s", program_hash);
    c->arguments = av;
    yyjson_doc *restored = restore_view ? yyjson_read(restore_view, strlen(restore_view), 0) : NULL;
    c->restore_view = restored ? yyjson_doc_get_root(restored) : NULL;
    c->host_calls_disabled = restore_view != NULL;
    if (restore_view && !yyjson_is_obj(c->restore_view)) {
        free(c); free(owned); yyjson_doc_free(request); yyjson_doc_free(restored);
        return failure(result, capacity, "invalid restored view record");
    }
    c->check = yyjson_get_bool(cv);
    c->tier = tools_execution_tier();
    jbuf_init(&c->output, 256);
    lua_State *L = lua_newstate(limited_alloc, c);
    bool ok = false;
    if (!L)
        failure(result, capacity, "cannot create LuaJIT state");
    else {
        lua_pushlightuserdata(L, &context_key);
        lua_pushlightuserdata(L, c);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lingo_depth++;
        lua_pushcfunction(L, run);
        int status = lua_pcall(L, 0, 1, 0);
        lingo_depth--;
        lua_sethook(L, NULL, 0, 0);
        if (status || c->exhausted)
            failure(result, capacity,
                    c->capture_failed ? "lingo: event capture failed; execution stopped"
                    : c->exhausted ? "lingo: execution budget exhausted"
                                 : (lua_tostring(L, -1) ? lua_tostring(L, -1)
                                                        : "Lingo raised a non-string error"));
        else if (retained && c->view_ref == LUA_NOREF) {
            failure(result, capacity, "workbench programs must declare one lingo.view");
        } else {
            char hash[65];
            sha256_hex((const uint8_t *)source, length, hash);
            jbuf_t b;
            jbuf_init(&b, 1024);
            jbuf_appendf(&b,
                         "{\"api\":\"0.2\",\"engine\":\"LuaJIT\",\"profile\":\"governed-"
                         "interpreter\",\"source_sha256\":\"%s\",\"tool_calls\":%u,\"value\":",
                         hash, c->calls);
            jbuf_append(&b, lua_tostring(L, -1));
            jbuf_append(&b, ",\"output\":");
            jbuf_append_json_str(&b, c->output.data);
            jbuf_append(&b, "}");
            if (b.len >= capacity)
                failure(result, capacity, "Lingo result exceeds caller output capacity");
            else {
                memcpy(result, b.data, b.len + 1);
                ok = true;
            }
            jbuf_free(&b);
        }
        if (ok && retained) {
            lingo_vm *vm = calloc(1, sizeof(*vm));
            if (!vm) {
                ok = failure(result, capacity, "session allocation failed");
            } else {
                vm->state = L; vm->context = c; vm->request = request; vm->owned_source = owned;
                c->host_calls_disabled = true;
                c->restore_view = NULL;
                c->arguments = NULL;
                lua_settop(L, 0);
                *retained = vm;
                yyjson_doc_free(restored);
                return true;
            }
        }
        lua_close(L);
    }
    if (c->decoded)
        yyjson_doc_free(c->decoded);
    if (c->encoded)
        yyjson_mut_doc_free(c->encoded);
    free(c->serialized);
    jbuf_free(&c->output);
    free(c);
    free(owned);
    yyjson_doc_free(request);
    yyjson_doc_free(restored);
    return ok;
#endif
}

bool lingo_execute(const char *input, char *result, size_t capacity) {
    return execute_internal(input, NULL, NULL, result, capacity);
}

bool lingo_vm_open(const char *request, const char *restore_view, lingo_vm **vm,
                   char *result, size_t capacity) {
    if (!vm) return failure(result, capacity, "session output is required");
    *vm = NULL;
    return execute_internal(request, restore_view, vm, result, capacity);
}

#ifdef HAVE_LUAJIT
static int command(lua_State *L) {
    lingo_context *c = context(L);
    lua_sethook(L, tick, LUA_MASKCOUNT, 1000);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->view_ref);
    push_json(L, c->arguments, 0);
    lua_call(L, 1, 1);
    budget(L);
    lua_pushcfunction(L, encode);
    lua_pushvalue(L, -2);
    lua_call(L, 1, 1);
    return 1;
}
#endif

bool lingo_vm_command(lingo_vm *vm, const char *input, char *result, size_t capacity) {
#ifndef HAVE_LUAJIT
    (void)vm; (void)input;
    return failure(result, capacity, "Lingo requires LuaJIT");
#else
    if (!vm || lingo_depth) return failure(result, capacity, "invalid or recursive session command");
    lingo_context *c = vm->context;
    if (c->exhausted) return failure(result, capacity, "session budget exhausted; close and reopen");
    yyjson_doc *request = yyjson_read(input, strlen(input), 0);
    if (!request || !yyjson_is_obj(yyjson_doc_get_root(request))) {
        yyjson_doc_free(request);
        return failure(result, capacity, "session command must be a JSON object");
    }
    c->arguments = yyjson_doc_get_root(request);
    c->instructions = 0;
    c->result_limit = capacity > 0 ? capacity - 1 : 1;
    c->output.len = 0;
    c->output.data[0] = 0;
    lua_State *L = vm->state;
    lua_settop(L, 0);
    lua_pushcfunction(L, command);
    lingo_depth++;
    int status = lua_pcall(L, 0, 1, 0);
    lingo_depth--;
    lua_sethook(L, NULL, 0, 0);
    bool ok = false;
    if (status || c->exhausted)
        failure(result, capacity, c->exhausted ? "session budget exhausted; close and reopen"
            : (lua_tostring(L, -1) ? lua_tostring(L, -1) : "session command failed"));
    else {
        size_t size = 0;
        const char *encoded = lua_tolstring(L, -1, &size);
        if (!encoded || size >= capacity) failure(result, capacity, "session result exceeds capacity");
        else { memcpy(result, encoded, size + 1); ok = true; }
    }
    lua_settop(L, 0);
    c->arguments = NULL;
    yyjson_doc_free(request);
    return ok;
#endif
}

unsigned lingo_vm_calls(const lingo_vm *vm) {
#ifdef HAVE_LUAJIT
    return vm ? vm->context->calls : 0;
#else
    (void)vm; return 0;
#endif
}
void lingo_vm_view_hash(char out[65]) {
#ifdef HAVE_LUAJIT
    sha256_hex(lingo_view, sizeof(lingo_view)-1, out);
#else
    out[0] = 0;
#endif
}
void lingo_vm_record_call(lingo_vm *vm) {
#ifdef HAVE_LUAJIT
    if (vm) vm->context->calls++;
#else
    (void)vm;
#endif
}
void lingo_vm_close(lingo_vm *vm) {
#ifdef HAVE_LUAJIT
    if (!vm) return;
    lua_close(vm->state);
    lingo_context *c = vm->context;
    yyjson_doc_free(c->decoded); yyjson_mut_doc_free(c->encoded);
    free(c->serialized); jbuf_free(&c->output); free(c);
    yyjson_doc_free(vm->request); free(vm->owned_source); free(vm);
#else
    (void)vm;
#endif
}

static int stream_fd(const char *text) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !text[0] || *end || value < 3 || value > INT_MAX) return -1;
    return (int)value;
}
static int lingo_usage(void) {
    fprintf(stderr,
        "usage: dsco lingo run FILE [ARGS_JSON] [--events-fd FD --events-db NEW_ABSOLUTE_PATH]\n"
        "       dsco lingo eval SOURCE [ARGS_JSON] [--events-fd FD --events-db NEW_ABSOLUTE_PATH]\n"
        "       dsco lingo check FILE\n"
        "       dsco lingo replay EVENTS_DB --events-fd FD [--after SEQUENCE]\n"
        "       dsco lingo open FILE [ARGS_JSON] [--restore SESSION_FILE] [--json]\n");
    return 2;
}
int lingo_cli(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[2], "open"))
        return lingo_workbench_cli(argc, argv);
    if (argc == 3 && !strcmp(argv[2], "--help")) { lingo_usage(); return 0; }
    if (argc < 4) return lingo_usage();
    bool replay = !strcmp(argv[2], "replay");
    if (!replay && strcmp(argv[2], "run") && strcmp(argv[2], "eval") &&
        strcmp(argv[2], "check")) return lingo_usage();
    const char *args_json = NULL, *events_db = NULL;
    int events_fd = -1;
    uint64_t after = 0;
    bool has_after = false;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--events-fd") && i + 1 < argc && events_fd < 0) {
            events_fd = stream_fd(argv[++i]);
            if (events_fd < 0) return lingo_usage();
        } else if (!strcmp(argv[i], "--events-db") && i + 1 < argc && !events_db && !replay)
            events_db = argv[++i];
        else if (!strcmp(argv[i], "--after") && i + 1 < argc && !has_after && replay) {
            const char *text = argv[++i]; char *end = NULL;
            errno = 0; unsigned long long value = strtoull(text, &end, 10);
            if (errno || !text[0] || text[0] == '-' || *end) return lingo_usage();
            after = (uint64_t)value; has_after = true;
        } else if (!args_json && argv[i][0] != '-' && !replay) args_json = argv[i];
        else return lingo_usage();
    }
    if (replay) {
        if (events_fd < 0) return lingo_usage();
        char receipt[8192] = {0};
        bool ok = event_stream_replay(argv[3], events_fd, after, receipt, sizeof(receipt));
        if (receipt[0]) puts(receipt);
        else fprintf(stderr, "event replay failed\n");
        return ok ? 0 : 1;
    }
    if ((events_fd >= 0) != (events_db != NULL)) return lingo_usage();
    const char *preserve = getenv("DSCO_SWARM_PRESERVE_CHILDREN");
    if (events_fd >= 0 && preserve && preserve[0] && preserve[0] != '0') {
        fprintf(stderr, "one-shot event capture requires owned worker cleanup; unset DSCO_SWARM_PRESERVE_CHILDREN\n");
        return 2;
    }
    jbuf_t in;
    jbuf_init(&in, 512);
    jbuf_append(&in, !strcmp(argv[2], "eval") ? "{\"source\":" : "{\"path\":");
    jbuf_append_json_str(&in, argv[3]);
    if (!strcmp(argv[2], "check")) jbuf_append(&in, ",\"check\":true");
    if (args_json) {
        yyjson_doc *d = yyjson_read(args_json, strlen(args_json), 0);
        if (!d || !yyjson_is_obj(yyjson_doc_get_root(d))) {
            yyjson_doc_free(d); jbuf_free(&in);
            fprintf(stderr, "args must be a JSON object\n"); return 2;
        }
        yyjson_doc_free(d);
        jbuf_append(&in, ",\"args\":"); jbuf_append(&in, args_json);
    }
    jbuf_append(&in, "}");
    char *out = calloc(1, 512 * 1024);
    if (!out) { jbuf_free(&in); return 1; }
    if (events_fd >= 0) {
        char error[512] = {0};
        int flags = fcntl(events_fd, F_GETFD);
        if (flags < 0 || fcntl(events_fd, F_SETFD, flags | FD_CLOEXEC) < 0 ||
            !event_stream_open(events_db, events_fd, error, sizeof(error))) {
            fprintf(stderr, "cannot open event stream: %s\n", error[0] ? error : strerror(errno));
            free(out); jbuf_free(&in); return 2;
        }
        chronicle_start(&(chronicle_start_opts_t){.provider="native", .model="LuaJIT", .mode="lingo"});
        (void)event_stream_emit("stream", "stream.opened", "{\"scope\":\"lingo_native_process_tree\"}");
    }
    tools_init_scripting();
    bool ok = tools_execute("lingo", in.data, out, 512 * 1024);
    char stream_receipt[8192] = {0};
    if (events_fd >= 0) {
        /* atexit would run after the terminal stream watermark. Seal only once
         * owned workers have been drained/reaped and session events recorded. */
        tools_finish_scripting();
        chronicle_stop();
        (void)event_stream_emit("stream", "stream.closed",
                                ok ? "{\"program_ok\":true}" : "{\"program_ok\":false}");
        if (!event_stream_close(stream_receipt, sizeof(stream_receipt))) ok = false;
    }
    yyjson_doc *d = yyjson_read(out, strlen(out), 0);
    jbuf_t b; jbuf_init(&b, strlen(out) + 8192);
    if (d && yyjson_is_obj(yyjson_doc_get_root(d))) {
        size_t size = strlen(out);
        while (size && (out[size-1] == '\n' || out[size-1] == ' ' || out[size-1] == '\r')) size--;
        jbuf_append_len(&b, out, size - 1);
        if (stream_receipt[0]) { jbuf_append(&b, ",\"stream\":"); jbuf_append(&b, stream_receipt); }
        jbuf_append(&b, "}");
    } else {
        jbuf_append(&b, "{\"error\":"); jbuf_append_json_str(&b, out);
        if (stream_receipt[0]) { jbuf_append(&b, ",\"stream\":"); jbuf_append(&b, stream_receipt); }
        jbuf_append(&b, "}");
    }
    puts(b.data);
    jbuf_free(&b); yyjson_doc_free(d); free(out); jbuf_free(&in);
    return ok ? 0 : 1;
}
