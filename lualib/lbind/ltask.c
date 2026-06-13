#include "lbind/ltask.h"
#if ENABLE_LUA_BYTECACHE
#include "lbind/lbytecache.h"
#endif

// 向 Lua table 中写入整数字段（setfield 比 push+settable 省一次 lock 配对）
#define LUA_TB_NUMBER(key, val)\
    lua_pushinteger(lua, val);\
    lua_setfield(lua, -2, key)

// 向 Lua table 中写入字符串字段
#define LUA_TB_STRING(key, val)\
    lua_pushstring(lua, val);\
    lua_setfield(lua, -2, key)

// 向 Lua table 中写入 userdata 指针和对应长度；val 为 NULL 时仅写入 size
#define LUA_TB_UD(val, size)\
    if (NULL != val){\
        lua_pushlightuserdata(lua, val); \
        lua_setfield(lua, -2, "data"); \
    }\
    lua_pushinteger(lua, size);\
    lua_setfield(lua, -2, "size")

// 向 Lua table 中写入网络公共字段：subtype、fd、skid
#define LUA_TB_NETPUB(msg)\
    LUA_TB_NUMBER("subtype", msg->subtype);\
    LUA_TB_NUMBER("fd", msg->fd);\
    LUA_TB_NUMBER("skid", msg->skid)

// Lua task 上下文：保存 Lua 虚拟机、消息分发函数引用、计时器、内存统计及中断信号
typedef struct ltask_ctx {
    int32_t    ref;       // message_dispatch 函数在 Lua 注册表中的引用 id
    atomic_t   trap;      // 中断信号：0=正常 1=待 hook 触发；其他线程置位后 hook 抛错
    size_t     mem;       // 当前 Lua 累计内存（字节，单 worker 串行操作，无需 atomic）
    task_ctx  *task;      // 回指 task_ctx，供 allocator/trap 日志取 name
    lua_State *lua;       // 当前 task 独占的 Lua 虚拟机（主 thread）
    lua_State *active_lua;// 当前活跃的 thread（main 或某 coroutine），由 srey.lua _coro_resume 通过 task.active 维护
    timer_ctx  timer;     // 任务内部计时器，用于获取当前毫秒时间戳
    tda_ctx    mem_tda;   // 内存翻倍告警：默认 0 禁用，task.memlimit(N) 设阈值后 mem 超阈值仅 LOG_WARN（不拒绝分配）
}ltask_ctx;

// Lua 脚本根路径（含末尾分隔符），由 ltask_startup 初始化
static char luapath[PATH_LENS] = { 0 };

