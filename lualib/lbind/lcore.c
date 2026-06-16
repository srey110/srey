#include "lbind/lpub.h"

// 辅助宏：若栈上指定位置为整数则取其值，否则默认为 1（复制语义）
#define COPY_TYPE(lua, idx) (lua_isinteger(lua, idx) ? (int32_t)luaL_checkinteger(lua, idx) : 1)

typedef struct _task_list_arg {
    int32_t n;
    lua_State *lua;
}_task_list_arg;

/// <summary>
/// 向当前 task 注册一个一次性超时事件
/// </summary>
/// <param name="sess" type="integer">会话 id，超时消息回调时回带</param>
/// <param name="time" type="integer">延迟毫秒数</param>
/// <returns>无</returns>
static int32_t _lcore_timeout(lua_State *lua) {
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 1);
    uint32_t time = (uint32_t)luaL_checkinteger(lua, 2);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    task_timeout(task, sess, time, NULL);
    return 0;
}
/// <summary>
/// 向目标 task 发送单向调用消息（无响应）
/// </summary>
/// <param name="dst" type="lightuserdata">目标 task 指针</param>
/// <param name="reqtype" type="integer">业务请求类型</param>
/// <param name="data" type="string|lightuserdata">消息内容；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，默认 1（复制）</param>
/// <returns>无</returns>
static int32_t _lcore_call(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    task_ctx *dst = (task_ctx *)lua_touserdata(lua, 1);
    subtype_t reqtype = (subtype_t)luaL_checkinteger(lua, 2);
    void *data;
    size_t size;
    int32_t copy;
    data = lpub_check_buf(lua, 3, &size, &copy);
    task_call(dst, reqtype, data, size, copy);
    return 0;
}
// 解析 dsts table(栈位置 idx)：MALLOC task_ctx*[n] 数组并填充,n=0 时返回 NULL。
// 调用方负责 FREE 返回值。table 类型校验由 luaL_checktype 负责出错时 longjmp。
static task_ctx **_parse_multi_dsts(lua_State *lua, int32_t idx, int32_t *n_out) {
    luaL_checktype(lua, idx, LUA_TTABLE);
    lua_Integer n = luaL_len(lua, idx);
    *n_out = (int32_t)n;
    if (n <= 0) {
        return NULL;
    }
    task_ctx **dsts;
    MALLOC(dsts, sizeof(task_ctx *) * (size_t)n);
    lua_Integer i;
    for (i = 0; i < n; i++) {
        lua_rawgeti(lua, idx, i + 1);
        dsts[i] = (task_ctx *)lua_touserdata(lua, -1);
        lua_pop(lua, 1);
    }
    return dsts;
}
/// <summary>
/// 广播请求：把同一份 data 投递给多个 task,各 dst 在 _request 回调中独立 task_response 回 src(共用 sess)。
/// 当前 task 作 src,sess 由调用方传入(非 0)；src 端 srey.on_responsed 会被回调 N 次,业务自行据 sess 识别与累计。
/// </summary>
/// <param name="dsts" type="lightuserdata[]">目标 task 指针数组(Lua table)；nil 元素被跳过</param>
/// <param name="reqtype" type="integer">业务请求类型</param>
/// <param name="sess" type="integer">会话 id(非 0),N 个 dst 共用此 sess</param>
/// <param name="data" type="string|lightuserdata">数据；string 时长度自动取,lightuserdata 必须传 size</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填</param>
/// <param name="copy" type="integer?">是否复制数据,默认 1（复制）;0 时直接转移所有权</param>
/// <returns type="integer">实际成功投递的 dst 数（非 NULL 元素个数,0 表示全部跳过未投递）</returns>
static int32_t _lcore_multi_request(lua_State *lua) {
    task_ctx *src = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == src) {
        return luaL_error(lua, "task is nil");
    }
    subtype_t reqtype = (subtype_t)luaL_checkinteger(lua, 2);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    void *data;
    size_t size;
    int32_t copy;
    data = lpub_check_buf(lua, 4, &size, &copy);
    int32_t n;
    task_ctx **dsts = _parse_multi_dsts(lua, 1, &n);
    // n<=0 / sess=0 / 类型错等异常退出由 Lua wrapper(srey.multi_request) 提前 utils.ud_free 兜底,
    // C 端只走正常路径,不再做 copy=0 + lightuserdata 的 FREE 兜底
    if (n <= 0) {
        lua_pushinteger(lua, 0);
        return 1;
    }
    int32_t valid = task_multi_request(dsts, n, src, reqtype, sess, data, size, copy);
    FREE(dsts);
    lua_pushinteger(lua, valid);
    return 1;
}
/// <summary>
/// 单向广播：把同一份 data 投递给多个 task（N 个 message 共享同一份 data，引用计数自动释放）
/// </summary>
/// <param name="dsts" type="lightuserdata[]">目标 task 指针数组(Lua table)；nil 元素被跳过</param>
/// <param name="reqtype" type="integer">业务请求类型</param>
/// <param name="data" type="string|lightuserdata">数据；string 时长度自动取,lightuserdata 必须传 size</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填</param>
/// <param name="copy" type="integer?">是否复制数据,默认 1（复制）;0 时直接转移所有权</param>
/// <returns>无</returns>
static int32_t _lcore_multi_call(lua_State *lua) {
    subtype_t reqtype = (subtype_t)luaL_checkinteger(lua, 2);
    void *data;
    size_t size;
    int32_t copy;
    data = lpub_check_buf(lua, 3, &size, &copy);
    int32_t n;
    task_ctx **dsts = _parse_multi_dsts(lua, 1, &n);
    // 空表 / 类型错等异常退出由 Lua wrapper(srey.multi_call) 提前 utils.ud_free 兜底,
    // C 端只走正常路径,不再做 copy=0 + lightuserdata 的 FREE 兜底
    if (n <= 0) {
        return 0;
    }
    task_multi_call(dsts, n, reqtype, data, size, copy);
    FREE(dsts);
    return 0;
}
/// <summary>
/// 向目标 task 发送请求消息，携带会话 id 以便对方响应
/// </summary>
/// <param name="dst" type="lightuserdata">目标 task 指针</param>
/// <param name="reqtype" type="integer">业务请求类型</param>
/// <param name="sess" type="integer">会话 id，响应回带</param>
/// <param name="data" type="string|lightuserdata">消息内容；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，默认 1（复制）</param>
/// <returns>无</returns>
static int32_t _lcore_request(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    task_ctx *dst = (task_ctx *)lua_touserdata(lua, 1);
    subtype_t reqtype = (subtype_t)luaL_checkinteger(lua, 2);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    void *data;
    size_t size;
    int32_t copy;
    task_ctx *src = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == src) {
        return luaL_error(lua, "task is nil");
    }
    data = lpub_check_buf(lua, 4, &size, &copy);
    task_request(dst, src, reqtype, sess, data, size, copy);
    return 0;
}
/// <summary>
/// 向请求方 task 回复响应消息，携带错误码及可选数据
/// </summary>
/// <param name="dst" type="lightuserdata">请求方 task 指针</param>
/// <param name="reqtype" type="integer">请求类型 request_type</param>
/// <param name="sess" type="integer">原请求会话 id</param>
/// <param name="erro" type="integer">错误码，0 表示成功</param>
/// <param name="data" type="string|lightuserdata|nil">响应数据；nil 表示无数据</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，默认 1（复制）</param>
/// <returns>无</returns>
static int32_t _lcore_response(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    task_ctx *dst = (task_ctx *)lua_touserdata(lua, 1);
    subtype_t reqtype = (subtype_t)luaL_checkinteger(lua, 2);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    int32_t erro = (int32_t)luaL_checkinteger(lua, 4);
    void *data;
    size_t size;
    int32_t copy;
    int32_t type = lua_type(lua, 5);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        data = NULL;
        size = 0;
        copy = 1;
    } else if (LUA_TSTRING == type) {
        data = (void *)luaL_checklstring(lua, 5, &size);
        copy = 1;
    } else if (LUA_TLIGHTUSERDATA == type) {
        data = lua_touserdata(lua, 5);
        size = (size_t)luaL_checkinteger(lua, 6);
        copy = COPY_TYPE(lua, 7);
    } else {
        return luaL_argerror(lua, 5, "nil, string or light userdata expected");
    }
    task_response(dst, reqtype, sess, erro, data, size, copy);
    return 0;
}
/// <summary>
/// 在当前 task 上监听 TCP/UDP 端口
/// </summary>
/// <param name="pktype" type="integer">封包协议类型，参考 PACK_TYPE</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="ip" type="string">监听 IP，如 "0.0.0.0"</param>
/// <param name="port" type="integer">监听端口</param>
/// <param name="netev" type="integer?">事件订阅掩码，默认 NETEV_NONE</param>
/// <returns type="integer">监听 id，失败返回 -1</returns>
static int32_t _lcore_listen(lua_State *lua) {
    pack_type pktype = (pack_type)luaL_checkinteger(lua, 1);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 2)) {
        LUACHECK_LUDATA(lua, 2);
        evssl = lua_touserdata(lua, 2);
    }
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 4);
    int32_t netev = lua_isinteger(lua, 5) ? (int32_t)luaL_checkinteger(lua, 5) : NETEV_NONE;
    uint64_t id;
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    if (ERR_OK != task_listen(task, pktype, evssl, ip, port, &id, netev)) {
        lua_pushinteger(lua, -1);
    } else {
        lua_pushinteger(lua, id);
    }
    return 1;
}
/// <summary>
/// 取消指定监听 id 的监听
/// </summary>
/// <param name="id" type="integer">listen 返回的监听 id</param>
/// <returns>无</returns>
static int32_t _lcore_unlisten(lua_State *lua) {
    uint64_t id = (uint64_t)luaL_checkinteger(lua, 1);
    ev_unlisten(&g_loader->netev, id);
    return 0;
}
/// <summary>
/// 发起 TCP 连接（异步）
/// </summary>
/// <param name="pktype" type="integer">封包协议类型，参考 PACK_TYPE</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="ip" type="string">对端 IP</param>
/// <param name="port" type="integer">对端端口</param>
/// <param name="netev" type="integer?">事件订阅掩码，默认 NETEV_NONE</param>
/// <param name="extra" type="userdata?">协议握手所需附加上下文，所有权移交框架</param>
/// <returns type="integer">socket fd；失败返回 INVALID_SOCK</returns>
/// <returns type="integer?">skid（连接唯一序号）；仅在 fd 有效时有效</returns>
static int32_t _lcore_connect(lua_State *lua) {
    pack_type pktype = (pack_type)luaL_checkinteger(lua, 1);
    struct evssl_ctx *evssl = NULL;
    if (LUA_TNIL != lua_type(lua, 2)) {
        LUACHECK_LUDATA(lua, 2);
        evssl = lua_touserdata(lua, 2);
    }
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 4);
    int32_t netev = lua_isinteger(lua, 5) ? (int32_t)luaL_checkinteger(lua, 5) : NETEV_NONE;
    void *extra = lua_isuserdata(lua, 6) ? lua_touserdata(lua, 6) : NULL;
    SOCKET fd;
    uint64_t skid;
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, extra, &fd, &skid)) {
        lua_pushinteger(lua, INVALID_SOCK);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, skid);
    return 2;
}
/// <summary>
/// 对已有明文连接发起 SSL 升级握手
/// </summary>
/// <param name="fd" type="integer">socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="client" type="integer">1 表示客户端握手，0 表示服务端</param>
/// <param name="evssl" type="lightuserdata">SSL 上下文；为 nil 或非 userdata 时直接失败</param>
/// <returns type="boolean">成功 true，失败 false</returns>
static int32_t _lcore_ssl_exchange(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t client = (int32_t)luaL_checkinteger(lua, 3);
    if (!lua_islightuserdata(lua, 4)) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    struct evssl_ctx *evssl = lua_touserdata(lua, 4);
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    if (ERR_OK == ev_ssl(&task->loader->netev, fd, skid, client, evssl)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 创建 UDP 套接字并绑定地址
/// </summary>
/// <param name="ip" type="string">绑定 IP</param>
/// <param name="port" type="integer">绑定端口</param>
/// <returns type="integer">socket fd；失败返回 INVALID_SOCK</returns>
/// <returns type="integer?">skid；仅在 fd 有效时有效</returns>
static int32_t _lcore_udp(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 2);
    SOCKET fd;
    uint64_t skid;
    task_ctx *task = global_userdata(lua, CUR_TASK_NAME);
    if (NULL == task) {
        return luaL_error(lua, "task is nil");
    }
    if (ERR_OK != task_udp(task, ip, port, &fd, &skid)) {
        lua_pushinteger(lua, INVALID_SOCK);
        return 1;
    }
    lua_pushinteger(lua, fd);
    lua_pushinteger(lua, skid);
    return 2;
}
/// <summary>
/// 向指定 fd/skid 发送 TCP 数据
/// </summary>
/// <param name="fd" type="integer">socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，默认 1（复制）</param>
/// <returns type="boolean">成功 true，失败 false</returns>
static int32_t _lcore_send(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    void *data;
    size_t size;
    int32_t copy;
    data = lpub_check_buf(lua, 3, &size, &copy);
    if (ERR_OK == ev_send(&g_loader->netev, fd, skid, data, size, copy)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 多播 TCP 数据：把同一份 data 零拷贝广播给多个 fd（N 个 buf 共享，引用计数自动释放）
/// </summary>
/// <param name="fds" type="integer[]">socket fd 数组(Lua table)</param>
/// <param name="skids" type="integer[]">连接 skid 数组,与 fds 等长一一配对</param>
/// <param name="data" type="string|lightuserdata">数据；string 时长度自动取,lightuserdata 必须传 size</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填</param>
/// <param name="copy" type="integer?">是否复制数据,默认 1（复制）;0 时直接转移所有权</param>
/// <returns type="boolean">至少 1 个 fd 投递成功 true,全部无效 false</returns>
static int32_t _lcore_send_multi(lua_State *lua) {
    luaL_checktype(lua, 1, LUA_TTABLE);
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_Integer n_fds = luaL_len(lua, 1);
    lua_Integer n_skids = luaL_len(lua, 2);
    if (n_fds != n_skids) {
        return luaL_error(lua, "fds and skids length mismatch (%d vs %d)",
                          (int)n_fds, (int)n_skids);
    }
    if (n_fds <= 0) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    void *data;
    size_t size;
    int32_t copy;
    data = lpub_check_buf(lua, 3, &size, &copy);
    SOCKET *fds;
    uint64_t *skids;
    MALLOC(fds, sizeof(SOCKET) * (size_t)n_fds);
    MALLOC(skids, sizeof(uint64_t) * (size_t)n_fds);
    lua_Integer i;
    for (i = 0; i < n_fds; i++) {
        lua_rawgeti(lua, 1, i + 1);
        fds[i] = (SOCKET)lua_tointeger(lua, -1);
        lua_pop(lua, 1);
        lua_rawgeti(lua, 2, i + 1);
        skids[i] = (uint64_t)lua_tointeger(lua, -1);
        lua_pop(lua, 1);
    }
    int32_t r = ev_send_multi(&g_loader->netev, fds, skids, (int32_t)n_fds,
                              data, size, copy);
    FREE(fds);
    FREE(skids);
    lua_pushboolean(lua, ERR_OK == r ? 1 : 0);
    return 1;
}
/// <summary>
/// 向指定 ip:port 发送 UDP 数据
/// </summary>
/// <param name="fd" type="integer">UDP socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="ip" type="string">目标 IP</param>
/// <param name="port" type="integer">目标端口</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，默认 1（复制）</param>
/// <returns type="boolean">成功 true，失败 false</returns>
static int32_t _lcore_sendto(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = (uint16_t)luaL_checkinteger(lua, 4);
    void *data;
    size_t size;
    int32_t copy;
    data = lpub_check_buf(lua, 5, &size, &copy);
    if (ERR_OK == ev_sendto(&g_loader->netev, fd, skid, ip, port, data, size, copy)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// UDP socket 加入多播组(IPv4/IPv6 自动按 socket family 分支)
/// </summary>
/// <param name="fd" type="integer">UDP socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="group_ip" type="string">多播组地址(IPv4 224.0.0.0/4 段 / IPv6 ff00::/8 段)</param>
/// <param name="iface_str" type="string?">网卡 IP(IPv4) / 接口名(IPv6),nil 走系统默认</param>
/// <returns type="boolean">成功 true</returns>
static int32_t _lcore_udp_join(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    const char *group_ip = luaL_checkstring(lua, 3);
    const char *iface_str = (LUA_TSTRING == lua_type(lua, 4)) ? luaL_checkstring(lua, 4) : NULL;
    lua_pushboolean(lua, ERR_OK == ev_udp_join(&g_loader->netev, fd, skid, group_ip, iface_str) ? 1 : 0);
    return 1;
}
/// <summary>
/// UDP socket 离开多播组,参数同 udp_join
/// </summary>
/// <param name="fd" type="integer">UDP socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="group_ip" type="string">多播组地址(IPv4 224.0.0.0/4 段 / IPv6 ff00::/8 段)</param>
/// <param name="iface_str" type="string?">网卡 IP(IPv4) / 接口名(IPv6),nil 走系统默认</param>
/// <returns type="boolean">成功 true</returns>
static int32_t _lcore_udp_leave(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    const char *group_ip = luaL_checkstring(lua, 3);
    const char *iface_str = (LUA_TSTRING == lua_type(lua, 4)) ? luaL_checkstring(lua, 4) : NULL;
    lua_pushboolean(lua, ERR_OK == ev_udp_leave(&g_loader->netev, fd, skid, group_ip, iface_str) ? 1 : 0);
    return 1;
}
/// <summary>
/// 设置 UDP 多播 TTL(IPv4) / Hop Limit(IPv6)
/// </summary>
/// <param name="fd" type="integer">UDP socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="ttl" type="integer">1-255,默认 1 仅本网段</param>
/// <returns type="boolean">成功 true</returns>
static int32_t _lcore_udp_ttl(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    uint8_t ttl = (uint8_t)luaL_checkinteger(lua, 3);
    lua_pushboolean(lua, ERR_OK == ev_udp_ttl(&g_loader->netev, fd, skid, ttl) ? 1 : 0);
    return 1;
}
/// <summary>
/// 设置 UDP 多播本机回环,默认 1(发出去自己也能收到),0=不收
/// </summary>
/// <param name="fd" type="integer">UDP socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="enable" type="integer">1=回环(默认,发出自收),0=不收</param>
/// <returns type="boolean">成功 true</returns>
static int32_t _lcore_udp_loop(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t enable = (int32_t)luaL_checkinteger(lua, 3);
    lua_pushboolean(lua, ERR_OK == ev_udp_loop(&g_loader->netev, fd, skid, enable) ? 1 : 0);
    return 1;
}
/// <summary>
/// 主动关闭指定 fd/skid 的网络连接
/// </summary>
/// <param name="fd" type="integer">socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="immed" type="integer">0=优雅关闭(等 send queue 发完);1=立即关闭(丢弃未发数据);默认 0</param>
/// <returns>无</returns>
static int32_t _lcore_close(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int32_t immed = (int32_t)luaL_optinteger(lua, 3, 0);
    ev_close(&g_loader->netev, fd, skid, immed);
    return 0;
}
/// <summary>
/// 动态修改指定连接的封包协议类型
/// </summary>
/// <param name="fd" type="integer">socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="pktype" type="integer">新封包协议类型，参考 PACK_TYPE</param>
/// <returns>无</returns>
static int32_t _lcore_pack_type(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    subtype_t pktype = (subtype_t)luaL_checkinteger(lua, 3);
    ev_ud_pktype(&g_loader->netev, fd, skid, pktype);
    return 0;
}
/// <summary>
/// 设置指定连接的用户自定义状态值
/// </summary>
/// <param name="fd" type="integer">socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="status" type="integer">用户自定义状态值（int8）</param>
/// <returns>无</returns>
static int32_t _lcore_status(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    int8_t status = (int8_t)luaL_checkinteger(lua, 3);
    ev_ud_status(&g_loader->netev, fd, skid, status);
    return 0;
}
/// <summary>
/// 将指定连接绑定到目标 task（后续网络消息投递到该 task）
/// </summary>
/// <param name="fd" type="integer">socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="name" type="string|integer">目标字符串名或数字句柄</param>
/// <returns>无</returns>
static int32_t _lcore_bind_task(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    name_t handle = (LUA_TSTRING == lua_type(lua, 3))
        ? task_find_name(g_loader, lua_tostring(lua, 3))
        : (name_t)luaL_checkinteger(lua, 3);
    ev_ud_handle(&g_loader->netev, fd, skid, handle);
    return 0;
}
/// <summary>
/// 为指定连接设置会话 id，用于关联请求与响应
/// </summary>
/// <param name="fd" type="integer">socket fd</param>
/// <param name="skid" type="integer">连接 skid</param>
/// <param name="sess" type="integer">会话 id</param>
/// <returns>无</returns>
static int32_t _lcore_session(lua_State *lua) {
    SOCKET fd = (SOCKET)luaL_checkinteger(lua, 1);
    uint64_t skid = (uint64_t)luaL_checkinteger(lua, 2);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    ev_ud_sess(&g_loader->netev, fd, skid, sess);
    return 0;
}
/// <summary>
/// 询问协议层指定封包是否允许恢复（分片重组判断）
/// </summary>
/// <param name="pktype" type="integer">封包协议类型，参考 PACK_TYPE</param>
/// <param name="data" type="lightuserdata">协议层封包指针</param>
/// <returns type="boolean">允许恢复 true，否则 false</returns>
static int32_t _lcore_may_resume(lua_State *lua) {
    pack_type pktype = (pack_type)luaL_checkinteger(lua, 1);
    LUACHECK_LUDATA(lua, 2);
    void *data = lua_touserdata(lua, 2);
    if (ERR_OK == prots_may_resume(pktype, data)) {
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
// task_list 收集回调：把 {name=名, handle=句柄} 追加进栈顶数组（匿名 task 省略 name 字段）
static void _lcore_task_list_collect(const char *name, name_t handle, void *arg) {
    _task_list_arg *c = (_task_list_arg *)arg;
    c->n++;
    lua_newtable(c->lua);
    if (NULL != name) {
        lua_pushstring(c->lua, name);
        lua_setfield(c->lua, -2, "name");
    }
    lua_pushinteger(c->lua, (lua_Integer)handle);
    lua_setfield(c->lua, -2, "handle");
    lua_rawseti(c->lua, -2, c->n);
}
/// <summary>
/// 枚举当前 loader 已注册的所有 task（C 层列表）
/// </summary>
/// <returns type="table">{name,handle} 对象数组（name 字符串、handle 整数；匿名 task 无 name 字段）；无 task 时空表</returns>
static int32_t _lcore_task_list(lua_State *lua) {
    lua_newtable(lua);
    _task_list_arg c;
    c.n = 0;
    c.lua = lua;
    loader_task_each(g_loader, _lcore_task_list_collect, &c);
    return 1;
}
/// <summary>
/// 获取全局内存分配/释放统计（MEMORY_CHECK 关闭时全为 0）
/// </summary>
/// <returns type="table">{nalloc=累计分配次数, nfree=累计释放次数, live=当前活跃分配数}</returns>
static int32_t _lcore_mem_stat(lua_State *lua) {
    uint64_t nalloc = 0, nfree = 0;
    mem_stat(&nalloc, &nfree);
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)nalloc);
    lua_setfield(lua, -2, "nalloc");
    lua_pushinteger(lua, (lua_Integer)nfree);
    lua_setfield(lua, -2, "nfree");
    lua_pushinteger(lua, (lua_Integer)(nalloc - nfree));
    lua_setfield(lua, -2, "live");
    return 1;
}
/// <summary>
/// 加载 PEM/DER 格式的 CA、证书和私钥，按 name 注册 SSL 上下文
/// </summary>
/// <param name="name" type="string">SSL 上下文注册名（字符串 key）</param>
/// <param name="ca" type="string">CA 文件名（相对 cert 目录）；空串表示不加载</param>
/// <param name="cert" type="string">证书文件名（相对 cert 目录）；空串表示不加载</param>
/// <param name="key" type="string">私钥文件名（相对 cert 目录）；空串表示不加载</param>
/// <param name="keytype" type="integer?">密钥格式，默认 SSL_FILETYPE_PEM</param>
/// <returns type="lightuserdata?">SSL 上下文指针；失败或未启用 SSL 时返回 nil</returns>
static int32_t _lcore_cert_register(lua_State *lua) {
#if WITH_SSL
    const char *name = luaL_checkstring(lua, 1);
    const char *ca = luaL_checkstring(lua, 2);
    const char *cert = luaL_checkstring(lua, 3);
    const char *key = luaL_checkstring(lua, 4);
    int32_t keytype;
    int32_t type = (int32_t)lua_type(lua, 5);
    if (LUA_TNUMBER == type) {
        keytype = (int32_t)luaL_checkinteger(lua, 5);
    } else {
        keytype = SSL_FILETYPE_PEM; // 默认使用 PEM 格式
    }
    char capath[PATH_LENS]   = { 0 };
    char certpath[PATH_LENS] = { 0 };
    char keypath[PATH_LENS]  = { 0 };
    char propath[PATH_LENS]  = { 0 };
    if (ERR_OK != global_string(lua, PATH_NAME, propath, sizeof(propath))) {
        lua_pushnil(lua);
        return 1;
    }
    if (0 != strlen(ca)) {
        SNPRINTF(capath, sizeof(capath), "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, ca);
    }
    if (0 != strlen(cert)) {
        SNPRINTF(certpath, sizeof(certpath), "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, cert);
    }
    if (0 != strlen(key)) {
        SNPRINTF(keypath, sizeof(keypath), "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, key);
    }
    evssl_ctx *ssl = evssl_new(capath, certpath, keypath, keytype);
    if (NULL != ssl) {
        if (ERR_OK != evssl_register(name, ssl)) {
            lua_pushnil(lua);
        } else {
            lua_pushlightuserdata(lua, ssl);
        }
    } else {
        lua_pushnil(lua);
    }
#else
    lua_pushnil(lua);
#endif
    return 1;
}
/// <summary>
/// 加载 PKCS12 格式证书文件，按 name 注册 SSL 上下文
/// </summary>
/// <param name="name" type="string">SSL 上下文注册名（字符串 key）</param>
/// <param name="p12" type="string">PKCS12 文件名（相对 cert 目录）</param>
/// <param name="pwd" type="string">PKCS12 文件密码</param>
/// <returns type="lightuserdata?">SSL 上下文指针；失败或未启用 SSL 时返回 nil</returns>
static int32_t _lcore_p12_register(lua_State *lua) {
#if WITH_SSL
    const char *name = luaL_checkstring(lua, 1);
    const char *p12 = luaL_checkstring(lua, 2);
    const char *pwd = luaL_checkstring(lua, 3);
    char p12path[PATH_LENS] = { 0 };
    if (0 != strlen(p12)) {
        char propath[PATH_LENS] = { 0 };
        if (ERR_OK != global_string(lua, PATH_NAME, propath, sizeof(propath))) {
            lua_pushnil(lua);
            return 1;
        }
        SNPRINTF(p12path, sizeof(p12path), "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, p12);
    }
    evssl_ctx *ssl = evssl_p12_new(p12path, pwd);
    if (NULL != ssl) {
        if (ERR_OK != evssl_register(name, ssl)) {
            lua_pushnil(lua);
        } else {
            lua_pushlightuserdata(lua, ssl);
        }
    } else {
        lua_pushnil(lua);
    }
#else
    lua_pushnil(lua);
#endif
    return 1;
}
/// <summary>
/// 按 name 查询已注册的 SSL 上下文
/// </summary>
/// <param name="name" type="string">SSL 上下文注册名</param>
/// <returns type="lightuserdata?">SSL 上下文指针；未找到或未启用 SSL 时返回 nil</returns>
static int32_t _lcore_ssl_qury(lua_State *lua) {
#if WITH_SSL
    const char *name = luaL_checkstring(lua, 1);
    struct evssl_ctx *ssl = evssl_qury(name);
    if (NULL != ssl) {
        lua_pushlightuserdata(lua, ssl);
    } else {
        lua_pushnil(lua);
    }
#else
    lua_pushnil(lua);
#endif
    return 1;
}
/// <summary>
/// 设置 SSL 安全级别
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="level" type="integer">安全级别 0-5</param>
static int32_t _lcore_ssl_seclevel(lua_State *lua) {
#if WITH_SSL
    LUACHECK_LUDATA(lua, 1);
    struct evssl_ctx *ssl = lua_touserdata(lua, 1);
    int32_t level = (int32_t)luaL_checkinteger(lua, 2);
    evssl_seclevel(ssl, level);
#endif
    return 0;
}
/// <summary>
/// 设置最低 TLS 协议版本
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="version" type="TLS_VERSION">
///        协议版本：
///            TLS1_VERSION(0x0301)
///            TLS1_1_VERSION(0x0302)
///            TLS1_2_VERSION(0x0303)
///            TLS1_3_VERSION(0x0304)
/// </param>
static int32_t _lcore_ssl_min_proto(lua_State *lua) {
#if WITH_SSL
    LUACHECK_LUDATA(lua, 1);
    struct evssl_ctx *ssl = lua_touserdata(lua, 1);
    int32_t version = (int32_t)luaL_checkinteger(lua, 2);
    evssl_min_proto(ssl, version);
#endif
    return 0;
}
/// <summary>
/// 设置 SSL 上下文是否验证对端证书
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="verify" type="integer">1 启用验证，0 不验证</param>
static int32_t _lcore_ssl_verify(lua_State *lua) {
#if WITH_SSL
    LUACHECK_LUDATA(lua, 1);
    struct evssl_ctx *ssl = lua_touserdata(lua, 1);
    int32_t verify = (int32_t)luaL_checkinteger(lua, 2);
    evssl_verify(ssl, verify);
#endif
    return 0;
}
//srey.core
LUAMOD_API int luaopen_core(lua_State *lua) {
    luaL_Reg reg[] = {
        { "timeout", _lcore_timeout },
        { "call", _lcore_call },
        { "multi_call", _lcore_multi_call },
        { "multi_request", _lcore_multi_request },
        { "request", _lcore_request },
        { "response", _lcore_response },
        { "listen", _lcore_listen },
        { "unlisten", _lcore_unlisten },
        { "connect", _lcore_connect },
        { "ssl_exchange", _lcore_ssl_exchange },
        { "udp", _lcore_udp },

        { "send", _lcore_send },
        { "send_multi", _lcore_send_multi },
        { "sendto", _lcore_sendto },
        { "udp_join", _lcore_udp_join },
        { "udp_leave", _lcore_udp_leave },
        { "udp_ttl", _lcore_udp_ttl },
        { "udp_loop", _lcore_udp_loop },
        { "close", _lcore_close },

        { "pack_type", _lcore_pack_type },
        { "status", _lcore_status },
        { "bind_task", _lcore_bind_task },
        { "session", _lcore_session },

        { "may_resume", _lcore_may_resume },

        { "task_list", _lcore_task_list },
        { "mem_stat", _lcore_mem_stat },

        { "cert_register", _lcore_cert_register },
        { "p12_register", _lcore_p12_register },
        { "ssl_qury", _lcore_ssl_qury },
        { "ssl_verify", _lcore_ssl_verify },
        { "ssl_seclevel", _lcore_ssl_seclevel },
        { "ssl_min_proto", _lcore_ssl_min_proto },

        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
