#include "lbind/ltask.h"

// 向 Lua table 中写入整数字段
#define LUA_TB_NUMBER(key, val)\
    lua_pushstring(lua, key);\
    lua_pushinteger(lua, val);\
    lua_settable(lua, -3)

// 向 Lua table 中写入字符串字段
#define LUA_TB_STRING(key, val)\
    lua_pushstring(lua, key);\
    lua_pushstring(lua, val);\
    lua_settable(lua, -3)

// 向 Lua table 中写入 userdata 指针和对应长度；val 为 NULL 时仅写入 size
#define LUA_TB_UD(val, size)\
    if (NULL != val){\
        lua_pushstring(lua, "data"); \
        lua_pushlightuserdata(lua, val); \
        lua_settable(lua, -3); \
    }\
    lua_pushstring(lua, "size"); \
    lua_pushinteger(lua, size);\
    lua_settable(lua, -3)

// 向 Lua table 中写入网络公共字段：pktype、fd、skid
#define LUA_TB_NETPUB(msg)\
    LUA_TB_NUMBER("pktype", msg->pktype);\
    LUA_TB_NUMBER("fd", msg->fd);\
    LUA_TB_NUMBER("skid", msg->skid);

// Lua task 上下文：保存 Lua 虚拟机、消息分发函数引用及计时器
typedef struct ltask_ctx {
    int32_t ref;        // message_dispatch 函数在 Lua 注册表中的引用 id
    lua_State *lua;     // 当前 task 独占的 Lua 虚拟机
    timer_ctx timer;    // 任务内部计时器，用于获取当前毫秒时间戳
}ltask_ctx;

// Lua 脚本根路径（含末尾分隔符），由 ltask_startup 初始化
static char luapath[PATH_LENS] = { 0 };