// 自定义 Lua 分配器：累计内存到 ltask_ctx，mem 越过 tda 告警阈值时 LOG_WARN 并翻倍阈值。
// 同一 task 的 lua_State 操作天然串行（loader 一次只允许一个 worker 处理同一 task），
// mem 字段无需 atomic。
static void *_ltask_lalloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    ltask_ctx *l = (ltask_ctx *)ud;
    if (0 == nsize) {
        if (NULL != ptr) {
            l->mem -= osize;
        }
        _free(ptr);
        return NULL;
    }
    size_t after = l->mem + nsize - (NULL != ptr ? osize : 0);
    void *np = _realloc(ptr, nsize);
    if (NULL == np) {
        return NULL;
    }
    l->mem = after;
    if (tda_check(&l->mem_tda, l->mem)) {
        LOG_WARN("task %s lua mem grow to %.2f MB.",
                 _NAME_OR(l->task->name), (double)l->mem / (1024.0 * 1024.0));
    }
    return np;
}
// Lua 字节码 hook：trap 触发后由 lua_sethook(active_lua, ..., LUA_MASKCOUNT, 1)
// 安排在下一条字节码执行时回调；清 trap 标志并抛 LUA_ERRRUN 终止当前协程。
// 通过 lua_getallocf 取出 ltask_ctx：自定义 allocator 的 ud 就是 ltask_ctx，无需额外查找。
static void _ltask_signal_hook(lua_State *lua, lua_Debug *ar) {
    (void)ar;
    void *ud = NULL;
    lua_getallocf(lua, &ud);
    ltask_ctx *l = (ltask_ctx *)ud;
    // 清除 hook 防止重入（hook 内调 luaL_error 会再次进入 vm）
    lua_sethook(lua, NULL, 0, 0);
    ATOMIC_SET(&l->trap, 0);
    luaL_error(lua, "task %s interrupted", _NAME_OR(l->task->name));
}
// 设置 Lua package 搜索路径（cpath 或 path），追加 luapath 下的对应扩展名目录
static void _ltask_setpath(lua_State *lua, const char *name, const char *exname) {
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, name);
    lua_pushfstring(lua, "%s;%s?.%s", lua_tostring(lua, -1), luapath, exname);
    lua_setfield(lua, -3, name);
    lua_pop(lua, 2);
}
// 创建并初始化一个新的 Lua 虚拟机，设置路径、全局变量及当前 task 指针。
// alloc_ud != NULL 时改用 lua_newstate 注入 _ltask_lalloc 以启用 per-task 内存监控；
// alloc_ud == NULL 走 luaL_newstate（默认 allocator），用于 startup.lua 这类只跑一次即关的临时 state。
static lua_State *_ltask_luainit(task_ctx *task, ltask_ctx *alloc_ud) {
    lua_State *lua;
    if (NULL == alloc_ud) {
        lua = luaL_newstate();
    } else {
        lua = lua_newstate(_ltask_lalloc, alloc_ud, luaL_makeseed(NULL));
    }
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
#if ENABLE_LUA_BYTECACHE
        lbc_install_searcher(lua);
#endif
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
    char tmp[PATH_LENS];
    size_t lens = strlen(file);
    if (lens >= sizeof(tmp)) {
        return ERR_FAILED;
    }
    memcpy(tmp, file, lens + 1);
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
// 将 from 栈 idx 位置的基本类型值（nil/bool/number/string）复制到 to 栈顶
static void _ltask_copy_arg(lua_State *from, int32_t idx, lua_State *to) {
    switch (lua_type(from, idx)) {
    case LUA_TNIL:
        lua_pushnil(to);
        break;
    case LUA_TBOOLEAN:
        lua_pushboolean(to, lua_toboolean(from, idx));
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(from, idx)) {
            lua_pushinteger(to, lua_tointeger(from, idx));
        } else {
            lua_pushnumber(to, lua_tonumber(from, idx));
        }
        break;
    case LUA_TSTRING: {
        size_t lens;
        const char *s = lua_tolstring(from, idx, &lens);
        lua_pushlstring(to, s, lens);
        break;
    }
    default:
        // 不支持的类型（table/function/userdata 等）退化为 nil
        lua_pushnil(to);
        break;
    }
}
// 搜索 → 加载脚本 → 从 from 栈复制 [arg_start, arg_top] 区间作为 chunk 参数运行。
// from 为 NULL 时不传参；脚本顶层用 `local a, b, ... = ...` 接收。
static int32_t _ltask_dofile_args(lua_State *lua, const char *file,
                                  lua_State *from, int32_t arg_start, int32_t arg_top) {
    char path[PATH_LENS];
    if (ERR_OK != _ltask_searchfile(file, path)) {
        LOG_ERROR("cannot find %s:, no such file.", file);
        return ERR_FAILED;
    }
    int32_t rtn;
#if ENABLE_LUA_BYTECACHE
    rtn = lbc_loadfile(lua, path);
#else
    rtn = luaL_loadfile(lua, path);
#endif
    if (LUA_OK != rtn) {
        LOG_ERROR("%s", lua_tostring(lua, -1));
        return ERR_FAILED;
    }
    int32_t nargs = 0;
    if (NULL != from && arg_top >= arg_start) {
        for (int32_t i = arg_start; i <= arg_top; i++) {
            _ltask_copy_arg(from, i, lua);
            nargs++;
        }
    }
    if (LUA_OK != lua_pcall(lua, nargs, 0, 0)) {
        LOG_ERROR("%s", lua_tostring(lua, -1));
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 对外接口：初始化 Lua 并执行 startup.lua，完成后关闭虚拟机
int32_t ltask_startup(const char *script) {
#if ENABLE_LUA_BYTECACHE
    LOG_INFO("%s, bytecache true", LUA_RELEASE);
#else
    LOG_INFO("%s, bytecache false", LUA_RELEASE);
#endif
    SNPRINTF(luapath, sizeof(luapath), "%s%s%s%s",
             procpath(), PATH_SEPARATORSTR, script, PATH_SEPARATORSTR);
    lua_State *lua = _ltask_luainit(NULL, NULL);
    if (NULL == lua) {
        return ERR_FAILED;
    }
    int32_t rtn = _ltask_dofile(lua, "startup");
    lua_close(lua);
    return rtn;
}
// 初始化 ltask_ctx：创建 Lua 虚拟机、执行脚本、引用消息分发函数。
// 若 from != NULL，将 from 栈 [arg_start..arg_top] 区间的基本类型值作为变参
// 传给脚本 chunk，脚本顶层用 `local a, b, ... = ...` 接收。
static int32_t _ltask_init(task_ctx *task, ltask_ctx *ltask, const char *file,
                           lua_State *from, int32_t arg_start, int32_t arg_top) {
    // mem 与 tda 告警阈值在 lua_newstate（会触发首批 alloc）之前初始化；默认 0 禁用，由 task.memlimit 启用
    ltask->mem = 0;
    tda_init(&ltask->mem_tda, 0);
    ltask->task = task;
    ATOMIC_SET(&ltask->trap, 0);
    lua_State *lua = _ltask_luainit(task, ltask);
    if (NULL == lua) {
        return ERR_FAILED;
    }
    // active_lua 默认指向主 thread；srey.lua _coro_resume 进入协程时切到 coro，退出时切回主 thread
    ltask->active_lua = lua;
    if (ERR_OK != _ltask_dofile_args(lua, file, from, arg_start, arg_top)) {
        lua_close(lua);
        ltask->active_lua = NULL;
        return ERR_FAILED;
    }
    lua_getglobal(lua, MSG_DISP_FUNC);
    if (LUA_TFUNCTION != lua_type(lua, 1)) {
        lua_close(lua);
        ltask->active_lua = NULL;
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
    message_ctx tmp = { 0 };
    lua_getfield(lua, 1, "mtype");
    tmp.mtype = (msg_type)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "subtype");
    tmp.subtype = (subtype_t)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "data");
    tmp.data = lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "shared");
    tmp.shared = (shared_data *)lua_touserdata(lua, -1);
    lua_pop(lua, 1);
    // shared 非 NULL 走广播 ref-- 分支；shared 为 NULL 时仅 data 非 NULL 才需清理
    if (NULL != tmp.shared || NULL != tmp.data) {
        _message_clean(&tmp);
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
        lua_pushlightuserdata(lua, ((char *)msg->data) + sizeof(netaddr_ctx));
        lua_setfield(lua, -2, "udata");
        LUA_TB_UD(msg->data, msg->size);
        break;
    }
    case MSG_TYPE_REQUEST:
        LUA_TB_NUMBER("subtype", msg->subtype);
        LUA_TB_NUMBER("sess", msg->sess);
        LUA_TB_NUMBER("src", msg->src);
        LUA_TB_UD(msg->data, msg->size);
        // task_multi_call / task_multi_request 广播路径：shared 透传到 Lua 表,__gc 时走 ref-- 分支
        if (NULL != msg->shared) {
            lua_pushlightuserdata(lua, msg->shared);
            lua_setfield(lua, -2, "shared");
        }
        break;
    case MSG_TYPE_RESPONSE:
        LUA_TB_NUMBER("subtype", msg->subtype);
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
/// <summary>
/// 注册一个新 task。第 4 起的变参会作为参数传给脚本 chunk，
/// 脚本顶层用 `local a, b, ... = ...` 接收。变参仅支持 nil/bool/number/string 基本类型，
/// 其他类型（table/function/userdata 等）会退化为 nil
/// </summary>
/// <param name="file" type="string">脚本文件名（不含 .lua 后缀，支持 a.b 形式映射到目录）</param>
/// <param name="name" type="string?">字符串 task 名；nil 或空串=匿名（仅有句柄）</param>
/// <param name="quecap" type="integer">消息队列容量；0 用默认 ONEK</param>
/// <param name="..." type="any">传给脚本的可变参数（nil/bool/number/string）</param>
/// <returns type="lightuserdata?">task 指针；失败返回 nil</returns>
static int32_t _ltask_register(lua_State *lua) {
    const char *file = luaL_checkstring(lua, 1);
    const char *name = luaL_optstring(lua, 2, NULL);
    size_t quecap = (size_t)luaL_checkinteger(lua, 3);
    int32_t arg_top = lua_gettop(lua);
    ltask_ctx *ltask;
    CALLOC(ltask, 1, sizeof(ltask_ctx));
    timer_init(&ltask->timer);
    task_ctx *task = task_new(g_loader, name, quecap, _ltask_run, _ltask_arg_free, ltask);
    task->type = TASK_LUA;
    if (ERR_OK != _ltask_init(task, ltask, file, lua, 4, arg_top)) {
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
/// <summary>
/// 关闭指定 task
/// </summary>
/// <param name="task" type="lightuserdata?">目标 task 指针；nil 时关闭当前 task</param>
/// <returns>无</returns>
static int32_t _ltask_close(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        LUACHECK_LUDATA(lua, 1);
        task = lua_touserdata(lua, 1);
    }
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_close(task);
    return 0;
}
/// <summary>
/// 按 name 查找并持有 task（引用计数 +1）
/// </summary>
/// <param name="name" type="string|integer">字符串名或数字句柄</param>
/// <returns type="lightuserdata?">task 指针；未找到返回 nil</returns>
static int32_t _ltask_grab(lua_State *lua) {
    name_t handle = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    task_ctx *task = task_grab(g_loader, handle);
    if (NULL == task) {
        lua_pushnil(lua);
    } else {
        lua_pushlightuserdata(lua, task);
    }
    return 1;
}
/// <summary>
/// 增加 task 引用计数
/// </summary>
/// <param name="task" type="lightuserdata">task 指针</param>
/// <returns>无</returns>
static int32_t _ltask_incref(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    task_ctx *task = lua_touserdata(lua, 1);
    task_incref(task);
    return 0;
}
/// <summary>
/// 释放 task_grab 持有的 task 引用（引用计数 -1）
/// </summary>
/// <param name="task" type="lightuserdata">task 指针</param>
/// <returns>无</returns>
static int32_t _ltask_ungrab(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    task_ctx *task = lua_touserdata(lua, 1);
    task_ungrab(task);
    return 0;
}
/// <summary>
/// 查询 task 是否正在关闭
/// </summary>
/// <param name="task" type="lightuserdata?">task 指针；nil 时查询当前 task</param>
/// <returns type="boolean">关闭中返回 true</returns>
static int32_t _ltask_isclosing(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        LUACHECK_LUDATA(lua, 1);
        task = lua_touserdata(lua, 1);
    }
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushboolean(lua, task_isclosing(task));
    return 1;
}
/// <summary>
/// 获取 task 类型
/// </summary>
/// <param name="task" type="lightuserdata?">task 指针；nil 时查询当前 task</param>
/// <returns type="TASK_TYPE">任务类型</returns>
static int32_t _ltask_get_type(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        LUACHECK_LUDATA(lua, 1);
        task = lua_touserdata(lua, 1);
    }
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task_get_type(task));
    return 1;
}
/// <summary>
/// 获取 task 的字符串名
/// </summary>
/// <param name="task" type="lightuserdata?">task 指针；nil 时取当前 task</param>
/// <returns type="string?">字符串 task 名；匿名 task 或不存在返回 nil</returns>
static int32_t _ltask_name(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        LUACHECK_LUDATA(lua, 1);
        task = lua_touserdata(lua, 1);
    }
    if (NULL == task
        || NULL == task->name) {
        lua_pushnil(lua);
    } else {
        lua_pushstring(lua, task->name);
    }
    return 1;
}
/// <summary>
/// 获取 task 的数字句柄（createid 生成）
/// </summary>
/// <param name="task" type="lightuserdata?">task 指针；nil 时取当前 task</param>
/// <returns type="integer">task 句柄；不存在返回 INVALID_TNAME</returns>
static int32_t _ltask_handle(lua_State *lua) {
    task_ctx *task;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        task = global_userdata(lua, CUR_TASK_NAME);
    } else {
        LUACHECK_LUDATA(lua, 1);
        task = lua_touserdata(lua, 1);
    }
    lua_pushinteger(lua, (NULL == task) ? INVALID_TNAME : task->handle);
    return 1;
}
/// <summary>
/// 返回当前 task 计时器已运行的毫秒数
/// </summary>
/// <param>无</param>
/// <returns type="integer">已运行毫秒数</returns>
static int32_t _ltask_timer_ms(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    ltask_ctx *ltask = task->arg;
    lua_pushinteger(lua, timer_cur_ms(&ltask->timer));
    return 1;
}
/// <summary>
/// 返回当前 task 按消息类型分桶的累计统计：每类消息条数与 dispatch 占用线程 CPU 纳秒。
/// total 字段为各桶之和，by_type 字段仅包含至少处理过 1 条消息的 mtype，键为 mtype 整数。
/// </summary>
/// <param>无</param>
/// <returns type="TaskStat">分桶累计统计</returns>
static int32_t _ltask_stat(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    uint64_t nmsg[MSG_TYPE_ALL];
    uint64_t dispatch_cpu_ns[MSG_TYPE_ALL];
    task_stat(task, nmsg, dispatch_cpu_ns);
    uint64_t total_nmsg = 0, total_ns = 0;
    for (int32_t i = 1; i < MSG_TYPE_ALL; i++) {
        total_nmsg += nmsg[i];
        total_ns += dispatch_cpu_ns[i];
    }
    lua_createtable(lua, 0, 2);
    lua_createtable(lua, 0, 2);
    LUA_TB_NUMBER("nmsg", (lua_Integer)total_nmsg);
    LUA_TB_NUMBER("dispatch_cpu_ns", (lua_Integer)total_ns);
    lua_setfield(lua, -2, "total");
    lua_createtable(lua, 0, MSG_TYPE_ALL - 1);
    for (int32_t i = 1; i < MSG_TYPE_ALL; i++) {
        if (0 == nmsg[i]) {
            continue;
        }
        lua_createtable(lua, 0, 2);
        LUA_TB_NUMBER("nmsg", (lua_Integer)nmsg[i]);
        LUA_TB_NUMBER("dispatch_cpu_ns", (lua_Integer)dispatch_cpu_ns[i]);
        lua_rawseti(lua, -2, i);
    }
    lua_setfield(lua, -2, "by_type");
    return 1;
}
/// <summary>
/// 获取当前 task lua_State 累计内存使用量（字节）。
/// 仅对 task.register 创建的 Lua task 有效；底层走自定义 allocator 累计 alloc/free 差值。
/// </summary>
/// <returns type="integer">累计字节数</returns>
static int32_t _ltask_mem(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    ltask_ctx *ltask = task->arg;
    lua_pushinteger(lua, (lua_Integer)ltask->mem);
    return 1;
}
/// <summary>
/// 设置当前 task lua_State 的内存告警阈值。mem 超过该阈值时翻倍告警，
/// 仅记录日志、不拒绝分配（不触发 LUA_ERRMEM）；默认 0 表示禁用。
/// </summary>
/// <param name="limit" type="integer">告警阈值字节数；0 表示禁用告警</param>
/// <returns>无</returns>
static int32_t _ltask_memlimit(lua_State *lua) {
    lua_Integer limit = luaL_checkinteger(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    ltask_ctx *ltask = task->arg;
    tda_init(&ltask->mem_tda, (size_t)(limit < 0 ? 0 : limit));
    return 0;
}
/// <summary>
/// 由 srey.lua _coro_resume 在 resume 前后调用，更新 ltask_ctx.active_lua 指向当前活跃 thread。
/// trap 触发方依据此字段定位字节码执行位置以 sethook；切换 active 后若发现 trap 已置位，
/// 立即给新 active 重新安装 hook，消除 sethook 安装到已死/已切走的 thread 时 trap 残留无法触发的 race 窗口。
/// </summary>
/// <param name="coro" type="thread?">当前活跃协程；nil/不传时还原为主 thread</param>
/// <returns>无</returns>
static int32_t _ltask_active(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    ltask_ctx *ltask = task->arg;
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type || LUA_TNONE == type) {
        ltask->active_lua = ltask->lua;
    } else if (LUA_TTHREAD == type) {
        ltask->active_lua = lua_tothread(lua, 1);
    } else {
        return luaL_argerror(lua, 1, "thread or nil expected");
    }
    if (ATOMIC_GET(&ltask->trap)) {
        lua_sethook(ltask->active_lua, _ltask_signal_hook, LUA_MASKCOUNT, 1);
    }
    return 0;
}
/// <summary>
/// 跨 task 中断指定 Lua task 的当前活跃协程：原子置 trap=1 并向 active_lua 安装 hook，
/// 目标协程下一条字节码即抛错退出。仅对 Lua task（task.register 创建）有效；目标不存在
/// 或非 Lua task 返回 false。
/// </summary>
/// <param name="name" type="string|integer">目标字符串名或数字句柄</param>
/// <returns type="boolean">成功 true；目标无效返回 false</returns>
static int32_t _ltask_trap(lua_State *lua) {
    name_t handle = (LUA_TSTRING == lua_type(lua, 1))
        ? task_find_name(g_loader, lua_tostring(lua, 1))
        : (name_t)luaL_checkinteger(lua, 1);
    task_ctx *target = task_grab(g_loader, handle);
    if (NULL == target) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    // 用 _arg_free 函数指针确认目标是 Lua task；C task 的 arg 类型不固定，错访会崩
    if (target->_arg_free != _ltask_arg_free) {
        task_ungrab(target);
        lua_pushboolean(lua, 0);
        return 1;
    }
    ltask_ctx *ltask = target->arg;
    // 0→1 CAS 保证多线程并发只生效一次；已有未触发的 trap 不重复 sethook。
    // active_lua 在 _ltask_init 设为主 thread 后只在 _ltask_active 切换，两条路径都赋非 NULL；
    // task_grab 的 rwlock acquire 屏障保证看到的是已初始化值，无需 NULL 检查。
    // 跨线程写 hook 字段非严格 thread-safe（Lua 内部仅写几个 byte/int）；
    // worker 若同步切换协程会在 _coro_resume 重新 sethook。
    if (ATOMIC_CAS(&ltask->trap, 0, 1)) {
        lua_sethook(ltask->active_lua, _ltask_signal_hook, LUA_MASKCOUNT, 1);
    }
    task_ungrab(target);
    lua_pushboolean(lua, 1);
    return 1;
}
/// <summary>
/// 设置当前 task 的请求超时时间
/// </summary>
/// <param name="ms" type="integer">超时毫秒数</param>
/// <returns>无</returns>
static int32_t _ltask_set_request_timeout(lua_State *lua) {
    uint32_t ms = (uint32_t)luaL_checkinteger(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_set_request_timeout(task, ms);
    return 0;
}
/// <summary>
/// 获取当前 task 的请求超时时间
/// </summary>
/// <param>无</param>
/// <returns type="integer">超时毫秒数</returns>
static int32_t _ltask_get_request_timeout(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task_get_request_timeout(task));
    return 1;
}
/// <summary>
/// 设置当前 task 的连接超时时间
/// </summary>
/// <param name="ms" type="integer">超时毫秒数</param>
/// <returns>无</returns>
static int32_t _ltask_set_connect_timeout(lua_State *lua) {
    uint32_t ms = (uint32_t)luaL_checkinteger(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_set_connect_timeout(task, ms);
    return 0;
}
/// <summary>
/// 获取当前 task 的连接超时时间
/// </summary>
/// <param>无</param>
/// <returns type="integer">超时毫秒数</returns>
static int32_t _ltask_get_connect_timeout(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task_get_connect_timeout(task));
    return 1;
}
/// <summary>
/// 设置当前 task 的网络读取超时时间
/// </summary>
/// <param name="ms" type="integer">超时毫秒数</param>
/// <returns>无</returns>
static int32_t _ltask_set_netread_timeout(lua_State *lua) {
    uint32_t ms = (uint32_t)luaL_checkinteger(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_set_netread_timeout(task, ms);
    return 0;
}
/// <summary>
/// 获取当前 task 的网络读取超时时间
/// </summary>
/// <param>无</param>
/// <returns type="integer">超时毫秒数</returns>
static int32_t _ltask_get_netread_timeout(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task_get_netread_timeout(task));
    return 1;
}
/// <summary>
/// 设置当前 task 调度优先级。每 +8 翻倍,每 +1 +12.5%;0..TASK_PRIORITY_MAX (16),超界自动 clamp
/// </summary>
/// <param name="priority" type="integer">0..16</param>
/// <returns>无</returns>
static int32_t _ltask_set_priority(lua_State *lua) {
    int32_t prio = (int32_t)luaL_checkinteger(lua, 1);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_set_priority(task, prio);
    return 0;
}
/// <summary>
/// 获取当前 task 调度优先级
/// </summary>
/// <param>无</param>
/// <returns type="integer">0..TASK_PRIORITY_MAX (16)</returns>
static int32_t _ltask_get_priority(lua_State *lua) {
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task_get_priority(task));
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
        { "isclosing", _ltask_isclosing },
        { "get_type", _ltask_get_type},
        { "name", _ltask_name },
        { "handle", _ltask_handle },
        { "timer_ms", _ltask_timer_ms },
        { "stat", _ltask_stat },
        { "mem", _ltask_mem },
        { "memlimit", _ltask_memlimit },
        { "active", _ltask_active },
        { "trap", _ltask_trap },

        { "set_request_timeout", _ltask_set_request_timeout },
        { "get_request_timeout", _ltask_get_request_timeout },
        { "set_connect_timeout", _ltask_set_connect_timeout },
        { "get_connect_timeout", _ltask_get_connect_timeout },
        { "set_netread_timeout", _ltask_set_netread_timeout },
        { "get_netread_timeout", _ltask_get_netread_timeout },
        { "set_priority", _ltask_set_priority },
        { "get_priority", _ltask_get_priority },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
