#include "lbind/lpub.h"

#define MT_HASH_RING "_hash_ring_ctx"
#define MT_LOAD_TREND "_load_trend_ctx"
#define MT_POPEN      "_popen_ctx"

/// <summary>
/// 设置全局日志输出等级
/// </summary>
/// <param name="lv" type="integer">日志等级，参考 LOG_LEVEL</param>
/// <returns>无</returns>
static int32_t _lutils_log_setlv(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
    log_setlv(lv);
    return 0;
}
/// <summary>
/// 获取当前全局日志输出等级
/// </summary>
/// <param>无</param>
/// <returns type="integer">当前日志等级，参考 LOG_LEVEL</returns>
static int32_t _lutils_log_getlv(lua_State *lua) {
    lua_pushinteger(lua, (lua_Integer)log_getlv());
    return 1;
}
/// <summary>
/// 输出一条日志，自动附带调用文件名、行号、task 名称（若存在）
/// </summary>
/// <param name="lv" type="integer">日志等级，参考 LOG_LEVEL</param>
/// <param name="file" type="string">调用方文件名</param>
/// <param name="line" type="integer">调用方行号</param>
/// <param name="log" type="string">日志正文</param>
/// <returns>无</returns>
static int32_t _lutils_log(lua_State *lua) {
    LOG_LEVEL lv = (LOG_LEVEL)luaL_checkinteger(lua, 1);
    const char *file = luaL_checkstring(lua, 2);
    int32_t line = (int32_t)luaL_checkinteger(lua, 3);
    const char *log = luaL_checkstring(lua, 4);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        slog(lv, "[%s %d] %s", __FILENAME__(file), line, log);
    } else {
        slog(lv, "[%s %d][%s] %s", __FILENAME__(file), line, _NAME_OR(task->name), log);
    }
    return 0;
}
/// <summary>
/// 将 C userdata 指针按指定长度转换为 Lua 字符串
/// </summary>
/// <param name="data" type="lightuserdata">C 指针；为 nil 时函数返回 nil</param>
/// <param name="size" type="integer">数据字节数</param>
/// <returns type="string?">转换后的字符串；data 为 nil 时返回 nil</returns>
static int32_t _lutils_ud_str(lua_State *lua) {
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type || LUA_TNONE == type) {
        lua_pushnil(lua);
        return 1;
    }
    LUACHECK_LUDATA(lua, 1);
    void *data = lua_touserdata(lua, 1);
    size_t size = (size_t)luaL_checkinteger(lua, 2);
    lua_pushlstring(lua, data, size);
    return 1;
}
/// <summary>
/// 释放 C 堆分配的 light userdata 指针；非 light userdata 时不操作（full userdata 由 GC 管理）
/// </summary>
/// <param name="data" type="lightuserdata">待释放的 C 堆指针</param>
/// <returns>无</returns>
static int32_t _lutils_ud_free(lua_State *lua) {
    /* 只释放 light userdata（lua_pushlightuserdata 推入的 C 堆指针）。
     * full userdata 由 Lua GC 管理，不得手动 free：
     * GC 会在对象回收时再次释放同一块内存，导致 double-free 崩溃。*/
    if (!lua_islightuserdata(lua, 1)) {
        return 0;
    }
    void *data = lua_touserdata(lua, 1);
    FREE(data);
    return 0;
}
/// <summary>
/// 将二进制数据编码为十六进制字符串
/// </summary>
/// <param name="data" type="string|lightuserdata">待编码数据；为字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="string">十六进制字符串</returns>
static int32_t _lutils_hex(lua_State *lua) {
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 1, &size, NULL);
    luaL_Buffer lbuf;
    char *out = luaL_buffinitsize(lua, &lbuf, HEX_ENSIZE(size));
    tohex(data, size, out);
    luaL_pushresultsize(&lbuf, strlen(out));
    return 1;
}
/// <summary>
/// 生成全局唯一 id（基于 createid 实现的单调递增 64 位整数）
/// </summary>
/// <param>无</param>
/// <returns type="integer">全局唯一 id</returns>
static int32_t _lutils_id(lua_State *lua) {
    lua_pushinteger(lua, createid());
    return 1;
}
/// <summary>
/// 从 createid 生成的 ID 中解析出服务器 id（高 16 位）
/// </summary>
/// <param name="id" type="integer">createid 返回的 ID</param>
/// <returns type="integer">服务器 id（与 serviceid 设置值一致，范围 0..0x7FFF）</returns>
static int32_t _lutils_parse_svid(lua_State *lua) {
    uint64_t id = (uint64_t)luaL_checkinteger(lua, 1);
    lua_pushinteger(lua, parse_svid(id));
    return 1;
}
/// <summary>
/// 生成 N 字节密码学安全随机数据（Linux getrandom / BSD arc4random / Windows BCryptGenRandom / Solaris /dev/urandom）
/// </summary>
/// <param name="lens" type="integer">随机字节数</param>
/// <returns type="string?">随机字节字符串；失败返回 nil</returns>
static int32_t _lutils_csprng_rand(lua_State *lua) {
    size_t n = (size_t)luaL_checkinteger(lua, 1);
    if (0 == n) {
        lua_pushlstring(lua, "", 0);
        return 1;
    }
    luaL_Buffer lbuf;
    char *buf = luaL_buffinitsize(lua, &lbuf, n);
    if (ERR_OK != csprng_rand(buf, n)) {
        lua_pushnil(lua);
        return 1;
    }
    luaL_pushresultsize(&lbuf, n);
    return 1;
}
/// <summary>
/// 获取指定 fd 对端的 IP 地址和端口号
/// </summary>
/// <param name="fd" type="integer">socket 文件描述符</param>
/// <returns type="string?">对端 IP；fd 无效或获取失败时返回 nil</returns>
/// <returns type="integer?">对端端口；仅在第一个返回值非 nil 时有效</returns>
static int32_t _lutils_remote_addr(lua_State *lua) {
    netaddr_ctx addr;
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    if (-1 == fd) {
        lua_pushnil(lua);
        return 1;
    }
    if (ERR_OK != netaddr_remote(&addr, fd)) {
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
//srey.utils
LUAMOD_API int luaopen_utils(lua_State *lua) {
    luaL_Reg reg[] = {
        { "log_setlv", _lutils_log_setlv },
        { "log_getlv", _lutils_log_getlv },
        { "log", _lutils_log },
        { "ud_free", _lutils_ud_free },
        { "ud_str", _lutils_ud_str },
        { "hex", _lutils_hex },
        { "id", _lutils_id },
        { "parse_svid", _lutils_parse_svid },
        { "csprng_rand", _lutils_csprng_rand },
        { "remote_addr", _lutils_remote_addr },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 创建一致性哈希环上下文（hashring.new）
/// </summary>
/// <param>无</param>
/// <returns type="_hash_ring_ctx">哈希环对象</returns>
static int32_t _lhash_ring_new(lua_State *lua) {
    hash_ring_ctx *ring = lua_newuserdata(lua, sizeof(hash_ring_ctx));
    hash_ring_init(ring);
    ASSOC_MTABLE(lua, MT_HASH_RING);
    return 1;
}
/// <summary>
/// 释放哈希环内部资源（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <returns>无</returns>
static int32_t _lhash_ring_free(lua_State *lua) {
    hash_ring_ctx *ring = luaL_checkudata(lua, 1, MT_HASH_RING);
    hash_ring_free(ring);
    return 0;
}
/// <summary>
/// 向哈希环添加节点（含虚拟节点）
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <param name="nreplicas" type="integer">虚拟节点数</param>
/// <param name="name" type="string|lightuserdata">节点名称；为字符串时长度自动取得</param>
/// <param name="lens" type="integer?">name 为 lightuserdata 时必填，表示名称字节数</param>
/// <returns type="boolean">成功 true，失败 false</returns>
static int32_t _lhash_ring_add(lua_State *lua) {
    hash_ring_ctx *ring = luaL_checkudata(lua, 1, MT_HASH_RING);
    uint32_t nreplicas = (uint32_t)luaL_checkinteger(lua, 2);
    size_t lens;
    void *name = NULL;
    int32_t type = lua_type(lua, 3);
    if (LUA_TSTRING == type) {
        name = (void *)luaL_checklstring(lua, 3, &lens);
    } else if (LUA_TLIGHTUSERDATA == type) {
        name = lua_touserdata(lua, 3);
        lens = (size_t)luaL_checkinteger(lua, 4);
    } else {
        return luaL_argerror(lua, 3, "string or light userdata expected");
    }
    if (ERR_OK == hash_ring_add(ring, name, lens, nreplicas)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 从哈希环中移除指定节点（含其全部虚拟节点）
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <param name="name" type="string|lightuserdata">节点名称；为字符串时长度自动取得</param>
/// <param name="lens" type="integer?">name 为 lightuserdata 时必填，表示名称字节数</param>
/// <returns>无</returns>
static int32_t _lhash_ring_remove(lua_State *lua) {
    hash_ring_ctx *ring = luaL_checkudata(lua, 1, MT_HASH_RING);
    size_t lens;
    void *name = NULL;
    int32_t type = lua_type(lua, 2);
    if (LUA_TSTRING == type) {
        name = (void *)luaL_checklstring(lua, 2, &lens);
    } else if (LUA_TLIGHTUSERDATA == type) {
        name = lua_touserdata(lua, 2);
        lens = (size_t)luaL_checkinteger(lua, 3);
    } else {
        return luaL_argerror(lua, 2, "string or light userdata expected");
    }
    hash_ring_remove(ring, name, lens);
    return 0;
}
/// <summary>
/// 按 key 在哈希环上查找最近节点
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <param name="key" type="string|lightuserdata">查找 key；为字符串时长度自动取得</param>
/// <param name="lens" type="integer?">key 为 lightuserdata 时必填，表示 key 字节数</param>
/// <returns type="string?">命中节点名称；环为空时返回 nil</returns>
static int32_t _lhash_ring_find(lua_State *lua) {
    hash_ring_ctx *ring = luaL_checkudata(lua, 1, MT_HASH_RING);
    size_t lens;
    void *key = NULL;
    int32_t type = lua_type(lua, 2);
    if (LUA_TSTRING == type) {
        key = (void *)luaL_checklstring(lua, 2, &lens);
    } else if (LUA_TLIGHTUSERDATA == type) {
        key = lua_touserdata(lua, 2);
        lens = (size_t)luaL_checkinteger(lua, 3);
    } else {
        return luaL_argerror(lua, 2, "string or light userdata expected");
    }
    hash_ring_node *node = hash_ring_find(ring, key, lens);
    if (NULL == node) {
        lua_pushnil(lua);
    } else {
        lua_pushlstring(lua, node->name, node->lens);
    }
    return 1;
}
/// <summary>
/// 将哈希环所有节点信息打印到日志（调试用途）
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <returns>无</returns>
static int32_t _lhash_ring_print(lua_State *lua) {
    hash_ring_ctx *ring = luaL_checkudata(lua, 1, MT_HASH_RING);
    hash_ring_print(ring);
    return 0;
}
//srey.hashring
LUAMOD_API int luaopen_hashring(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lhash_ring_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "add", _lhash_ring_add },
        { "remove", _lhash_ring_remove },
        { "find", _lhash_ring_find },
        { "print", _lhash_ring_print },
        { "__gc", _lhash_ring_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_HASH_RING, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 创建负载趋势检测器（trend.new）
/// </summary>
/// <returns type="_load_trend_ctx">负载趋势对象</returns>
static int32_t _ltrend_new(lua_State *lua) {
    load_trend_ctx *trend = lua_newuserdata(lua, sizeof(load_trend_ctx));
    load_trend_init(trend);
    ASSOC_MTABLE(lua, MT_LOAD_TREND);
    return 1;
}
/// <summary>
/// 采样当前值并基于趋势判断负载是否繁忙；典型阈值 (4, 5) 表示采样值跌幅超过 20% 视为繁忙
/// </summary>
/// <param name="self" type="userdata">负载趋势对象</param>
/// <param name="cur" type="integer">当前采样值</param>
/// <param name="busy_num" type="integer">繁忙阈值分子</param>
/// <param name="busy_den" type="integer">繁忙阈值分母</param>
/// <returns type="boolean">繁忙 true；不忙 false</returns>
static int32_t _ltrend_busy(lua_State *lua) {
    load_trend_ctx *trend = luaL_checkudata(lua, 1, MT_LOAD_TREND);
    size_t cur = (size_t)luaL_checkinteger(lua, 2);
    uint32_t busy_num = (uint32_t)luaL_checkinteger(lua, 3);
    uint32_t busy_den = (uint32_t)luaL_checkinteger(lua, 4);
    lua_pushboolean(lua, load_trend_busy(trend, cur, busy_num, busy_den));
    return 1;
}
//srey.trend
LUAMOD_API int luaopen_trend(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _ltrend_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "busy", _ltrend_busy },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_LOAD_TREND, reg_new, reg_func);
    return 1;
}
/// <summary>
/// 启动子进程；mode="r" 读子进程 stdout，mode="w" 写子进程 stdin
/// </summary>
/// <param name="cmd" type="string">命令行字符串</param>
/// <param name="mode" type="string">"r" 或 "w"</param>
/// <returns type="_popen_ctx?">popen 对象；启动失败返回 nil</returns>
static int32_t _lpopen_new(lua_State *lua) {
    const char *cmd = luaL_checkstring(lua, 1);
    const char *mode = luaL_checkstring(lua, 2);
    popen_ctx *ctx = lua_newuserdata(lua, sizeof(popen_ctx));
    // 提前绑 metatable,失败时 lua_pop 弹引用后 GC 走 _lpopen_gc → popen_close + popen_free 兜底
    ASSOC_MTABLE(lua, MT_POPEN);
    if (ERR_OK != popen_startup(ctx, cmd, mode)) {
        lua_pop(lua, 1);
        lua_pushnil(lua);
        return 1;
    }
    return 1;
}
/// <summary>
/// 阻塞等待子进程退出（最多 ms 毫秒）
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <param name="ms" type="integer">超时毫秒数</param>
/// <returns type="boolean">true 表示已退出，false 表示超时仍在运行</returns>
static int32_t _lpopen_waitexit(lua_State *lua) {
    popen_ctx *ctx = luaL_checkudata(lua, 1, MT_POPEN);
    uint32_t ms = (uint32_t)luaL_checkinteger(lua, 2);
    lua_pushboolean(lua, ERR_OK == popen_waitexit(ctx, ms));
    return 1;
}
/// <summary>
/// 获取子进程退出码（须在 waitexit 之后调用）
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <returns type="integer">退出码；非 Windows 平台可能取不到</returns>
static int32_t _lpopen_exitcode(lua_State *lua) {
    popen_ctx *ctx = luaL_checkudata(lua, 1, MT_POPEN);
    lua_pushinteger(lua, popen_exitcode(ctx));
    return 1;
}
/// <summary>
/// 阻塞读子进程 stdout，直到读到 EOF 或读到 max_lens 字节
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <param name="max_lens" type="integer?">最大读取字节数，默认 65536</param>
/// <returns type="string">读到的内容（可为空字符串）</returns>
static int32_t _lpopen_read(lua_State *lua) {
    popen_ctx *ctx = luaL_checkudata(lua, 1, MT_POPEN);
    size_t cap = (size_t)luaL_optinteger(lua, 2, 65536);
    luaL_Buffer lbuf;
    luaL_buffinit(lua, &lbuf);
    char tmp[4096];
    size_t total = 0;
    int32_t nread;
    while (total < cap) {
        size_t want = sizeof(tmp);
        if (want > cap - total) {
            want = cap - total;
        }
        nread = popen_read(ctx, tmp, want);
        if (nread <= 0) {
            break;
        }
        luaL_addlstring(&lbuf, tmp, (size_t)nread);
        total += (size_t)nread;
    }
    luaL_pushresult(&lbuf);
    return 1;
}
/// <summary>
/// 向子进程 stdin 写入数据；popen2.c 契约：以 '\n' 结尾才会真正执行写入
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <param name="data" type="string">待写入数据</param>
/// <returns type="integer">实际写入的字节数；失败返回 -1</returns>
static int32_t _lpopen_write(lua_State *lua) {
    popen_ctx *ctx = luaL_checkudata(lua, 1, MT_POPEN);
    size_t lens;
    const char *data = luaL_checklstring(lua, 2, &lens);
    lua_pushinteger(lua, popen_write(ctx, data, lens));
    return 1;
}
/// <summary>
/// 主动终止子进程并关闭句柄（不释放结构体本身，可继续调用 exitcode）
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <returns>无</returns>
static int32_t _lpopen_close(lua_State *lua) {
    popen_ctx *ctx = luaL_checkudata(lua, 1, MT_POPEN);
    popen_close(ctx);
    return 0;
}
// __gc：先 close 防止 zombie 子进程，再 free 释放管道/句柄
static int32_t _lpopen_gc(lua_State *lua) {
    popen_ctx *ctx = luaL_checkudata(lua, 1, MT_POPEN);
    popen_close(ctx);
    popen_free(ctx);
    return 0;
}
//srey.popen
LUAMOD_API int luaopen_popen(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lpopen_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "waitexit", _lpopen_waitexit },
        { "exitcode", _lpopen_exitcode },
        { "read",     _lpopen_read },
        { "write",    _lpopen_write },
        { "close",    _lpopen_close },
        { "__gc",     _lpopen_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_POPEN, reg_new, reg_func);
    return 1;
}
