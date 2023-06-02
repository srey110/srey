#include "ltask.h"
#include "lua/lua.h"
#include "lua/lapi.h"
#include "lua/lstring.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"
#include "proto/http.h"
#include "proto/simple.h"

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
int32_t ltask_startup(srey_ctx *ctx) {
    srey = ctx;
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
static int32_t _ltask_log(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
    const char *file = luaL_checkstring(lua, 2);
    int32_t line = (int32_t)luaL_checkinteger(lua, 3);
    const char *log = luaL_checkstring(lua, 4);
    loger_log(&g_logerctx, lv, "[%s][%s %d]%s", _getlvstr(lv), __FILENAME__(file), line, log);
    return 0;
}
static int32_t _ltask_setlog(lua_State *lua) {
    if (LUA_TNIL != lua_type(lua, 1)) {
        LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
        SETLOGLV(lv);
    }
    if (LUA_TNIL != lua_type(lua, 2)) {
        int32_t prt = (int32_t)luaL_checkinteger(lua, 2);
        SETLOGPRT(prt);
    }
    return 0;
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
    lua_rawgeti(ltask->lua, LUA_REGISTRYINDEX, ltask->ref);
    lua_pushinteger(ltask->lua, msg->msgtype);
    lua_pushinteger(ltask->lua, msg->pktype);
    lua_pushinteger(ltask->lua, msg->error);
    lua_pushinteger(ltask->lua, msg->fd);
    if (NULL == msg->src) {
        lua_pushnil(ltask->lua);
    } else {
        lua_pushlightuserdata(ltask->lua, msg->src);
    }
    if (NULL == msg->data) {
        lua_pushnil(ltask->lua);
    } else {
        lua_pushlightuserdata(ltask->lua, msg->data);
    }
    lua_pushinteger(ltask->lua, msg->size);
    lua_pushinteger(ltask->lua, msg->session);
    lua_pushlightuserdata(ltask->lua, &msg->addr);
    if (LUA_OK != lua_pcall(ltask->lua, 9, 0, 0)) {
        LOG_ERROR("%s", lua_tostring(ltask->lua, 1));
    }
}
static int32_t _ltask_register(lua_State *lua) {
    const char *file = luaL_checkstring(lua, 1);
    int32_t name = (int32_t)luaL_checkinteger(lua, 2);
    uint32_t maxcnt = (uint32_t)luaL_checkinteger(lua, 3);
    ltask_ctx *ltask;
    CALLOC(ltask, 1, sizeof(ltask_ctx));
    strcpy(ltask->file, file);
    lua_pushlightuserdata(lua, srey_tasknew(srey, name, maxcnt, 
        _ltask_new, _ltask_run, _ltask_free, ltask));
    return 1;
}
static int32_t _ltask_qury(lua_State *lua) {
    int32_t name = (int32_t)luaL_checkinteger(lua, 1);
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
    uint64_t session = (uint64_t)luaL_checkinteger(lua, 3);
    size_t size = 0;
    void *data = (void *)luaL_checklstring(lua, 4, &size);
    task_user(dst, src, session, data, size, 1);
    return 0;
}
static int32_t _ltask_timeout(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    uint64_t session = (uint64_t)luaL_checkinteger(lua, 2);
    uint32_t time = (uint32_t)luaL_checkinteger(lua, 3);
    task_timeout(task, session, time);
    return 0;
}
#if WITH_SSL
static int32_t _ltask_sslevnew(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
    const char *ca = luaL_checkstring(lua, 2);
    const char *cert = luaL_checkstring(lua, 3);
    const char *key = luaL_checkstring(lua, 4);
    int32_t keytype = (int32_t)luaL_checkinteger(lua, 5);
    int32_t verify = (int32_t)luaL_checkinteger(lua, 6);
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
    evssl_ctx *ssl = evssl_new(capath, certpath, keypath, keytype, verify);
    certs_register(srey, name, ssl);
    lua_pushlightuserdata(lua, ssl);
    return 1;
}
static int32_t _ltask_sslevp12new(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
    const char *p12 = luaL_checkstring(lua, 2);
    const char *pwd = luaL_checkstring(lua, 3);
    int32_t verify = (int32_t)luaL_checkinteger(lua, 4);
    char p12path[PATH_LENS] = { 0 };
    if (0 != strlen(p12)) {
        SNPRINTF(p12path, sizeof(p12path) - 1, "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, p12);
    }
    evssl_ctx *ssl = evssl_p12_new(p12path, pwd, verify);
    certs_register(srey, name, ssl);
    lua_pushlightuserdata(lua, ssl);
    return 1;
}
static int32_t _ltask_sslevqury(lua_State *lua) {
    const char *name = luaL_checkstring(lua, 1);
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
    int32_t ptype = (int32_t)luaL_checkinteger(lua, 2);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 3)) {
        evssl = lua_touserdata(lua, 3);
    }
    const char *host = luaL_checkstring(lua, 4);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 5);
    int32_t sendev = (int32_t)luaL_checkinteger(lua, 6);
    if (ERR_OK == task_netlisten(task, ptype, evssl, host, port, sendev)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
static int32_t _ltask_connect(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    int32_t ptype = (int32_t)luaL_checkinteger(lua, 2);
    uint64_t session = (uint64_t)luaL_checkinteger(lua, 3);
    struct evssl_ctx *evssl = lua_touserdata(lua, 4);
    const char *host = luaL_checkstring(lua, 5);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 6);
    int32_t sendev = (int32_t)luaL_checkinteger(lua, 7);
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
    int32_t ptype = (int32_t)luaL_checkinteger(lua, 2);
    const char *host = luaL_checkstring(lua, 3);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 4);
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
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        data = (void *)luaL_checklstring(lua, 3, &size);
    } else {
        data = lua_touserdata(lua, 3);
        size = (size_t)luaL_checkinteger(lua, 4);
    }
    pack_type ptype = (pack_type)luaL_checkinteger(lua, 5);
    task_netsend(task, fd, data, size, ptype);
    return 0;
}
static int32_t _ltask_sendto(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    const char *host = luaL_checkstring(lua, 3);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 4);
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 5)) {
        data = (void *)luaL_checklstring(lua, 5, &size);
    } else {
        data = lua_touserdata(lua, 5);
        size = (size_t)luaL_checkinteger(lua, 6);
    }
    ev_sendto(task_netev(task), fd, host, port, data, size);
    return 0;
}
static int32_t _ltask_close(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 2);
    ev_close(task_netev(task), fd);
    return 0;
}
static int32_t _ltask_ipport(lua_State *lua) {
    netaddr_ctx *addr = lua_touserdata(lua, 1);
    if (NULL == addr) {
        lua_pushnil(lua);
        LOG_WARN("%s", ERRSTR_INVPARAM);
        return 1;
    }
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
static int32_t _ltask_remoteaddr(lua_State *lua) {
    netaddr_ctx addr;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    if (-1 == fd) {
        lua_pushnil(lua);
        LOG_WARN("%s", ERRSTR_INVPARAM);
        return 1;
    }
    if (ERR_OK != netaddr_remoteaddr(&addr, fd, sock_family(fd))) {
        lua_pushnil(lua);
        return 1;
    }
    char ip[IP_LENS];
    if (ERR_OK != netaddr_ip(&addr, ip)) {
        lua_pushnil(lua);
        return 1;
    }
    uint16_t port = netaddr_port(&addr);
    lua_pushstring(lua, ip);
    lua_pushinteger(lua, port);
    return 2;
}
static int32_t _ltask_md5(lua_State *lua) {
    void *data;
    size_t size;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        data = (void *)luaL_checklstring(lua, 1, &size);
    } else {
        data = lua_touserdata(lua, 1);
        size = (size_t)luaL_checkinteger(lua, 2);
    }
    char strmd5[33];
    md5((const char*)data, size, strmd5);
    lua_pushstring(lua, strmd5);
    return 1;
}
static int32_t _ltask_udtostr(lua_State *lua) {
    void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    lua_pushlstring(lua, data, size);
    return 1;
}
static int32_t _ltask_http_method(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    size_t lens = 0;
    const char *value = http_method(pack, &lens);
    if (NULL == value) {
        lua_pushnil(lua);
    } else {
        lua_pushlstring(lua, value, lens);
    }
    return 1;
}
static int32_t _ltask_http_chunked(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    lua_pushinteger(lua, http_chunked(pack));
    return 1;
}
static int32_t _ltask_http_data(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    size_t lens = 0;
    char *value = http_data(pack, &lens);
    if (NULL == value) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlightuserdata(lua, value);
    lua_pushinteger(lua, lens);
    return 2;
}
static int32_t _ltask_http_copydata(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    size_t lens = 0;
    char *value = http_data(pack, &lens);
    if (NULL == value) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlstring(lua, value, lens);
    return 1;
}
static int32_t _ltask_http_header(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    const char *key = luaL_checkstring(lua, 2);
    size_t lens = 0;
    const char *value = http_header(pack, key, &lens);
    if (NULL == value) {
        lua_pushnil(lua);
    } else {
        lua_pushlstring(lua, value, lens);
    }
    return 1;
}
static int32_t _ltask_http_headers(lua_State *lua) {
    void *pack = lua_touserdata(lua, 1);
    size_t size = http_nheader(pack);
    http_header_ctx *header;
    lua_createtable(lua, 0, (int32_t)size);
    for (size_t i = 0; i < size; i++) {
        header = http_header_at(pack, i);
        lua_pushlstring(lua, header->key, header->klen);
        lua_pushlstring(lua, header->value, header->vlen);
        lua_settable(lua, -3);
    }
    return 1;
}
static int32_t _ltask_simple_data(lua_State *lua) {
    void *data = lua_touserdata(lua, 1);
    size_t size;
    void *pack = simple_data(data, &size);
    if (0 == size) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushlightuserdata(lua, pack);
    lua_pushinteger(lua, size);
    return 2;
}
LUAMOD_API int luaopen_srey(lua_State *lua) {
    luaL_Reg reg[] = {
        { "log", _ltask_log },
        { "setlog", _ltask_setlog },

        { "register", _ltask_register },
        { "qury", _ltask_qury },
        { "name", _ltask_name },
        { "session", _ltask_session },
#if WITH_SSL
        { "sslevnew", _ltask_sslevnew },
        { "sslevp12new", _ltask_sslevp12new },
        { "sslevqury", _ltask_sslevqury },
#endif
        { "user", _ltask_user },
        { "timeout", _ltask_timeout },
        { "listen", _ltask_listen },
        { "connect", _ltask_connect },
        { "udp", _ltask_udp },

        { "send", _ltask_send },
        { "sendto", _ltask_sendto },
        { "close", _ltask_close },

        { "ipport", _ltask_ipport },
        { "remoteaddr", _ltask_remoteaddr },
        { "md5", _ltask_md5 },
        { "utostr", _ltask_udtostr },

        { "http_method", _ltask_http_method },        
        { "http_chunked", _ltask_http_chunked },
        { "http_data", _ltask_http_data },
        { "http_copydata", _ltask_http_copydata },
        { "http_header", _ltask_http_header },
        { "http_headers", _ltask_http_headers },

        { "simple_data", _ltask_simple_data },

        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