// 设置 Lua package 搜索路径（cpath 或 path），追加 luapath 下的对应扩展名目录
static void _ltask_setpath(lua_State *lua, const char *name, const char *exname) {
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, name);
    lua_pushfstring(lua, "%s;%s?.%s", lua_tostring(lua, -1), luapath, exname);
    lua_setfield(lua, -3, name);
    lua_pop(lua, 2);
}
// 创建并初始化一个新的 Lua 虚拟机，设置路径、全局变量及当前 task 指针
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
// 将脚本名称格式化为完整的 .lua 文件路径，存入 path
static inline void _ltask_fmtfile(const char *file, char *path) {
    ZERO(path, PATH_LENS);
    SNPRINTF(path, PATH_LENS, "%s%s.lua", luapath, file);
}
// 搜索脚本文件：先按原名查找，再将 '.' 替换为路径分隔符后重试
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
// 搜索并执行指定 Lua 脚本文件，执行失败时记录错误日志
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
// 对外接口：初始化 Lua 并执行 startup.lua，完成后关闭虚拟机
int32_t ltask_startup(const char *script) {
    LOG_INFO(LUA_RELEASE);
    SNPRINTF(luapath, sizeof(luapath), "%s%s%s%s",
             procpath(), PATH_SEPARATORSTR, script, PATH_SEPARATORSTR);
    lua_State *lua = _ltask_luainit(NULL);
    if (NULL == lua) {
        return ERR_FAILED;
    }
    int32_t rtn = _ltask_dofile(lua, "startup");
    lua_close(lua);
    return rtn;
}
// 初始化 ltask_ctx：创建 Lua 虚拟机、执行脚本、引用消息分发函数
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
// task 参数释放回调：关闭 Lua 虚拟机并释放 ltask_ctx 内存
static void _ltask_arg_free(void *arg) {
    ltask_ctx *ltask = arg;
    if (NULL != ltask->lua) {
        lua_close(ltask->lua);
    }
    FREE(ltask);
}
// Lua 回调：清理消息 table 中的 C 内存（作为 __gc 元方法使用）
static int32_t _msg_clean(lua_State *lua) {
    ASSERTAB(LUA_TTABLE == lua_type(lua, 1), "_msg_clean type error.");
    lua_pushnil(lua);
    void *data = NULL;
    char *key;
    uint8_t mtype = 0;
    uint8_t pktype = 0;
    int32_t n = 0;
    while (lua_next(lua, 1)) {
        if (LUA_TSTRING != lua_type(lua, -2)) {
            lua_pop(lua, 1);
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
            lua_pop(lua, 1);
            break;
        }
    }
    if (NULL != data) {
        _message_clean(mtype, pktype, data);
    }
    return 0;
}
// 将 C 层 message_ctx 打包为 Lua table，按 mtype 类型填充对应字段
static void _ltask_pack_msg(lua_State *lua, message_ctx *msg) {
    lua_createtable(lua, 0, 11);
    if (ERR_OK == _message_should_clean(msg)) {
        // 需要手动释放内存的消息挂 __gc 元方法
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
    case MSG_TYPE_SSLEXCHANGED:
        LUA_TB_NETPUB(msg);
        LUA_TB_NUMBER("client", msg->client);
        LUA_TB_NUMBER("sess", msg->sess);
        break;
    case MSG_TYPE_HANDSHAKED:
        LUA_TB_NETPUB(msg);
        LUA_TB_NUMBER("client", msg->client);
        LUA_TB_NUMBER("sess", msg->sess);
        LUA_TB_NUMBER("erro", msg->erro);
        LUA_TB_UD(msg->data, msg->size);
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
        // udata 指向紧跟在 netaddr_ctx 后面的 UDP 数据负载
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
// task 消息分发回调：从注册表取消息分发函数，打包消息后调用 Lua
static void _ltask_run(task_dispatch_arg *arg) {
    ltask_ctx *ltask = arg->task->arg;
    lua_rawgeti(ltask->lua, LUA_REGISTRYINDEX, ltask->ref);
    _ltask_pack_msg(ltask->lua, &arg->msg);
    if (LUA_OK != lua_pcall(ltask->lua, 1, 0, 0)) {
        LOG_ERROR("%s", lua_tostring(ltask->lua, -1));
        lua_pop(ltask->lua, 1);
    }
}
// Lua 绑定：注册一个新 task，返回 task 指针（lightuserdata）或 nil
static int32_t _ltask_register(lua_State *lua) {
    const char *file = luaL_checkstring(lua, 1);
    name_t name = (name_t)luaL_checkinteger(lua, 2);
    ltask_ctx *ltask;
    CALLOC(ltask, 1, sizeof(ltask_ctx));
    if (NULL == ltask) {
        lua_pushnil(lua);
        return 1;
    }
    timer_init(&ltask->timer);
    task_ctx *task = task_new(g_loader, name, _ltask_run, _ltask_arg_free, ltask);
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
// Lua 绑定：关闭指定 task；参数为 nil 时关闭当前 task
static int32_t _ltask_close(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        task = lua_touserdata(lua, 1);
    }
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_close(task);
    return 0;
}
// Lua 绑定：按 name 查找并持有 task（引用计数 +1），返回 task 指针或 nil
static int32_t _ltask_grab(lua_State *lua) {
    name_t name = (name_t)luaL_checkinteger(lua, 1);
    task_ctx *task = task_grab(g_loader, name);
    if (NULL == task) {
        lua_pushnil(lua);
    } else {
        lua_pushlightuserdata(lua, task);
    }
    return 1;
}
// Lua 绑定：增加 task 引用计数
static int32_t _ltask_incref(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    task_incref(task);
    return 0;
}
// Lua 绑定：释放 task_grab 持有的 task 引用（引用计数 -1）
static int32_t _ltask_ungrab(lua_State *lua) {
    task_ctx *task = lua_touserdata(lua, 1);
    task_ungrab(task);
    return 0;
}
// Lua 绑定：获取 task 的 name 标识；参数为 nil 时取当前 task
static int32_t _ltask_name(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        task = lua_touserdata(lua, 1);
    }
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task->name);
    return 1;
}
// Lua 绑定：返回当前 task 计时器已运行的毫秒数
static int32_t _ltask_timer_ms(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    ltask_ctx *ltask = task->arg;
    lua_pushinteger(lua, timer_cur_ms(&ltask->timer));
    return 1;
}
// Lua 绑定：设置当前 task 的请求超时时间（毫秒）
static int32_t _ltask_set_request_timeout(lua_State *lua) {
    uint32_t ms = (uint32_t)luaL_checkinteger(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_set_request_timeout(task, ms);
    return 0;
}
// Lua 绑定：获取当前 task 的请求超时时间（毫秒）
static int32_t _ltask_get_request_timeout(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task_get_request_timeout(task));
    return 1;
}
// Lua 绑定：设置当前 task 的连接超时时间（毫秒）
static int32_t _ltask_set_connect_timeout(lua_State *lua) {
    uint32_t ms = (uint32_t)luaL_checkinteger(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_set_connect_timeout(task, ms);
    return 0;
}
// Lua 绑定：获取当前 task 的连接超时时间（毫秒）
static int32_t _ltask_get_connect_timeout(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task_get_connect_timeout(task));
    return 1;
}
// Lua 绑定：设置当前 task 的网络读取超时时间（毫秒）
static int32_t _ltask_set_netread_timeout(lua_State *lua) {
    uint32_t ms = (uint32_t)luaL_checkinteger(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_set_netread_timeout(task, ms);
    return 0;
}
// Lua 绑定：获取当前 task 的网络读取超时时间（毫秒）
static int32_t _ltask_get_netread_timeout(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task_get_netread_timeout(task));
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

        { "set_request_timeout", _ltask_set_request_timeout },
        { "get_request_timeout", _ltask_get_request_timeout },
        { "set_connect_timeout", _ltask_set_connect_timeout },
        { "get_connect_timeout", _ltask_get_connect_timeout },
        { "set_netread_timeout", _ltask_set_netread_timeout },
        { "get_netread_timeout", _ltask_get_netread_timeout },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
