#include "ltask.h"

#include "lua/lua.h"
#include "lua/lapi.h"
#include "lua/lstring.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"

typedef struct ltask_ctx{
    int32_t ref;
    lua_State *lua;
    char file[PATH_LENS];
}ltask_ctx;
static srey_ctx *srey;
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
    if (NULL != task) {
        lua_pushlightuserdata(lua, task);
        lua_setglobal(lua, "_curtask");
    }
    return lua;
}
static inline int32_t _ltask_dofile(lua_State *lua, const char *file) {
    char path[PATH_LENS] = { 0 };
    SNPRINTF(path, sizeof(path) - 1, "%s%s", luapath, file);
    if (LUA_OK != luaL_dofile(lua, path)) {
        LOG_ERROR("%s", lua_tostring(lua, -1));
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t ltask_startup(srey_ctx *ctx) {
    srey = ctx;
    ASSERTAB(ERR_OK == procpath(propath), "procpath failed.");
    SNPRINTF(luapath, sizeof(luapath) - 1, "%s%s%s%s",
        propath, PATH_SEPARATORSTR, "script", PATH_SEPARATORSTR);
    lua_State *lua = _ltask_luainit(NULL);
    if (NULL == lua) {
        return ERR_FAILED;
    }
    int32_t rtn = _ltask_dofile(lua, "startup.lua");
    lua_close(lua);
    return rtn;
}

static int32_t _ltask_log(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)lua_tointeger(lua, 1);
    const char *file = lua_tostring(lua, 2);
    int32_t line = (int32_t)lua_tointeger(lua, 3);
    const char *log = lua_tostring(lua, 4);
    loger_log(&g_logerctx, lv, "[%s][%s %d]%s", _getlvstr(lv), __FILENAME__(file), line, log);
    return 0;
}
static inline void *_ltask_new(task_ctx *task, void *arg) {
    ltask_ctx *ltask = arg;
    ltask->lua = _ltask_luainit(task);
    ASSERTAB(NULL != ltask->lua, "init lua error.");
    ASSERTAB(ERR_OK == _ltask_dofile(ltask->lua, ltask->file), "lua dofile error.");
    lua_getglobal(ltask->lua, "_dispatch_message");
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
    lua_rawgeti(ltask->lua, LUA_REGISTRYINDEX, ltask->ref);
    lua_pushinteger(ltask->lua, msg->type);
    lua_pushinteger(ltask->lua, msg->ptype);
    lua_pushinteger(ltask->lua, msg->error);
    lua_pushinteger(ltask->lua, msg->fd);
    lua_pushlightuserdata(ltask->lua, msg->src);
    lua_pushlightuserdata(ltask->lua, msg->data);
    lua_pushinteger(ltask->lua, msg->size);
    lua_pushinteger(ltask->lua, msg->session);
    lua_pushlightuserdata(ltask->lua, &msg->addr);
    if (LUA_OK != lua_pcall(ltask->lua, 9, 0, 0)) {
        LOG_ERROR("%s", lua_tostring(ltask->lua, 1));
    }
}
static int32_t _ltask_register(lua_State *lua) {
    const char *file = lua_tostring(lua, 1);
    int32_t name = (int32_t)lua_tointeger(lua, 2);
    uint32_t maxcnt = 3;
    if (LUA_TNIL != lua_type(lua, 3)) {
        maxcnt = (uint32_t)lua_tointeger(lua, 3);
    }
    maxcnt = (0 == maxcnt ? 1 : maxcnt);
    ltask_ctx *ltask;
    MALLOC(ltask, sizeof(ltask_ctx));
    ZERO(ltask, sizeof(ltask_ctx));
    strcpy(ltask->file, file);
    lua_pushlightuserdata(lua, srey_tasknew(srey, name, maxcnt, 
        _ltask_new, _ltask_run, _ltask_free, ltask));
    return 1;
}
static int32_t _ltask_qury(lua_State *lua) {
    int32_t name = (int32_t)lua_tointeger(lua, 1);
    task_ctx *task = srey_taskqury(srey, name);
    NULL == task ? lua_pushnil(lua) : lua_pushlightuserdata(lua, task);
    return 1;
}
static int32_t _ltask_name(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    lua_pushinteger(lua, task_name(task));
    return 1;
}
static int32_t _ltask_session(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    lua_pushinteger(lua, task_session(task));
    return 1;
}
static int32_t _ltask_user(lua_State *lua) {
    task_ctx *dst = lua_touserdata(lua, 1);
    task_ctx *src = lua_touserdata(lua, 2);
    uint64_t session = (uint64_t)lua_tointeger(lua, 3);
    size_t size = 0;
    void *data = (void *)lua_tolstring(lua, 4, &size);
    task_user(dst, src, session, data, size, 1);
    return 0;
}
static int32_t _ltask_timeout(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    uint64_t session = (uint64_t)lua_tointeger(lua, 2);
    uint32_t time = (uint32_t)lua_tointeger(lua, 3);
    task_timeout(task, session, time);
    return 0;
}
#if WITH_SSL
static int32_t _ltask_sslevnew(lua_State *lua) {
    const char *name = lua_tostring(lua, 1);
    const char *ca = lua_tostring(lua, 2);
    const char *cert = lua_tostring(lua, 3);
    const char *key = lua_tostring(lua, 4);
    int32_t keytype = (int32_t)lua_tointeger(lua, 5);
    char capath[PATH_LENS] = { 0 };
    char certpath[PATH_LENS] = { 0 };
    char keypath[PATH_LENS] = { 0 };
    if (0 != strlen(ca)){
        SNPRINTF(capath, sizeof(capath) - 1, "%s%s%s%s%s", 
            propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, ca);
    }
    if (0 != strlen(cert)) {
        SNPRINTF(certpath, sizeof(certpath) - 1, "%s%s%s%s%s", 
            propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, cert);
    }
    if (0 != strlen(key)) {
        SNPRINTF(keypath, sizeof(keypath) - 1, "%s%s%s%s%s", 
            propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, key);
    }
    evssl_ctx *ssl = evssl_new(capath, certpath, keypath, keytype, NULL);
    certs_register(srey, name, ssl);
    lua_pushlightuserdata(lua, ssl);
    return 1;
}
static int32_t _ltask_sslevp12new(lua_State *lua) {
    const char *name = lua_tostring(lua, 1);
    const char *p12 = lua_tostring(lua, 2);
    const char *pwd = lua_tostring(lua, 3);
    char p12path[PATH_LENS] = { 0 };
    SNPRINTF(p12path, sizeof(p12path) - 1, "%s%s%s%s%s", 
        propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, p12);
    evssl_ctx *ssl = evssl_p12_new(p12path, pwd, NULL);
    certs_register(srey, name, ssl);
    lua_pushlightuserdata(lua, ssl);
    return 1;
}
static int32_t _ltask_sslevqury(lua_State *lua) {
    const char *name = lua_tostring(lua, 1);
    struct evssl_ctx *ssl = certs_qury(srey, name);
    if (NULL == ssl) {
        lua_pushnil(lua);
        LOG_WARN("not find cert, name:%s.", name);
    } else {
        lua_pushlightuserdata(lua, ssl);
    }
    return 1;
}
#endif
static int32_t _ltask_listen(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    int32_t ptype = (int32_t)lua_tointeger(lua, 2);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 3)) {
        evssl = lua_touserdata(lua, 3);
    }
    const char *host = lua_tostring(lua, 4);
    uint16_t port = (uint16_t)lua_tointeger(lua, 5);
    int32_t sendev = (int32_t)lua_tointeger(lua, 6);
    if (ERR_OK == task_netlisten(task, ptype, evssl, host, port, sendev)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
static int32_t _ltask_connect(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    int32_t ptype = (int32_t)lua_tointeger(lua, 2);
    uint64_t session = (uint64_t)lua_tointeger(lua, 3);
    struct evssl_ctx *evssl = lua_touserdata(lua, 4);
    const char *host = lua_tostring(lua, 5);
    uint16_t port = (uint16_t)lua_tointeger(lua, 6);
    int32_t sendev = (int32_t)lua_tointeger(lua, 7);
    SOCKET fd = task_netconnect(task, ptype, session, evssl, host, port, sendev);
    if (INVALID_SOCK != fd) {
        lua_pushinteger(lua, fd);
    } else {
        lua_pushinteger(lua, -1);
    }
    return 1;
}
static int32_t _ltask_udp(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    int32_t ptype = (int32_t)lua_tointeger(lua, 2);
    const char *host = lua_tostring(lua, 3);
    uint16_t port = (uint16_t)lua_tointeger(lua, 4);
    SOCKET fd = task_netudp(task, ptype, host, port);
    if (INVALID_SOCK != fd) {
        lua_pushinteger(lua, fd);
    } else {
        lua_pushinteger(lua, -1);
    }
    return 1;
}
static int32_t _ltask_send(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)lua_tointeger(lua, 2);
    size_t size;
    void *data;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)lua_tolstring(lua, 3, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)lua_tointeger(lua, 4);
    }
    pack_type ptype = (pack_type)lua_tointeger(lua, 5);
    task_netsend(task, fd, data, size, ptype);
    return 0;
}
static int32_t _ltask_sendto(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)lua_tointeger(lua, 2);
    const char *host = lua_tostring(lua, 3);
    uint16_t port = (uint16_t)lua_tointeger(lua, 4);
    size_t size = 0;
    void *data;
    if (LUA_TSTRING == lua_type(lua, 5)) {
        data = (void *)lua_tolstring(lua, 5, &size);
    } else {
        data = lua_touserdata(lua, 5);
        size = (size_t)lua_tointeger(lua, 6);
    }
    ev_sendto(task_netev(task), fd, host, port, data, size);
    return 0;
}
static int32_t _ltask_close(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)lua_tointeger(lua, 2);
    ev_close(task_netev(task), fd);
    return 0;
}
static int32_t _ltask_ipport(lua_State *lua) {
    netaddr_ctx *addr = lua_touserdata(lua, 1);
    char ip[IP_LENS];
    if (ERR_OK != netaddr_ip(addr, ip)) {
        lua_pushnil(lua);
        return 1;
    }
    uint16_t port = netaddr_port(addr);
    lua_pushstring(lua, ip);
    lua_pushinteger(lua, port);
    return 2;
}
LUAMOD_API int luaopen_srey(lua_State *lua) {
    luaL_Reg reg[] = {
        { "log", _ltask_log },
        { "register", _ltask_register },
        { "qury", _ltask_qury },
        { "name", _ltask_name },
        { "session", _ltask_session },
        { "user", _ltask_user },
        { "timeout", _ltask_timeout },
#if WITH_SSL
        { "sslevnew", _ltask_sslevnew },
        { "sslevp12new", _ltask_sslevp12new },
        { "sslevqury", _ltask_sslevqury },
#endif
        { "listen", _ltask_listen },
        { "connect", _ltask_connect },
        { "udp", _ltask_udp },
        { "send", _ltask_send },
        { "sendto", _ltask_sendto },
        { "close", _ltask_close },
        { "ipport", _ltask_ipport },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
