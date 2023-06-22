#include "ltask.h"
#if WITH_LUA
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#ifdef OS_WIN
#pragma comment(lib, "lualib.lib")
#endif

typedef struct ltask_ctx {
    int32_t ref;
    lua_State *lua;
    char file[PATH_LENS];
}ltask_ctx;
static char propath[PATH_LENS] = { 0 };
static char luapath[PATH_LENS] = { 0 };

static inline void _ltask_setpath(lua_State *lua, const char *name, const char *exname) {
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, name);
    lua_pushfstring(lua, "%s;%s?.%s", lua_tostring(lua, -1), luapath, exname);
    lua_setfield(lua, -3, name);
    lua_pop(lua, 2);
}
static inline lua_State *_ltask_luainit(task_ctx *task) {
    lua_State *lua = luaL_newstate();
    if (NULL == lua) {
        LOG_ERROR("%s", "luaL_newstate failed.");
        return NULL;
    }
    luaL_openlibs(lua);
    _ltask_setpath(lua, "cpath", DLL_EXNAME);
    _ltask_setpath(lua, "path", "lua");
    lua_pushstring(lua, propath);
    lua_setglobal(lua, "_propath");
    lua_pushstring(lua, PATH_SEPARATORSTR);
    lua_setglobal(lua, "_pathsep");
    if (NULL != task) {
        lua_pushlightuserdata(lua, task);
        lua_setglobal(lua, "_curtask");
    }
    return lua;
}
static inline void _ltask_fmtfile(const char *file, char *path) {
    ZERO(path, PATH_LENS);
    SNPRINTF(path, PATH_LENS - 1, "%s%s.lua", luapath, file);
}
static inline int32_t _ltask_searchfile(const char *file, char *path) {
    _ltask_fmtfile(file, path);
    if (ERR_OK == isfile(path)) {
        return ERR_OK;
    }
    char tmp[PATH_LENS] = { 0 };
    strcpy(tmp, file);
    size_t lens = strlen(tmp);
    for (size_t i = 0; i < lens; i++) {
        if ('.' == tmp[i]) {
            tmp[i] = PATH_SEPARATOR;
            _ltask_fmtfile(tmp, path);
            if (ERR_OK == isfile(path)) {
                return ERR_OK;
            }
        }
    }
    return ERR_FAILED;
}
static inline int32_t _ltask_dofile(lua_State *lua, const char *file) {
    char path[PATH_LENS];
    if (ERR_OK != _ltask_searchfile(file, path)) {
        LOG_ERROR("cannot find %s:, no such file.", file);
        return ERR_FAILED;
    }
    if (LUA_OK != luaL_dofile(lua, path)) {
        LOG_ERROR("%s", lua_tostring(lua, -1));
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t _ltask_startup(void) {
    PRINT(LUA_RELEASE);
    ASSERTAB(ERR_OK == procpath(propath), "procpath failed.");
    SNPRINTF(luapath, sizeof(luapath) - 1, "%s%s%s%s",
             propath, PATH_SEPARATORSTR, "script", PATH_SEPARATORSTR);
    lua_State *lua = _ltask_luainit(NULL);
    if (NULL == lua) {
        return ERR_FAILED;
    }
    int32_t rtn = _ltask_dofile(lua, "startup");
    lua_close(lua);
    return rtn;
}
static inline void *_ltask_new(task_ctx *task, void *arg) {
    ltask_ctx *ltask = arg;
    ltask->lua = _ltask_luainit(task);
    ASSERTAB(NULL != ltask->lua, "init lua error.");
    ASSERTAB(ERR_OK == _ltask_dofile(ltask->lua, ltask->file), "lua dofile error.");
    lua_getglobal(ltask->lua, "dispatch_message");
    ASSERTAB(LUA_TFUNCTION == lua_type(ltask->lua, 1), "not have function _dispatch_message.");
    ltask->ref = luaL_ref(ltask->lua, LUA_REGISTRYINDEX);
    return ltask;
}
static inline void _ltask_free(task_ctx *task) {
    ltask_ctx *ltask = task_handle(task);
    if (0 != ltask->ref) {
        luaL_unref(ltask->lua, LUA_REGISTRYINDEX, ltask->ref);
    }
    lua_close(ltask->lua);
    FREE(ltask);
}
static inline void _ltask_run(task_ctx *task, message_ctx *msg) {
    ltask_ctx *ltask = task_handle(task);
    if (0 == ltask->ref) {
        return;
    }
    lua_State *lua = ltask->lua;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, ltask->ref);
    lua_pushinteger(lua, msg->msgtype);
    lua_pushlightuserdata(lua, msg);
    if (LUA_OK != lua_pcall(lua, 2, 0, 0)) {
        LOG_ERROR("%s", lua_tostring(lua, 1));
    }
}
static int32_t _ltask_register(lua_State *lua) {
    const char *file = luaL_checkstring(lua, 1);
    int32_t name = (int32_t)luaL_checkinteger(lua, 2);
    uint32_t maxcnt = (uint32_t)luaL_checkinteger(lua, 3);
    ltask_ctx *ltask;
    CALLOC(ltask, 1, sizeof(ltask_ctx));
    strcpy(ltask->file, file);
    task_ctx *task = srey_tasknew(srey, name, maxcnt,
                                  _ltask_new, _ltask_run, _ltask_free, ltask);
    NULL == task ? lua_pushnil(lua) : lua_pushlightuserdata(lua, task);
    return 1;
}
LUAMOD_API int luaopen_srey(lua_State *lua) {
    luaL_Reg reg[] = {
        { "task_register", _ltask_register },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}

#endif
