#include "lbind/ltask.h"
#if ENABLE_LUA_BYTECACHE
#include "lbind/lbytecache.h"
#endif

// 向 Lua table 中写入整数字段（setfield 比 push+settable 省一次 lock 配对）。
#define LUA_TB_NUMBER(key, val)\
    do {\
        lua_pushinteger(lua, val);\
        lua_setfield(lua, -2, key);\
    } while (0)

// Lua task 上下文：保存 Lua 虚拟机、消息分发函数引用、计时器、内存统计
#define PATH_SEP_NAME "_pathsep" // Lua 全局变量名：路径分隔符字符串
#define MSG_DISP_FUNC "message_dispatch" // Lua 脚本中消息分发回调函数名

typedef struct ltask_ctx {
    int32_t    ref;       // message_dispatch 函数在 Lua 注册表中的引用 id
    int32_t    msg_mtref;    // 无载荷消息的元表 ref（只有 __index）；0=尚未创建，luaL_ref 恒不返回 0
    int32_t    msg_mtref_gc; // 带载荷消息的元表 ref（__index + __gc）；同上
    size_t     mem;       // 当前 Lua 累计内存（字节，单 worker 串行操作，无需 atomic）
    task_ctx  *task;      // 回指 task_ctx，供 allocator 日志取 name
    lua_State *lua;       // 当前 task 独占的 Lua 虚拟机（主 thread）
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
    lpub_reg_sock(lua);
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
// 打印栈顶错误对象并弹掉。Lua 允许 error() 抛任意值,非字符串时 lua_tostring 返回 NULL,
// 直接喂 LOG_ERROR 的 %s 是 UB
static void _ltask_log_err(lua_State *lua) {
    const char *err = lua_tostring(lua, -1);
    if (NULL != err) {
        LOG_ERROR("%s", err);
    } else {
        LOG_ERROR("error object is %s (%p), not a string.", luaL_typename(lua, -1), lua_topointer(lua, -1));
    }
    lua_pop(lua, 1);
}
// 搜索并执行指定 Lua 脚本文件，执行失败时记录错误日志
static int32_t _ltask_dofile(lua_State *lua, const char *file) {
    char path[PATH_LENS];
    if (ERR_OK != _ltask_searchfile(file, path)) {
        LOG_ERROR("cannot find %s:, no such file.", file);
        return ERR_FAILED;
    }
    if (LUA_OK != luaL_dofile(lua, path)) {
        _ltask_log_err(lua);
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
        _ltask_log_err(lua);
        return ERR_FAILED;
    }
    int32_t nargs = 0;
    if (NULL != from && arg_top >= arg_start) {
        nargs = arg_top - arg_start + 1;
        // 新建的 lua_State 只有 45 个栈位，而 nargs 由业务脚本随便给。lua_push* 系列只推进栈顶
        // 不扩容，release 构建下越界那道 api_check 又是空操作，写满就直接写到栈数组外面去了
        if (!lua_checkstack(lua, nargs)) {
            LOG_ERROR("cannot run %s: stack overflow, %d args.", file, nargs);
            return ERR_FAILED;
        }
        for (int32_t i = arg_start; i <= arg_top; i++) {
            _ltask_copy_arg(from, i, lua);
        }
    }
    if (LUA_OK != lua_pcall(lua, nargs, 0, 0)) {
        _ltask_log_err(lua);
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
    lua_State *lua = _ltask_luainit(task, ltask);
    if (NULL == lua) {
        return ERR_FAILED;
    }
    // 在跑 chunk 之前落定：脚本顶层就能调各绑定，它们直接解引用。
    // 失败路径要把 lua 一并清空——它兼作 _ltask_arg_free 的"已初始化"标志，留着就是二次 lua_close
    ltask->lua = lua;
    if (ERR_OK != _ltask_dofile_args(lua, file, from, arg_start, arg_top)) {
        lua_close(lua);
        ltask->lua = NULL;
        return ERR_FAILED;
    }
    lua_getglobal(lua, MSG_DISP_FUNC);
    if (LUA_TFUNCTION != lua_type(lua, -1)) {
        lua_close(lua);
        ltask->lua = NULL;
        LOG_ERROR("not find function %s.", MSG_DISP_FUNC);
        return ERR_FAILED;
    }
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
// 消息对象的 __gc：payload 就是 message_ctx 本身，直接交给 _message_clean。
// 字段在 Lua 侧不可写（userdata 无 __newindex），故不存在改 mtype/data 换掉释放契约的问题
static int32_t _msg_clean(lua_State *lua) {
    message_ctx *ud = (message_ctx *)lua_touserdata(lua, 1);
    if (NULL == ud) {
        return 0;
    }
    // shared 非 NULL 走广播 ref-- 分支；shared 为 NULL 时仅 data 非 NULL 才需清理
    if (NULL != ud->shared
        || NULL != ud->data) {
        _message_clean(ud);
    }
    return 0;
}
// 消息对象的 __index：按字段名取 message_ctx 里的值。按长度 + 首字符分发，不做全量串比。
// data/shared 为空时返回 nil 而不是 0，调用方有 if msg.data then 这类真值判断；
// ip/port/udata 只有 RECVFROM 才从 recvfrom_ctx 里取，别的类型 data 不是那个结构
static int32_t _msg_index(lua_State *lua) {
    message_ctx *ud = (message_ctx *)lua_touserdata(lua, 1);
    size_t len;
    const char *k = lua_tolstring(lua, 2, &len);
    if (NULL == ud
        || NULL == k) {
        lua_pushnil(lua);
        return 1;
    }
    // 按字段长度 + 首字符分发
    switch (len) {
    case 2:// ip sk
        // 连接标识只经 sk 给出,按 skid 缓存在注册表里,取多少次都是同一个 userdata
        if ('s' == k[0]) {
            lpub_push_sock_msg(lua, &ud->sk);
            return 1;
        }
        if ('i' == k[0]) {
            if (MSG_TYPE_RECVFROM != ud->mtype
                || NULL == ud->data) {
                break;
            }
            char ip[IP_LENS];
            netaddr_ip(&((recvfrom_ctx *)ud->data)->addr, ip);
            lua_pushstring(lua, ip);
            return 1;
        }
        break;
    case 3:// src
        if ('s' == k[0]) {
            lua_pushinteger(lua, (lua_Integer)ud->src);
            return 1;
        }
        break;
    case 4:// sess size erro data port
        switch (k[0]) {
        case 's':// sess size,再比第二字符
            if ('e' == k[1]) {
                lua_pushinteger(lua, (lua_Integer)ud->sess);
                return 1;
            }
            if ('i' == k[1]) {
                lua_pushinteger(lua, (lua_Integer)ud->size);
                return 1;
            }
            break;
        case 'e':// erro
            lua_pushinteger(lua, ud->erro);
            return 1;
        case 'd':// data
            if (NULL == ud->data) {
                break;
            }
            lua_pushlightuserdata(lua, ud->data);
            return 1;
        case 'p':// port
            if (MSG_TYPE_RECVFROM != ud->mtype
                || NULL == ud->data) {
                break;
            }
            lua_pushinteger(lua, netaddr_port(&((recvfrom_ctx *)ud->data)->addr));
            return 1;
        default:
            break;
        }
        break;
    case 5:// mtype slice udata
        if ('m' == k[0]) {
            lua_pushinteger(lua, ud->mtype);
            return 1;
        }
        if ('s' == k[0]) {
            lua_pushinteger(lua, ud->slice);
            return 1;
        }
        if ('u' == k[0]) {
            if (MSG_TYPE_RECVFROM != ud->mtype
                || NULL == ud->data) {
                break;
            }
            lua_pushlightuserdata(lua, ((recvfrom_ctx *)ud->data)->data);
            return 1;
        }
        break;
    case 6:// client shared
        if ('c' == k[0]) {
            lua_pushinteger(lua, ud->client);
            return 1;
        }
        if ('s' == k[0]) {
            if (NULL == ud->shared) {
                break;
            }
            lua_pushlightuserdata(lua, ud->shared);
            return 1;
        }
        break;
    case 7:// subtype
        if ('s' == k[0]) {
            lua_pushinteger(lua, ud->subtype);
            return 1;
        }
        break;
    default:
        break;
    }
    lua_pushnil(lua);
    return 1;
}
// 取消息对象的元表压栈。分两张：带载荷的才挂 __gc，无载荷的不挂——挂了 __gc 的对象
// 回收时要多走一遍 finalizer 链，没东西可释放的消息不该付这笔。
// 元表 ref 缓存在 ltask_ctx，按整数下标直取，省掉 registry 的字符串查找
static inline void _ltask_msg_mt(lua_State *lua, ltask_ctx *ltask, int32_t withgc) {
    int32_t *ref = (0 != withgc) ? &ltask->msg_mtref_gc : &ltask->msg_mtref;
    if (0 != *ref) {
        lua_rawgeti(lua, LUA_REGISTRYINDEX, *ref);
        return;
    }
    lua_newtable(lua);
    if (0 != withgc) {
        lua_pushcfunction(lua, _msg_clean);
        lua_setfield(lua, -2, "__gc");
    }
    lua_pushcfunction(lua, _msg_index);
    lua_setfield(lua, -2, "__index");
    lua_pushstring(lua, "msg");
    lua_setfield(lua, -2, "__metatable");
    lua_pushvalue(lua, -1);
    *ref = luaL_ref(lua, LUA_REGISTRYINDEX);
}
// 把 message_ctx 整个拷进 userdata 交给 Lua，字段经 __index 按需取。
// 不建表是因为表要为每条消息付一次 hash 部分的分配（16 槽）和逐字段 setfield，
// 而 Lua 侧多数时候只读其中几个
static inline void _ltask_push_msg(lua_State *lua, ltask_ctx *ltask, message_ctx *msg) {
    message_ctx *ud = (message_ctx *)lua_newuserdatauv(lua, sizeof(message_ctx), 0);
    *ud = *msg;
    _ltask_msg_mt(lua, ltask, ERR_OK == _message_should_clean(msg));
    lua_setmetatable(lua, -2);
}
// task 消息分发回调：从注册表取消息分发函数，打包消息后调用 Lua
static void _ltask_run(task_dispatch_arg *arg) {
    ltask_ctx *ltask = arg->task->arg;
    lua_rawgeti(ltask->lua, LUA_REGISTRYINDEX, ltask->ref);
    _ltask_push_msg(ltask->lua, ltask, arg->msg);
    if (LUA_OK != lua_pcall(ltask->lua, 1, 0, 0)) {
        _ltask_log_err(ltask->lua);
    }
    // 连接关了才摘缓存,且必须排在分发之后——业务回调里还要用这条 sk
    if (MSG_TYPE_CLOSE == arg->msg->mtype) {
        lpub_sock_uncache(ltask->lua, arg->msg->sk.skid);
    }
}
/// <summary>
/// 注册一个新 task。第 4 起的变参会作为参数传给脚本 chunk，
/// 脚本顶层用 `local a, b, ... = ...` 接收。变参仅支持 nil/bool/number/string 基本类型，
/// 其他类型（table/function/userdata 等）会退化为 nil
/// </summary>
/// <param name="file" type="string">脚本文件名（不含 .lua 后缀，支持 a.b 形式映射到目录）</param>
/// <param name="name" type="string?">字符串 task 名；nil 或空串=匿名（仅有句柄）</param>
/// <param name="quecap" type="integer">消息队列容量（条数）；0 用默认 TASK_QUEUE_CAP。
/// 取值须在 [0, UINT32_MAX]，越界直接报错而不是截断——截断的话 0x100000000 会变成 0、
/// 被 task_new 悄悄换成默认 TASK_QUEUE_CAP，调用方从返回值看不出自己要的容量根本没生效</param>
/// <param name="..." type="any">传给脚本的可变参数（nil/bool/number/string）</param>
/// <returns type="lightuserdata?">task 指针；失败返回 nil</returns>
static int32_t _ltask_register(lua_State *lua) {
    const char *file = luaL_checkstring(lua, 1);
    const char *name = luaL_optstring(lua, 2, NULL);
    lua_Integer cap_arg = luaL_checkinteger(lua, 3);
    if (cap_arg < 0) {
        return luaL_argerror(lua, 3, "quecap must be non-negative");
    }
    if (cap_arg > UINT32_MAX) {
        return luaL_argerror(lua, 3, "quecap exceeds UINT32_MAX");
    }
    uint32_t quecap = (uint32_t)cap_arg;
    int32_t arg_top = lua_gettop(lua);
    ltask_ctx *ltask;
    CALLOC(ltask, 1, sizeof(ltask_ctx));
    timer_init(&ltask->timer);
    task_ctx *task = task_new(g_loader, name, quecap, _ltask_run, _ltask_arg_free, ltask);
    task->type = TASK_LUA;
    if (ERR_OK != _ltask_init(task, ltask, file, lua, 4, arg_top)) {
        task_free(task);
        return lpub_rtn_nil(lua, 1);
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
    LPUB_TASK_ARG(lua, task);
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
    name_t handle = lpub_task_handle(lua, 1);
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
    LPUB_LUD_ARG(lua, task_ctx, 1, task);
    task_incref(task);
    return 0;
}
/// <summary>
/// 释放 task_grab 持有的 task 引用（引用计数 -1）
/// </summary>
/// <param name="task" type="lightuserdata">task 指针</param>
/// <returns>无</returns>
static int32_t _ltask_ungrab(lua_State *lua) {
    LPUB_LUD_ARG(lua, task_ctx, 1, task);
    task_ungrab(task);
    return 0;
}
/// <summary>
/// 查询 task 是否正在关闭
/// </summary>
/// <param name="task" type="lightuserdata?">task 指针；nil 时查询当前 task</param>
/// <returns type="boolean">关闭中返回 true</returns>
static int32_t _ltask_isclosing(lua_State *lua) {
    LPUB_TASK_ARG(lua, task);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    return lpub_rtn_bool(lua, task_isclosing(task));
}
/// <summary>
/// 获取 task 类型
/// </summary>
/// <param name="task" type="lightuserdata?">task 指针；nil 时查询当前 task</param>
/// <returns type="TASK_TYPE">任务类型</returns>
static int32_t _ltask_get_type(lua_State *lua) {
    LPUB_TASK_ARG(lua, task);
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
    LPUB_TASK_ARG(lua, task);
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
/// <returns type="integer">task 句柄</returns>
static int32_t _ltask_handle(lua_State *lua) {
    LPUB_TASK_ARG(lua, task);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    lua_pushinteger(lua, task->handle);
    return 1;
}
/// <summary>
/// 返回当前 task 计时器已运行的毫秒数
/// </summary>
/// <param>无</param>
/// <returns type="integer">已运行毫秒数</returns>
static int32_t _ltask_timer_ms(lua_State *lua) {
    LPUB_CUR_TASK(lua, task);
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
    LPUB_CUR_TASK(lua, task);
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
    LPUB_CUR_TASK(lua, task);
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
    LPUB_CUR_TASK(lua, task);
    ltask_ctx *ltask = task->arg;
    tda_init(&ltask->mem_tda, (size_t)(limit < 0 ? 0 : limit));
    return 0;
}
static int32_t _ltask_opt_timeout(lua_State *lua, task_ctx *task, uint32_t *ms) {
    lua_Integer val = luaL_checkinteger(lua, 1);
    if (val <= 0) {
        LOG_WARN("task %s, invalid timeout %"PRId64", ignored.", _NAME_OR(task->name), (int64_t)val);
        return ERR_FAILED;
    }
    *ms = (uint32_t)(val > TASK_TIMEOUT_MAX ? TASK_TIMEOUT_MAX : val);
    return ERR_OK;
}
/// <summary>
/// 设置当前 task 的请求超时时间
/// </summary>
/// <param name="ms" type="integer">超时毫秒数，须 大于 0；超过 TASK_TIMEOUT_MAX 时 clamp 到该上界</param>
/// <returns>无</returns>
static int32_t _ltask_set_request_timeout(lua_State *lua) {
    LPUB_CUR_TASK(lua, task);
    uint32_t ms;
    if (ERR_OK != _ltask_opt_timeout(lua, task, &ms)) {
        return 0;
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
    LPUB_CUR_TASK(lua, task);
    lua_pushinteger(lua, task_get_request_timeout(task));
    return 1;
}
/// <summary>
/// 设置当前 task 的连接超时时间
/// </summary>
/// <param name="ms" type="integer">超时毫秒数，须 大于 0；超过 TASK_TIMEOUT_MAX 时 clamp 到该上界</param>
/// <returns>无</returns>
static int32_t _ltask_set_connect_timeout(lua_State *lua) {
    LPUB_CUR_TASK(lua, task);
    uint32_t ms;
    if (ERR_OK != _ltask_opt_timeout(lua, task, &ms)) {
        return 0;
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
    LPUB_CUR_TASK(lua, task);
    lua_pushinteger(lua, task_get_connect_timeout(task));
    return 1;
}
/// <summary>
/// 设置当前 task 的网络读取超时时间
/// </summary>
/// <param name="ms" type="integer">超时毫秒数，须 大于 0；超过 TASK_TIMEOUT_MAX 时 clamp 到该上界</param>
/// <returns>无</returns>
static int32_t _ltask_set_netread_timeout(lua_State *lua) {
    LPUB_CUR_TASK(lua, task);
    uint32_t ms;
    if (ERR_OK != _ltask_opt_timeout(lua, task, &ms)) {
        return 0;
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
    LPUB_CUR_TASK(lua, task);
    lua_pushinteger(lua, task_get_netread_timeout(task));
    return 1;
}
/// <summary>
/// 设置当前 task 调度优先级。每 +8 翻倍,每 +1 +12.5%;0..TASK_PRIORITY_MAX (16),超界自动 clamp。
/// 超出 int32 的值先压到 int32 边界再交给 clamp,不会被截断成另一个合法优先级
/// </summary>
/// <param name="priority" type="integer">0..16</param>
/// <returns>无</returns>
static int32_t _ltask_set_priority(lua_State *lua) {
    lua_Integer prio = luaL_checkinteger(lua, 1);
    if (prio < INT32_MIN) {
        prio = INT32_MIN;
    } else if (prio > INT32_MAX) {
        prio = INT32_MAX;
    }
    LPUB_CUR_TASK(lua, task);
    task_set_priority(task, (int32_t)prio);
    return 0;
}
/// <summary>
/// 获取当前 task 调度优先级
/// </summary>
/// <param>无</param>
/// <returns type="integer">0..TASK_PRIORITY_MAX (16)</returns>
static int32_t _ltask_get_priority(lua_State *lua) {
    LPUB_CUR_TASK(lua, task);
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
