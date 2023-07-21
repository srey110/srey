#include "ltask.h"
#if WITH_LUA
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#ifdef OS_WIN
#pragma comment(lib, "lualib.lib")
#endif

#define LUA_TB_NUMBER(key, val)\
lua_pushstring(lua, key);\
lua_pushinteger(lua, val);\
lua_settable(lua, -3)
#define LUA_TB_STRING(key, val)\
lua_pushstring(lua, key);\
lua_pushstring(lua, val);\
lua_settable(lua, -3)
#define LUA_TB_UD(val, size)\
lua_pushstring(lua, "data"); \
lua_pushlightuserdata(lua, val); \
lua_settable(lua, -3); \
lua_pushstring(lua, "size"); \
lua_pushinteger(lua, size);\
lua_settable(lua, -3)
#define LUA_TB_NETPUB(msg)\
LUA_TB_NUMBER("pktype", msg->pktype);\
LUA_TB_NUMBER("fd", msg->fd);\
LUA_TB_NUMBER("skid", msg->skid);

typedef struct ltask_ctx {
    int32_t ref;
    lua_State *lua;
}ltask_ctx;
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
    lua_pushstring(lua, procpath());
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
    SNPRINTF(path, PATH_LENS - 1, "%s%s%s", luapath, file, ".lua");
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
    LOG_INFO(LUA_RELEASE);
    SNPRINTF(luapath, sizeof(luapath) - 1, "%s%s%s%s",
             procpath(), PATH_SEPARATORSTR, "lua", PATH_SEPARATORSTR);
    lua_State *lua = _ltask_luainit(NULL);
    if (NULL == lua) {
        return ERR_FAILED;
    }
    int32_t rtn = _ltask_dofile(lua, "startup");
    lua_close(lua);
    return rtn;
}
static int32_t _ltask_init(task_ctx *task, ltask_ctx *ltask, const char *file) {
    lua_State *lua = _ltask_luainit(task);
    if (NULL == lua) {
        return ERR_FAILED;
    }
    if (ERR_OK != _ltask_dofile(lua, file)) {
        lua_close(lua);
        return ERR_FAILED;
    }
    lua_getglobal(lua, "dispatch_message");
    if (LUA_TFUNCTION != lua_type(lua, 1)) {
        lua_close(lua);
        LOG_ERROR("not have function dispatch_message.");
        return ERR_FAILED;
    }
    ltask->lua = lua;
    ltask->ref = luaL_ref(ltask->lua, LUA_REGISTRYINDEX);
    return ERR_OK;
}
static void _ltask_arg_free(void *arg) {
    ltask_ctx *ltask = arg;
    if (NULL != ltask->lua) {
        lua_close(ltask->lua);
    }
    FREE(ltask);
}
static inline void _ltask_pack_msg(lua_State *lua, message_ctx *msg) {
    lua_createtable(lua, 0, 11);
    LUA_TB_NUMBER("mtype", msg->mtype);
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP:
        break;
    case MSG_TYPE_CLOSING:
        break;
    case MSG_TYPE_TIMEOUT:
        LUA_TB_NUMBER("sess", msg->sess);
        break;
    case MSG_TYPE_ACCEPT:
        LUA_TB_NETPUB(msg);
        break;
    case MSG_TYPE_RECV:
        LUA_TB_NETPUB(msg);
        LUA_TB_NUMBER("client", msg->client); 
        LUA_TB_NUMBER("sess", msg->sess);
        LUA_TB_NUMBER("slice", msg->slice);
        LUA_TB_UD(msg->data, msg->size);
        break;
    case MSG_TYPE_SEND:
        LUA_TB_NETPUB(msg);
        LUA_TB_NUMBER("client", msg->client);
        LUA_TB_NUMBER("sess", msg->sess);
        LUA_TB_NUMBER("size", msg->size);
        break;
    case MSG_TYPE_CLOSE:
        LUA_TB_NETPUB(msg);
        LUA_TB_NUMBER("sess", msg->sess);
        break;
    case MSG_TYPE_CONNECT:
        LUA_TB_NETPUB(msg);
        LUA_TB_NUMBER("sess", msg->sess);
        LUA_TB_NUMBER("erro", msg->erro);
        break;
    case MSG_TYPE_HANDSHAKED:
        LUA_TB_NETPUB(msg);
        LUA_TB_NUMBER("client", msg->client);
        LUA_TB_NUMBER("sess", msg->sess);
        LUA_TB_NUMBER("erro", msg->erro);
        break;
    case MSG_TYPE_RECVFROM: {
        LUA_TB_NUMBER("fd", msg->fd);
        LUA_TB_NUMBER("skid", msg->skid);
        LUA_TB_NUMBER("sess", msg->sess);
        char ip[IP_LENS];
        netaddr_ctx *addr = msg->data;
        netaddr_ip(addr, ip);
        LUA_TB_STRING("ip", ip);
        LUA_TB_NUMBER("port", netaddr_port(addr));
        lua_pushstring(lua, "udata");
        lua_pushlightuserdata(lua, ((char *)msg->data) + sizeof(netaddr_ctx));
        lua_settable(lua, -3);
        LUA_TB_UD(msg->data, msg->size);
        break;
    }
    case MSG_TYPE_REQUEST:
        LUA_TB_NUMBER("sess", msg->sess);
        LUA_TB_NUMBER("src", msg->src);
        LUA_TB_UD(msg->data, msg->size);
        break;
    case MSG_TYPE_RESPONSE:
        LUA_TB_NUMBER("sess", msg->sess);
        LUA_TB_NUMBER("erro", msg->erro);
        LUA_TB_UD(msg->data, msg->size);
        break;
    default:
        break;
    }
}
static inline void _ltask_run(task_ctx *task, message_ctx *msg) {
    ltask_ctx *ltask = task->arg;
    lua_rawgeti(ltask->lua, LUA_REGISTRYINDEX, ltask->ref);
    _ltask_pack_msg(ltask->lua, msg);
    if (LUA_OK != lua_pcall(ltask->lua, 1, 0, 0)) {
        LOG_ERROR("%s", lua_tostring(ltask->lua, 1));
    }
}
static int32_t _ltask_register(lua_State *lua) {
    const char *file = luaL_checkstring(lua, 1);
    name_t name = (name_t)luaL_checkinteger(lua, 2);
    uint16_t maxcnt = (uint16_t)luaL_checkinteger(lua, 3);
    uint16_t maxmsgqulens = (uint16_t)luaL_checkinteger(lua, 4);
    ltask_ctx *ltask;
    CALLOC(ltask, 1, sizeof(ltask_ctx));
    task_ctx *task = srey_task_new(TTYPE_LUA, name, maxcnt, maxmsgqulens, _ltask_arg_free, ltask);
    if (NULL == task) {
        FREE(ltask);
        lua_pushboolean(lua, 0);
        return 1;
    }
    if (ERR_OK != _ltask_init(task, ltask, file)) {
        srey_task_free(task);
        lua_pushboolean(lua, 0);
        return 1;
    }
    srey_task_regcb(task, MSG_TYPE_ALL, _ltask_run);
    if (ERR_OK == srey_task_register(srey, task)) {
        lua_pushboolean(lua, 1);
    } else {
        srey_task_free(task);
        lua_pushboolean(lua, 0);
    }
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
