#include "ltask.h"

#if WITH_LUA
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
    if (NULL != val){\
        lua_pushstring(lua, "data"); \
        lua_pushlightuserdata(lua, val); \
        lua_settable(lua, -3); \
    }\
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
    timer_ctx timer;
}ltask_ctx;
static char luapath[PATH_LENS] = { 0 };

static void _ltask_setpath(lua_State *lua, const char *name, const char *exname) {
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, name);
    lua_pushfstring(lua, "%s;%s?.%s", lua_tostring(lua, -1), luapath, exname);
    lua_setfield(lua, -3, name);
    lua_pop(lua, 2);
}
static lua_State *_ltask_luainit(task_ctx *task) {
    lua_State *lua = luaL_newstate();
    if (NULL == lua) {
        LOG_ERROR("%s", "luaL_newstate failed.");
        return NULL;
    }
    luaL_openlibs(lua);
    _ltask_setpath(lua, "cpath", DLL_EXNAME);
    _ltask_setpath(lua, "path", "lua");
    lua_pushstring(lua, procpath());
    lua_setglobal(lua, PATH_NAME);
    lua_pushstring(lua, PATH_SEPARATORSTR);
    lua_setglobal(lua, PATH_SEP_NAME);
    if (NULL != task) {
        lua_pushlightuserdata(lua, task);
        lua_setglobal(lua, CUR_TASK_NAME);
    }
    return lua;
}
static inline void _ltask_fmtfile(const char *file, char *path) {
    ZERO(path, PATH_LENS);
    SNPRINTF(path, PATH_LENS - 1, "%s%s%s", luapath, file, ".lua");
}
static int32_t _ltask_searchfile(const char *file, char *path) {
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
static int32_t _ltask_dofile(lua_State *lua, const char *file) {
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
int32_t ltask_startup(void) {
    LOG_INFO(LUA_RELEASE);
    SNPRINTF(luapath, sizeof(luapath) - 1, "%s%s%s%s",
             procpath(), PATH_SEPARATORSTR, SCRIPT_FOLDER, PATH_SEPARATORSTR);
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
    lua_getglobal(lua, MSG_DISP_FUNC);
    if (LUA_TFUNCTION != lua_type(lua, 1)) {
        lua_close(lua);
        LOG_ERROR("not find function %s.", MSG_DISP_FUNC);
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
static int32_t _msg_clean(lua_State *lua) {
    ASSERTAB(LUA_TTABLE == lua_type(lua, 1), "_msg_clean type error.");
    lua_gettable(lua, 1);
    lua_pushnil(lua);
    void *data = NULL;
    char *key;
    uint8_t mtype = 0;
    uint8_t pktype = 0;
    int32_t n = 0;
    while (lua_next(lua, -2)) {
        if (LUA_TSTRING != lua_type(lua, -2)) {
            continue;
        }
        key = (char *)lua_tostring(lua, -2);
        if (0 == strcmp(key, "mtype")) {
            mtype = (uint8_t)lua_tointeger(lua, -1);
            n++;
        } else if (0 == strcmp(key, "pktype")) {
            pktype = (uint8_t)lua_tointeger(lua, -1);
            n++;
        } else if (0 == strcmp(key, "data")) {
            data = lua_touserdata(lua, -1);
            n++;
        }
        lua_pop(lua, 1);
        if (n >= 3) {
            break;
        }
    }
    if (NULL != data) {
        _message_clean(mtype, pktype, data);
    }
    return 0;
}
static void _ltask_pack_msg(lua_State *lua, message_ctx *msg) {
    lua_createtable(lua, 0, 11);
    if (ERR_OK == _message_should_clean(msg)) {
        lua_newtable(lua);
        lua_pushcfunction(lua, _msg_clean);
        lua_setfield(lua, -2, "__gc");
        lua_setmetatable(lua, -2);
    }
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
        LUA_TB_NUMBER("size", msg->size);
        break;
    case MSG_TYPE_CLOSE:
        LUA_TB_NETPUB(msg);
        LUA_TB_NUMBER("client", msg->client);
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
        LUA_TB_NUMBER("pktype", msg->pktype);
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
static void _ltask_run(task_dispatch_arg *arg) {
    ltask_ctx *ltask = arg->task->arg;
    lua_rawgeti(ltask->lua, LUA_REGISTRYINDEX, ltask->ref);
    _ltask_pack_msg(ltask->lua, &arg->msg);
    if (LUA_OK != lua_pcall(ltask->lua, 1, 0, 0)) {
        LOG_ERROR("%s", lua_tostring(ltask->lua, 1));
    }
}
static int32_t _ltask_register(lua_State *lua) {
    const char *file = luaL_checkstring(lua, 1);
    name_t name = (name_t)luaL_checkinteger(lua, 2);
    ltask_ctx *ltask;
    CALLOC(ltask, 1, sizeof(ltask_ctx));
    timer_init(&ltask->timer);
    task_ctx *task = task_new(g_scheduler, name, _ltask_run, _ltask_arg_free, ltask);
    if (NULL == task) {
        FREE(ltask);
        lua_pushnil(lua);
        return 1;
    }
    if (ERR_OK != _ltask_init(task, ltask, file)) {
        task_free(task);
        lua_pushnil(lua);
        return 1;
    }
    if (ERR_OK == task_register(task, NULL, NULL)) {
        lua_pushlightuserdata(lua, task);
    } else {
        task_free(task);
        lua_pushnil(lua);
    }
    return 1;
}
static int32_t _ltask_close(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        task = lua_touserdata(lua, 1);
    }
    task_close(task);
    return 0;
}
static int32_t _ltask_grab(lua_State *lua) {
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    task_ctx *task = task_grab(g_scheduler, name);
    if (NULL == task) {
        lua_pushnil(lua);
    } else {
        lua_pushlightuserdata(lua, task);
    }
    return 1;
}
static int32_t _ltask_incref(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    task_incref(task);
    return 0;
}
static int32_t _ltask_ungrab(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    task_ungrab(task);
    return 0;
}
static int32_t _ltask_name(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        task = lua_touserdata(lua, 1);
    }
    lua_pushinteger(lua, task->name);
    return 1;
}
static int32_t _ltask_timer_ms(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    ltask_ctx *ltask = task->arg;
    lua_pushinteger(lua, timer_cur_ms(&ltask->timer));
    return 1;
}
//srey.task
LUAMOD_API int luaopen_task(lua_State *lua) {
    luaL_Reg reg[] = {
        { "register", _ltask_register },
        { "close", _ltask_close },
        { "grab", _ltask_grab },
        { "incref", _ltask_incref },
        { "ungrab", _ltask_ungrab },
        { "name", _ltask_name },
        { "timer_ms", _ltask_timer_ms },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}

#endif
