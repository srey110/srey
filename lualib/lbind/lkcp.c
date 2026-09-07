#include "lbind/lpub.h"

#define MT_KCP "_kcp_ctx"

/// <summary>
/// 创建 KCP 会话句柄
/// </summary>
/// <param name="fd" type="integer">底层 UDP socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="conv" type="integer">会话号(同一 socket 内唯一,两端约定一致)</param>
/// <returns type="userdata">kcp 会话句柄(_kcp_ctx)</returns>
static int32_t _lkcp_new(lua_State *lua) {
    LPUB_CUR_TASK(lua, task);
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    uint32_t conv = lpub_check_u32(lua, 3, "conv out of range");
    kcp_ctx *kcp = lua_newuserdata(lua, sizeof(kcp_ctx));
    kcp_init(kcp, &task->loader->netev, fd, skid, conv);
    ASSOC_MTABLE(lua, MT_KCP);
    return 1;
}
/// <summary>
/// 停止并释放当前 kcp 会话(从会话表移除;之后需重新 start 才能再用),__gc 亦复用此清理
/// </summary>
/// <param name="self" type="userdata">kcp 会话句柄</param>
static int32_t _lkcp_stop(lua_State *lua) {
    kcp_ctx *kcp = luaL_checkudata(lua, 1, MT_KCP);
    kcp_stop(kcp);
    return 0;
}
// 从 idx 处 table 读整数字段 key,缺省返回 dft,越界或非整数即报错并点出字段名。
// 取值域见 lib/kcp.lua 的 kcp_config:窗口上界是绑定层策略,kcp.h 不卡
static int32_t _lkcp_cfgint(lua_State *lua, int32_t tidx, const char *key,
                            int32_t dft, int32_t lo, int32_t hi) {
    lua_getfield(lua, tidx, key);
    if (lua_isnoneornil(lua, -1)) {
        lua_pop(lua, 1);
        return dft;
    }
    int32_t isnum;
    lua_Integer v = lua_tointegerx(lua, -1, &isnum);
    if (0 == isnum) {
        return luaL_error(lua, "kcp config '%s' must be an integer", key);
    }
    if (v < lo || v > hi) {
        return luaL_error(lua, "kcp config '%s' out of range [%d, %d]", key, lo, hi);
    }
    lua_pop(lua, 1);
    return (int32_t)v;
}
/// <summary>
/// 建立会话,数据到达以当前 task 为推送目标(MSG_TYPE.RECVFROM)。
/// 本句柄已有会话未 stop 时先隐式 stop 旧会话再起新的(旧会话投出 CLOSE),不返回失败
/// </summary>
/// <param name="self" type="userdata">kcp 会话句柄</param>
/// <param name="sess" type="integer">本次会话的唤醒 sess(调用方生成,如 srey.id());0 表示不唤醒。
///   每次 start 须传新值:stop 后重启若复用旧 sess,上一会话在途的 CLOSE 会击穿本次等待</param>
/// <param name="ip" type="string">对端 IP</param>
/// <param name="port" type="integer">对端端口</param>
/// <param name="config" type="kcp_config?">KCP 可调参数,缺省用库默认,非 table 报错;
///   字段与取值域见 lib/kcp.lua 的 kcp_config</param>
/// <returns type="boolean">成功 true,失败 false</returns>
static int32_t _lkcp_start(lua_State *lua) {
    kcp_ctx *kcp = luaL_checkudata(lua, 1, MT_KCP);
    LPUB_CUR_TASK(lua, task);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 2);
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = lpub_check_u16(lua, 4, PORT_OUT_OF_RANGE);
    luaL_argcheck(lua, lua_isnoneornil(lua, 5) || lua_istable(lua, 5), 5, "config must be a table");
    kcp_config cfg;
    kcp_config *pcfg = NULL;
    if (lua_istable(lua, 5)) {
        cfg.nodelay = _lkcp_cfgint(lua, 5, "nodelay", -1, -1, 1);
        cfg.interval = _lkcp_cfgint(lua, 5, "interval", -1, -1, 5000);
        cfg.resend = _lkcp_cfgint(lua, 5, "resend", -1, -1, INT32_MAX);
        cfg.nc = _lkcp_cfgint(lua, 5, "nc", -1, -1, 1);
        cfg.sndwnd = _lkcp_cfgint(lua, 5, "sndwnd", 0, 0, UINT16_MAX);
        cfg.rcvwnd = _lkcp_cfgint(lua, 5, "rcvwnd", 0, 0, UINT16_MAX);
        cfg.mtu = _lkcp_cfgint(lua, 5, "mtu", 0, 0, UINT16_MAX);
        pcfg = &cfg;
    }
    return lpub_rtn_bool(lua, ERR_OK == kcp_start(kcp, task->handle, sess, ip, port, pcfg));
}
/// <summary>
/// 变更会话数据推送目标 task
/// </summary>
/// <param name="self" type="userdata">kcp 会话句柄</param>
/// <param name="handle" type="string|integer">目标 task：字符串按名字查，整数按句柄直取</param>
/// <returns type="boolean">成功 true；目标不存在（名字未注册 / 数字句柄对应 task 已退出）或会话已 stop 时 false。
/// 仅保证调用时目标存在：目标若在此之后退出，该会话的消息会被静默丢弃。探测口径同 core.bind_task</returns>
static int32_t _lkcp_handle(lua_State *lua) {
    kcp_ctx *kcp = luaL_checkudata(lua, 1, MT_KCP);
    name_t handle = lpub_task_handle(lua, 2);
    task_ctx *dst = task_grab(g_loader, handle);
    if (NULL == dst) {
        return lpub_rtn_bool(lua, 0);
    }
    task_ungrab(dst);
    return lpub_rtn_bool(lua, ERR_OK == kcp_handle(kcp, handle));
}
/// <summary>
/// 发送数据:交 KCP 可靠传输,实际发包由 event 线程 tick 周期驱动
/// </summary>
/// <param name="self" type="userdata">kcp 会话句柄</param>
/// <param name="data" type="string|lightuserdata">数据;字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填</param>
/// <param name="copy" type="integer?">是否复制数据,只收 0/1,默认 1(复制)</param>
/// <returns type="boolean">成功 true,失败 false</returns>
static int32_t _lkcp_send(lua_State *lua) {
    kcp_ctx *kcp = luaL_checkudata(lua, 1, MT_KCP);
    size_t size;
    int32_t copy;
    void *data = lpub_check_buf(lua, 2, &size, &copy);
    return lpub_rtn_bool(lua, ERR_OK == kcp_send(kcp, data, size, copy));
}
LUAMOD_API int luaopen_kcp(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lkcp_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "start", _lkcp_start },
        { "handle", _lkcp_handle },
        { "send", _lkcp_send },
        { "stop", _lkcp_stop },
        { "__gc", _lkcp_stop },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_KCP, reg_new, reg_func);
    return 1;
}
