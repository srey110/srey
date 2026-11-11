#include "lbind/lpub.h"

#define MT_POPEN "_popen_ctx"

/// <summary>
/// 启动子进程；mode 含 "r" 读子进程 stdout（stderr 并入同一通道），含 "w" 写子进程 stdin，"rw" 两者都要。
/// POSIX 经 /bin/sh -c 执行，Windows 直接起可执行文件、不经 cmd.exe；POSIX 上子进程的 SIGPIPE 为默认处置，
/// 不接管的 stdin 是 /dev/null。Windows 上别和 io.popen / os.execute 并发用：它们起的进程会带走这里在建的管道端，
/// 拖迟读到 EOF（见 popen_startup）
/// </summary>
/// <param name="cmd" type="string">命令行字符串</param>
/// <param name="mode" type="string">"r"、"w" 或 "rw"</param>
/// <returns type="_popen_ctx?">popen 对象；启动失败返回 nil</returns>
static int32_t _lpopen_new(lua_State *lua) {
    const char *cmd = lpub_check_cstr(lua, 1);
    const char *mode = lpub_check_cstr(lua, 2);
    popen_ctx *ctx = lua_newuserdata(lua, sizeof(popen_ctx));
    // 提前绑 metatable,失败时 lua_pop 弹引用后 GC 走 _lpopen_gc → popen_free 兜底
    ASSOC_MTABLE(lua, MT_POPEN);
    if (ERR_OK != popen_startup(ctx, cmd, mode)) {
        lua_pop(lua, 1);
        return lpub_rtn_nil(lua, 1);
    }
    return 1;
}
/// <summary>
/// 阻塞等待子进程退出（最多 ms 毫秒）。阻塞的是整条 worker 线程而不只是当前协程，期间同一 worker 上的其他 task 都停着。POSIX 上只记退出码、不收尸，用完调 close，否则僵尸留到对象回收
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <param name="ms" type="integer">超时毫秒数</param>
/// <returns type="boolean">true 表示已退出，false 表示超时仍在运行</returns>
static int32_t _lpopen_waitexit(lua_State *lua) {
    popen_ctx *ctx = lpub_check_udata(lua, 1, MT_POPEN);
    uint32_t ms = lpub_check_u32(lua, 2, "ms out of range");
    return lpub_rtn_bool(lua, ERR_OK == popen_waitexit(ctx, ms));
}
/// <summary>
/// 获取子进程退出码，要等它退出先 waitexit
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <returns type="integer">退出码；被信号终止、被 close 结束、还没退出或出错时 -1；
/// Windows 上异常结束的子进程给的是异常码（如 0xC0000005，为负数）</returns>
static int32_t _lpopen_exitcode(lua_State *lua) {
    popen_ctx *ctx = lpub_check_udata(lua, 1, MT_POPEN);
    lua_pushinteger(lua, popen_exitcode(ctx));
    return 1;
}
/// <summary>
/// 非阻塞读取子进程 stdout 当前已缓冲的数据，最多 max_lens 字节；底层 popen_read 一旦探测到当前无数据即返回，
/// 故本函数不等待、不保证读到 EOF：子进程尚未产出的输出读不到。输出可能超过管道缓冲时要边读边等：
/// 循环 read 到第二个返回值为 true 再 waitexit（每轮夹一次 srey.sleep，否则是在 worker 上忙等），先 waitexit 再读子进程会卡在写上退不了
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <param name="max_lens" type="integer?">最大读取字节数，取值 [0, INT32_MAX]，默认 65536</param>
/// <returns type="string?">读到的内容（可为空字符串；当前无数据即返回，不阻塞等待）；
/// 出错（mode 不含 r、已 close、读出错）且一个字节都没读到时 nil，读到一部分再出错照旧返回已读的</returns>
/// <returns type="boolean">是否已读到流末尾：true=写端全关、输出已完整；false=读满 max_lens/暂无数据/出错即停，可能不完整</returns>
static int32_t _lpopen_read(lua_State *lua) {
    popen_ctx *ctx = lpub_check_udata(lua, 1, MT_POPEN);
    // 可选参数，lpub_check_lens 没有 opt 形态，故缺省分支单列；给了就按它校验上下界
    size_t cap = lua_isnoneornil(lua, 2) ? 65536 : lpub_check_lens(lua, 2, INT32_MAX);
    const size_t chunk = 4096;
    luaL_Buffer lbuf;
    luaL_buffinit(lua, &lbuf);
    size_t total = 0;
    size_t want;
    int32_t nread = 0;
    int32_t eof = 0;
    while (total < cap) {
        want = chunk;
        if (want > cap - total) {
            want = cap - total;
        }
        nread = popen_read(ctx, luaL_prepbuffsize(&lbuf, want), want, &eof);
        if (nread <= 0) {
            break;
        }
        luaL_addsize(&lbuf, (size_t)nread);
        total += (size_t)nread;
    }
    luaL_pushresult(&lbuf);
    // 出错时给 nil，与"暂无数据"的空串分开，读到 eof 的循环才停得下来
    if (ERR_FAILED == nread
        && 0 == total) {
        lua_pop(lua, 1);
        lua_pushnil(lua);
    }
    lua_pushboolean(lua, eof);
    return 2;
}
/// <summary>
/// 向子进程 stdin 写入数据，当场写进管道；子进程按行读的话换行要自己带
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <param name="data" type="string">待写入数据</param>
/// <returns type="integer">实际写入的字节数，**可能少于 data 长度**——写端非阻塞，
/// 对端缓冲写满即返回，单次最多写 INT32_MAX 字节，剩余部分由调用方决定重试还是放弃；
/// mode 不含 "w"、已 close_stdin，或一个字节都没写进去且出错时返回 -1</returns>
static int32_t _lpopen_write(lua_State *lua) {
    popen_ctx *ctx = lpub_check_udata(lua, 1, MT_POPEN);
    size_t lens;
    const char *data = luaL_checklstring(lua, 2, &lens);
    lua_pushinteger(lua, popen_write(ctx, data, lens));
    return 1;
}
/// <summary>
/// 只关子进程的 stdin、让它读到 EOF，子进程照常运行、输出照常可读；
/// sort、wc 这类读 stdin 到 EOF 才出结果的程序靠它收尾。之后 write 返回 -1
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <returns type="boolean">true 已送出 EOF；mode 不含 "w"、已调过一次或出错时 false</returns>
static int32_t _lpopen_close_stdin(lua_State *lua) {
    popen_ctx *ctx = lpub_check_udata(lua, 1, MT_POPEN);
    return lpub_rtn_bool(lua, ERR_OK == popen_close_stdin(ctx));
}
/// <summary>
/// 主动终止子进程连同它派生的孙进程并收尸；子进程已自行退出时也照样清理剩下的孙进程
/// （Windows 只发起终止不等它结束，取 exitcode 前先 waitexit）。
/// 读写句柄要到对象回收时才关：close 之后仍可 read 剩下的输出、可取 exitcode
/// </summary>
/// <param name="self" type="userdata">popen 对象</param>
/// <returns>无</returns>
static int32_t _lpopen_close(lua_State *lua) {
    popen_ctx *ctx = lpub_check_udata(lua, 1, MT_POPEN);
    popen_close(ctx);
    return 0;
}
// __gc：popen_free 内部对没 close 过的 ctx 会自己收尾，这里不必再显式 close
static int32_t _lpopen_gc(lua_State *lua) {
    popen_ctx *ctx = lpub_check_udata(lua, 1, MT_POPEN);
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
        { "close_stdin", _lpopen_close_stdin },
        { "close",    _lpopen_close },
        { "__gc",     _lpopen_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_POPEN, reg_new, reg_func);
    return 1;
}
