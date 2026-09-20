#include "lbind/lpub.h"

#define CERT_FOLDER "keys" // SSL 证书文件所在子目录名
#define MTYPE_OUT_OF_RANGE "message type out of range"
#define SECLEVEL_OUT_OF_RANGE "ssl security level out of range"
#define TLSVER_OUT_OF_RANGE "tls version out of range"

// multi_request / multi_call 投递一次所需的全部东西。两个 int32 挨着放在 8 字节字段之前,不留 padding
typedef struct {
    int32_t    count; // grab 成功数;0 表示无处可投
    int32_t    copy;  // 载荷 copy 语义,透传给 task_multi_*
    size_t     size;  // 载荷字节数
    task_ctx **dsts;  // grab 到的目标数组
    void      *data;  // 载荷,可为 NULL
}_multi_args;
typedef struct _task_entry {
    char  *name; // strdup 的任务名，匿名 task 为 NULL；押进 Lua 表后即 FREE
    name_t handle;
}_task_entry;


/// <summary>
/// 向当前 task 注册一个一次性超时事件
/// </summary>
/// <param name="sess" type="integer">会话 id(非 0)，超时消息回调时回带</param>
/// <param name="time" type="integer">延迟毫秒数；非正数即刻触发，超 UINT32_MAX(约 49.7 天)钳到该上界</param>
/// <returns>无</returns>
static int32_t _lcore_timeout(lua_State *lua) {
    uint64_t sess = lpub_check_sess(lua, 1);
    lua_Integer ms = luaL_checkinteger(lua, 2);
    // 校验放 C 层而非 Lua wrapper：core.timeout 直接注册在 core 表上，绕过 lib.srey 的脚本同受保护。
    // 不可原样 (uint32_t) 截断——2^32 会截成 0 被 tw_add 当场回调，2^32+1000 则变 1 秒
    uint32_t time;
    if (ms <= 0) {
        time = 0;
    } else if (ms > (lua_Integer)UINT32_MAX) {
        time = UINT32_MAX;
    } else {
        time = (uint32_t)ms;
    }
    LPUB_CUR_TASK(lua, task);
    task_timeout(task, sess, time, NULL);
    return 0;
}
// data 参数可选:nil/none 视为无载荷(NULL,0,copy=1);否则同 lpub_check_buf(string/lightuserdata,非法类型 argerror)
static void *_lcore_opt_buf(lua_State *lua, int32_t idx, size_t *size, int32_t *copy) {
    int32_t type = lua_type(lua, idx);
    if (LUA_TNIL == type
        || LUA_TNONE == type) {
        *size = 0;
        *copy = 1;
        return NULL;
    }
    return lpub_check_buf(lua, idx, size, copy);
}
/// <summary>
/// 向目标 task 发送单向调用消息（无响应）
/// </summary>
/// <param name="dst" type="string|integer">目标 task 名(字符串)或句柄(整数)</param>
/// <param name="reqtype" type="integer">业务请求类型，取值 [0, UINT16_MAX]，越界报错</param>
/// <param name="data" type="string|lightuserdata|nil">消息内容(nil 表示无载荷)；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，只收 0/1，默认 1（复制）</param>
/// <returns type="boolean">grab 到目标并投递 true；目标不存在 false</returns>
static int32_t _lcore_call(lua_State *lua) {
    name_t handle = lpub_task_handle(lua, 1);
    subtype_t reqtype = lpub_check_u16(lua, 2, REQTYPE_OUT_OF_RANGE);
    void *data;
    size_t size;
    int32_t copy;
    data = _lcore_opt_buf(lua, 3, &size, &copy);
    task_ctx *dst = task_grab(g_loader, handle);
    if (NULL == dst) {
        CHECK_COPY_FREE(data, copy);
        return lpub_rtn_bool(lua, 0);
    }
    task_call(dst, reqtype, data, size, copy);
    task_ungrab(dst);
    return lpub_rtn_bool(lua, 1);
}
// 校验 dsts table 类型 + 逐元素类型(string/integer 名或 nil)。成功返回长度(>=0);
// dsts 非 table 或含非法元素返回 -1。内部的 luaL_len 会走 __len 元方法、也会对非整数结果自行抛错,
// 所以调用方必须在接管 copy=0 缓冲之前调它——抛出时那块内存还没有主人。
static int32_t _check_multi_names(lua_State *lua, int32_t idx) {
    if (LUA_TTABLE != lua_type(lua, idx)) {
        return -1;
    }
    int32_t n = (int32_t)luaL_len(lua, idx);
    int32_t vtype;
    int32_t isint;
    for (int32_t i = 0; i < n; i++) {
        lua_rawgeti(lua, idx, i + 1);
        vtype = lua_type(lua, -1);
        isint = lua_isinteger(lua, -1);
        lua_pop(lua, 1);
        if (LUA_TNIL != vtype && LUA_TSTRING != vtype && !isint) {
            return -1;
        }
    }
    return n;
}
// 按 _check_multi_names 已校验的长度 n(>0) 从 dsts table(栈位置 idx)逐元素 grab,填充 task_ctx*[n],
// *cnt 出参为实际 grab 成功数(跳过 nil/NONE/不存在)。调用方 FREE 返回值并对前 *cnt 个 ungrab。
// 元素类型已校验,循环内 lpub_task_handle/task_grab 不 longjmp,可在其它资源就绪后安全调用。
static task_ctx **_grab_multi_names(lua_State *lua, int32_t idx, int32_t n, int32_t *cnt) {
    task_ctx **dsts;
    MALLOC(dsts, sizeof(task_ctx *) * (size_t)n);
    int32_t count = 0;
    task_ctx *t;
    for (int32_t i = 0; i < n; i++) {
        lua_rawgeti(lua, idx, i + 1);
        if (LUA_TNIL != lua_type(lua, -1)) {
            t = task_grab(g_loader, lpub_task_handle(lua, -1));
            if (NULL != t) {
                dsts[count++] = t;
            }
        }
        lua_pop(lua, 1);
    }
    *cnt = count;
    return dsts;
}
// multi_request / multi_call 的共同前半段:校验 dsts、取载荷、逐个 grab。
// 返回 grab 成功数;返回 0 表示无处可投,此时载荷已按 copy 释放、数组已 FREE,调用方直接返回即可,
// 非 0 时调用方投递完必须调 _multi_done。
// 校验必须排在取载荷之前,理由见 _check_multi_names
static int32_t _multi_prepare(lua_State *lua, int32_t bufidx, _multi_args *ma) {
    int32_t n = _check_multi_names(lua, 1);
    if (n < 0) {
        return luaL_error(lua, "dsts must be a table of task name(string/integer) or nil");
    }
    ma->count = 0;
    ma->dsts = NULL;
    ma->data = _lcore_opt_buf(lua, bufidx, &ma->size, &ma->copy);
    if (n > 0) {
        ma->dsts = _grab_multi_names(lua, 1, n, &ma->count);
    }
    if (0 == ma->count) {
        CHECK_COPY_FREE(ma->data, ma->copy);
        FREE(ma->dsts);
    }
    return ma->count;
}
// 投递之后的收尾:逐个 ungrab 并释放数组
static void _multi_done(_multi_args *ma) {
    for (int32_t i = 0; i < ma->count; i++) {
        task_ungrab(ma->dsts[i]);
    }
    FREE(ma->dsts);
}
/// <summary>
/// 广播请求：把同一份 data 投递给多个 task,各 dst 在 _request 回调中独立 task_response 回 src(共用 sess)。
/// 当前 task 作 src,sess 由调用方传入(非 0)；src 端 srey.on_responsed 会被回调 N 次,业务自行据 sess 识别与累计。
/// </summary>
/// <param name="dsts" type="(string|integer)[]">目标 task 名/句柄数组(Lua table)；nil/NONE/不存在的被跳过</param>
/// <param name="reqtype" type="integer">业务请求类型，取值 [0, UINT16_MAX]，越界报错</param>
/// <param name="sess" type="integer">会话 id(非 0),N 个 dst 共用此 sess</param>
/// <param name="data" type="string|lightuserdata|nil">数据(nil 表示无载荷)；string 时长度自动取,lightuserdata 必须传 size</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填</param>
/// <param name="copy" type="integer?">是否复制数据,只收 0/1,默认 1（复制）;0 时直接转移所有权</param>
/// <returns type="integer">实际成功投递的 dst 数（非 NULL 元素个数,0 表示全部跳过未投递）</returns>
static int32_t _lcore_multi_request(lua_State *lua) {
    LPUB_CUR_TASK(lua, src);
    subtype_t reqtype = lpub_check_u16(lua, 2, REQTYPE_OUT_OF_RANGE);
    uint64_t sess = lpub_check_sess(lua, 3);
    _multi_args ma;
    if (0 == _multi_prepare(lua, 4, &ma)) {
        lua_pushinteger(lua, 0);
        return 1;
    }
    int32_t valid = task_multi_request(ma.dsts, ma.count, src, reqtype, sess,
                                       ma.data, ma.size, ma.copy);
    _multi_done(&ma);
    lua_pushinteger(lua, valid);
    return 1;
}
/// <summary>
/// 单向广播：把同一份 data 投递给多个 task（N 个 message 共享同一份 data，引用计数自动释放）
/// </summary>
/// <param name="dsts" type="(string|integer)[]">目标 task 名/句柄数组(Lua table)；nil/NONE/不存在的被跳过</param>
/// <param name="reqtype" type="integer">业务请求类型，取值 [0, UINT16_MAX]，越界报错</param>
/// <param name="data" type="string|lightuserdata|nil">数据(nil 表示无载荷)；string 时长度自动取,lightuserdata 必须传 size</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填</param>
/// <param name="copy" type="integer?">是否复制数据,只收 0/1,默认 1（复制）;0 时直接转移所有权</param>
/// <returns>无</returns>
static int32_t _lcore_multi_call(lua_State *lua) {
    subtype_t reqtype = lpub_check_u16(lua, 2, REQTYPE_OUT_OF_RANGE);
    _multi_args ma;
    if (0 == _multi_prepare(lua, 3, &ma)) {
        return 0;
    }
    task_multi_call(ma.dsts, ma.count, reqtype, ma.data, ma.size, ma.copy);
    _multi_done(&ma);
    return 0;
}
/// <summary>
/// 向目标 task 发送请求消息，携带会话 id 以便对方响应
/// </summary>
/// <param name="dst" type="string|integer">目标 task 名(字符串)或句柄(整数)</param>
/// <param name="reqtype" type="integer">业务请求类型，取值 [0, UINT16_MAX]，越界报错</param>
/// <param name="sess" type="integer">会话 id(非 0)，响应回带</param>
/// <param name="data" type="string|lightuserdata|nil">消息内容(nil 表示无载荷)；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，只收 0/1，默认 1（复制）</param>
/// <returns type="boolean">grab 到目标并投递 true；目标不存在 false</returns>
static int32_t _lcore_request(lua_State *lua) {
    name_t handle = lpub_task_handle(lua, 1);
    subtype_t reqtype = lpub_check_u16(lua, 2, REQTYPE_OUT_OF_RANGE);
    uint64_t sess = lpub_check_sess(lua, 3);
    void *data;
    size_t size;
    int32_t copy;
    LPUB_CUR_TASK(lua, src);
    data = _lcore_opt_buf(lua, 4, &size, &copy);
    task_ctx *dst = task_grab(g_loader, handle);
    if (NULL == dst) {
        CHECK_COPY_FREE(data, copy);
        return lpub_rtn_bool(lua, 0);
    }
    task_request(dst, src, reqtype, sess, data, size, copy);
    task_ungrab(dst);
    return lpub_rtn_bool(lua, 1);
}
/// <summary>
/// 向请求方 task 回复响应消息，携带错误码及可选数据
/// </summary>
/// <param name="dst" type="string|integer">请求方 task 名(字符串)或句柄(整数)</param>
/// <param name="reqtype" type="integer">请求类型 request_type，取值 [0, UINT16_MAX]，越界报错</param>
/// <param name="sess" type="integer">原请求会话 id</param>
/// <param name="erro" type="integer">错误码，0 表示成功</param>
/// <param name="data" type="string|lightuserdata|nil">响应数据；nil 表示无数据</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，只收 0/1，默认 1（复制）</param>
/// <returns type="boolean">grab 到目标并投递 true；目标不存在 false</returns>
static int32_t _lcore_response(lua_State *lua) {
    name_t handle = lpub_task_handle(lua, 1);
    subtype_t reqtype = lpub_check_u16(lua, 2, REQTYPE_OUT_OF_RANGE);
    uint64_t sess = (uint64_t)luaL_checkinteger(lua, 3);
    int32_t erro = (int32_t)luaL_checkinteger(lua, 4);
    void *data;
    size_t size;
    int32_t copy;
    data = _lcore_opt_buf(lua, 5, &size, &copy);
    task_ctx *dst = task_grab(g_loader, handle);
    if (NULL == dst) {
        CHECK_COPY_FREE(data, copy);
        return lpub_rtn_bool(lua, 0);
    }
    task_response(dst, reqtype, sess, erro, data, size, copy);
    task_ungrab(dst);
    return lpub_rtn_bool(lua, 1);
}
/// <summary>
/// 在当前 task 上监听 TCP/UDP 端口
/// </summary>
/// <param name="pktype" type="integer">封包协议类型，参考 PACK_TYPE</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="ip" type="string">监听 IP。"::" 只收 IPv6(强制 IPV6_V6ONLY)，要同时收两种就 "0.0.0.0" 与 "::" 各监听一次</param>
/// <param name="port" type="integer">监听端口</param>
/// <param name="netev" type="integer?">事件订阅掩码，默认 NETEV_NONE</param>
/// <returns type="integer">监听 id，失败返回 -1</returns>
static int32_t _lcore_listen(lua_State *lua) {
    pack_type pktype = lpub_check_pktype(lua, 1);
    struct evssl_ctx *evssl = lpub_check_evssl(lua, 2);
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = lpub_check_u16(lua, 4, PORT_OUT_OF_RANGE);
    int32_t netev = (int32_t)luaL_optinteger(lua, 5, NETEV_NONE);
    uint64_t id;
    LPUB_CUR_TASK(lua, task);
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
/// <param name="extra" type="lightuserdata?">协议握手所需附加上下文，所有权移交框架</param>
/// <param name="setsess" type="integer?">是否连接时置 ud->sess=skid（同步请求/响应模式），默认 1</param>
/// <returns type="userdata">连接标识；失败时其 valid 字段为 false，返回值个数恒为 1</returns>
static int32_t _lcore_connect(lua_State *lua) {
    pack_type pktype = lpub_check_pktype(lua, 1);
    struct evssl_ctx *evssl = lpub_check_evssl(lua, 2);
    const char *ip = luaL_checkstring(lua, 3);
    uint16_t port = lpub_check_u16(lua, 4, PORT_OUT_OF_RANGE);
    int32_t netev = (int32_t)luaL_optinteger(lua, 5, NETEV_NONE);
    // extra 的所有权在 lua_touserdata 那一刻就离开了 Lua，之后到 task_connect 接管为止不能再有
    // 任何会 longjmp 的调用，否则它既没进框架也没人 ud_free。setsess 与取 task 因此排在前面
    int32_t setsess = (int32_t)luaL_optinteger(lua, 7, 1);
    LPUB_CUR_TASK(lua, task);
    void *extra = NULL;
    if (!lua_isnoneornil(lua, 6)) {
        LUACHECK_LUDATA(lua, 6);
        extra = lua_touserdata(lua, 6);
    }
    sock_ctx sk;
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, extra, setsess, &sk)) {
        // 失败也推一个连接标识,个数与成功路径一致;调用方只判 sk.valid
        return lpub_push_sock_invalid(lua);
    }
    lpub_push_sock(lua, &sk);
    return 1;
}
/// <summary>
/// 对已有明文连接发起 SSL 升级握手
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="client" type="integer">1 表示客户端握手，0 表示服务端；其余值报错</param>
/// <param name="evssl" type="lightuserdata">SSL 上下文；为 nil 或非 userdata 时直接失败</param>
/// <returns type="boolean">成功 true，失败 false</returns>
static int32_t _lcore_ssl_exchange(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    int32_t client = lpub_check_flag(lua, 2);
    if (!lua_islightuserdata(lua, 3)) {
        return lpub_rtn_bool(lua, 0);
    }
    struct evssl_ctx *evssl = lua_touserdata(lua, 3);
    LPUB_CUR_TASK(lua, task);
    return lpub_rtn_bool(lua, ERR_OK == ev_ssl(&task->loader->netev, sk, client, evssl));
}
/// <summary>
/// 创建 UDP 套接字并绑定地址
/// </summary>
/// <param name="pktype" type="integer">封包协议类型，参考 PACK_TYPE</param>
/// <param name="ip" type="string">绑定 IP。"::" 只收 IPv6(强制 IPV6_V6ONLY)；多播时组地址须与此同族</param>
/// <param name="port" type="integer">绑定端口</param>
/// <returns type="userdata">连接标识；失败时其 valid 字段为 false，返回值个数恒为 1</returns>
static int32_t _lcore_udp(lua_State *lua) {
    pack_type pktype = lpub_check_pktype(lua, 1);
    const char *ip = luaL_checkstring(lua, 2);
    uint16_t port = lpub_check_u16(lua, 3, PORT_OUT_OF_RANGE);
    sock_ctx sk;
    LPUB_CUR_TASK(lua, task);
    if (ERR_OK != task_udp(task, pktype, ip, port, &sk)) {
        // 失败也推一个连接标识,个数与成功路径一致;调用方只判 sk.valid
        return lpub_push_sock_invalid(lua);
    }
    lpub_push_sock(lua, &sk);
    return 1;
}
/// <summary>
/// 向指定 fd/skid 发送 TCP 数据
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，只收 0/1，默认 1（复制）</param>
/// <returns type="boolean">成功 true，失败 false</returns>
static int32_t _lcore_send(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    void *data;
    size_t size;
    int32_t copy;
    data = lpub_check_buf(lua, 2, &size, &copy);
    return lpub_rtn_bool(lua, ERR_OK == ev_send(&g_loader->netev, sk, data, size, copy));
}
/// <summary>
/// 多播 TCP 数据：把同一份 data 零拷贝广播给多个连接（N 个 buf 共享，引用计数自动释放）
/// </summary>
/// <param name="sks" type="userdata[]">连接标识数组(Lua table)</param>
/// <param name="data" type="string|lightuserdata">数据；string 时长度自动取,lightuserdata 必须传 size</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填</param>
/// <param name="copy" type="integer?">是否复制数据,只收 0/1,默认 1（复制）;0 时直接转移所有权</param>
/// <returns type="boolean">至少 1 个连接投递成功 true,全部无效 false</returns>
static int32_t _lcore_send_multi(lua_State *lua) {
    luaL_checktype(lua, 1, LUA_TTABLE);
    lua_Integer n = luaL_len(lua, 1);
    // 校验必须整趟走完再取载荷：n 来自 luaL_len，会走 __len 元方法，是业务可控的。
    // 撒谎的 __len 在这里撞上首个非法元素就报错退出，接管那步根本到不了
    lua_Integer i;
    for (i = 0; i < n; i++) {
        lua_rawgeti(lua, 1, i + 1);
        if (!lpub_is_sock(lua, -1)) {
            return luaL_error(lua, "sks[%d] must be a sock, got %s",
                              (int)(i + 1), lua_typename(lua, lua_type(lua, -1)));
        }
        lua_pop(lua, 1);
    }
    size_t size;
    int32_t copy;
    void *data = lpub_check_buf(lua, 2, &size, &copy);
    if (n <= 0) {
        CHECK_COPY_FREE(data, copy);
        return lpub_rtn_bool(lua, 0);
    }
    sock_ctx *sks;
    MALLOC(sks, sizeof(sock_ctx) * (size_t)n);
    for (i = 0; i < n; i++) {
        lua_rawgeti(lua, 1, i + 1);
        sks[i] = *(sock_ctx *)lua_touserdata(lua, -1);
        lua_pop(lua, 1);
    }
    int32_t r = ev_send_multi(&g_loader->netev, sks, (int32_t)n, data, size, copy);
    FREE(sks);
    return lpub_rtn_bool(lua, ERR_OK == r);
}
/// <summary>
/// 向指定 ip:port 发送 UDP 数据
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="ip" type="string">目标 IP</param>
/// <param name="port" type="integer">目标端口</param>
/// <param name="data" type="string|lightuserdata">数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <param name="copy" type="integer?">是否复制数据，只收 0/1，默认 1（复制）</param>
/// <returns type="boolean">成功 true，失败 false</returns>
static int32_t _lcore_sendto(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    const char *ip = luaL_checkstring(lua, 2);
    uint16_t port = lpub_check_u16(lua, 3, PORT_OUT_OF_RANGE);
    void *data;
    size_t size;
    int32_t copy;
    data = lpub_check_buf(lua, 4, &size, &copy);
    return lpub_rtn_bool(lua, ERR_OK == ev_sendto(&g_loader->netev, sk, ip, port, data, size, copy));
}
/// <summary>
/// UDP socket 加入多播组(按 group_ip 的 family 选 IPv4 / IPv6 选项)
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="group_ip" type="string">多播组地址(IPv4 224.0.0.0/4 段 / IPv6 ff00::/8 段)，必须与 socket 绑定地址同族，不同族返 false</param>
/// <param name="iface_str" type="string?">网卡 IP(IPv4) / 接口名(IPv6),nil 走系统默认</param>
/// <returns type="boolean">true 只表示参数合法且命令已入队,setsockopt 成败不回传,契约见 ev_udp_join</returns>
static int32_t _lcore_udp_join(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    const char *group_ip = luaL_checkstring(lua, 2);
    luaL_argcheck(lua, lua_isnoneornil(lua, 3) || LUA_TSTRING == lua_type(lua, 3), 3, "iface must be a string");
    const char *iface_str = (LUA_TSTRING == lua_type(lua, 3)) ? luaL_checkstring(lua, 3) : NULL;
    return lpub_rtn_bool(lua, ERR_OK == ev_udp_join(&g_loader->netev, sk, group_ip, iface_str));
}
/// <summary>
/// UDP socket 离开多播组,参数同 udp_join
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="group_ip" type="string">多播组地址(IPv4 224.0.0.0/4 段 / IPv6 ff00::/8 段)，必须与 socket 绑定地址同族，不同族返 false</param>
/// <param name="iface_str" type="string?">网卡 IP(IPv4) / 接口名(IPv6),nil 走系统默认</param>
/// <returns type="boolean">true 只表示参数合法且命令已入队,setsockopt 成败不回传,契约见 ev_udp_join</returns>
static int32_t _lcore_udp_leave(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    const char *group_ip = luaL_checkstring(lua, 2);
    luaL_argcheck(lua, lua_isnoneornil(lua, 3) || LUA_TSTRING == lua_type(lua, 3), 3, "iface must be a string");
    const char *iface_str = (LUA_TSTRING == lua_type(lua, 3)) ? luaL_checkstring(lua, 3) : NULL;
    return lpub_rtn_bool(lua, ERR_OK == ev_udp_leave(&g_loader->netev, sk, group_ip, iface_str));
}
/// <summary>
/// 设置 UDP 多播 TTL(IPv4) / Hop Limit(IPv6)
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="ttl" type="integer">0-255；0 只到本机，1(默认) 只到本网段，逐跳递减。
/// 越界即抛错——直接窄化到 uint8_t 的话 256 会静默变成 0，多播从此出不了本机，
/// 是语义反转而不是"值不对"，排查起来比报错难得多</param>
/// <returns type="boolean">true 只表示参数合法且命令已入队,setsockopt 成败不回传,契约见 ev_udp_join</returns>
static int32_t _lcore_udp_ttl(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    lua_Integer val = luaL_checkinteger(lua, 2);
    luaL_argcheck(lua, val >= 0 && val <= 255, 3, "ttl out of range [0, 255]");
    return lpub_rtn_bool(lua, ERR_OK == ev_udp_ttl(&g_loader->netev, sk, (uint8_t)val));
}
/// <summary>
/// 设置 UDP 多播本机回环,默认 1(发出去自己也能收到),0=不收
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="enable" type="integer">1=回环(默认,发出自收),0=不收；其余值报错</param>
/// <returns type="boolean">true 只表示参数合法且命令已入队,setsockopt 成败不回传,契约见 ev_udp_join</returns>
static int32_t _lcore_udp_loop(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    int32_t enable = lpub_check_flag(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == ev_udp_loop(&g_loader->netev, sk, enable));
}
/// <summary>
/// 取一个无效的连接标识。供脚本侧"还没走到 C 就失败"的路径用，
/// 让成功与失败返回同一种类型，调用方只判 sk.valid
/// </summary>
/// <returns type="userdata">连接标识，其 valid 字段恒为 false</returns>
static int32_t _lcore_sock_invalid(lua_State *lua) {
    return lpub_push_sock_invalid(lua);
}
/// <summary>
/// 主动关闭指定 fd/skid 的网络连接；未发数据的丢弃契约同 ev_close
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <returns>无</returns>
static int32_t _lcore_close(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    ev_close(&g_loader->netev, sk);
    return 0;
}
/// <summary>
/// 动态修改指定连接的封包协议类型
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="pktype" type="integer">新封包协议类型，参考 PACK_TYPE</param>
/// <returns type="boolean">成功 true，stop 非0失败</returns>
static int32_t _lcore_pack_type(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    // ud->pktype 是 uint16_t，比 pack_type 还窄；不校验的话 65538 截成 2 就成了 PACK_HTTP
    subtype_t pktype = (subtype_t)lpub_check_pktype(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == ev_ud_pktype(&g_loader->netev, sk, pktype));
}
/// <summary>
/// 设置指定连接的用户自定义状态值
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="status" type="integer">用户自定义状态值（int8）</param>
/// <returns type="boolean">成功 true，stop 非0失败</returns>
static int32_t _lcore_status(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    int8_t status = lpub_check_i8(lua, 2, "status out of range");
    return lpub_rtn_bool(lua, ERR_OK == ev_ud_status(&g_loader->netev, sk, status));
}
/// <summary>
/// 将指定连接绑定到目标 task（后续网络消息投递到该 task）
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="name" type="string|integer">目标字符串名或数字句柄</param>
/// <returns type="boolean">成功 true；目标 task 不存在(名字未注册 / 句柄对应 task 已退出)或事件线程已停时 false。
///   仅保证调用时目标存在——若目标在绑定之后才退出,该连接下一条消息仍会被静默关闭</returns>
static int32_t _lcore_bind_task(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    name_t handle = lpub_task_handle(lua, 2);
    // 用 grab 探存在性:lpub_task_handle 对数字句柄原样返回,业务缓存的旧句柄在目标退出后
    // 照样非 INVALID_TNAME、单查该值会放行;task_grab 首行已挡 INVALID_TNAME,一次覆盖两种
    task_ctx *dst = task_grab(g_loader, handle);
    if (NULL == dst) {
        return lpub_rtn_bool(lua, 0);
    }
    task_ungrab(dst);
    return lpub_rtn_bool(lua, ERR_OK == ev_ud_handle(&g_loader->netev, sk, handle));
}
// session / session_clear 共用：取连接标识,把会话键设成 use_skid 非 0 时的 skid、否则 0
static int32_t _lcore_session_set(lua_State *lua, int32_t use_skid) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    uint64_t sess = (0 != use_skid) ? sk->skid : 0;
    return lpub_rtn_bool(lua, ERR_OK == ev_ud_sess(&g_loader->netev, sk, sess));
}
/// <summary>
/// 把连接的会话键设为它自己的 skid，后续该 socket 的消息携带此值。与 C 侧 coro_sync 同形，
/// 会话键不可自定义：CLOSE 恒以 skid 为 sess 发出，挂在别的键上的等待者断连时一个都唤不到
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <returns type="boolean">成功 true，stop 非0失败</returns>
static int32_t _lcore_session(lua_State *lua) {
    return _lcore_session_set(lua, 1);
}
/// <summary>
/// 清除连接的会话键（置 0），此后该 socket 的消息不再携带会话键，一律走注册的回调而非协程等待。
/// 0 上挂不了等待者(会话 id 由 createid 生成，恒非 0)，故清除不会漏唤醒已挂起的协程；
/// 清除前已挂起在该 skid 上的等待者仍由 CLOSE 唤醒——它恒以 skid 为 sess 发出，与本设置无关
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <returns type="boolean">成功 true，stop 非0失败</returns>
static int32_t _lcore_session_clear(lua_State *lua) {
    return _lcore_session_set(lua, 0);
}
/// <summary>
/// 把本次要发的请求方法登记到连接上：HTTP 解包侧要靠它才能判定响应有无报文体，契约见 http_set_method。
/// 与组请求成对调用即可，须在发送之前；哪些方法需要特殊处理由 C 侧判断
/// </summary>
/// <param name="sk" type="userdata">连接标识，由 core.connect / core.udp / 各 accept 回调给出</param>
/// <param name="method" type="string">与请求行同一个 method；按 RFC 7231 §4.1 区分大小写</param>
/// <returns type="boolean">成功 true（含"该方法无需登记"这一档）；fd 为 INVALID_SOCK 时 false</returns>
static int32_t _lcore_http_set_method(lua_State *lua) {
    sock_ctx *sk = lpub_check_sock(lua, 1);
    const char *method = luaL_checkstring(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == http_set_method(&g_loader->netev, sk, method));
}
/// <summary>
/// 询问协议层指定封包能否唤醒等待者(非 true 时框架改新建协程走 on_recved),契约见 prots_may_resume
/// </summary>
/// <param name="pktype" type="integer">封包协议类型，参考 PACK_TYPE</param>
/// <param name="data" type="lightuserdata">协议层封包指针</param>
/// <returns type="boolean">可唤醒等待者 true；false 表示该包不走命令响应路径</returns>
static int32_t _lcore_may_resume(lua_State *lua) {
    pack_type pktype = lpub_check_pktype(lua, 1);
    // 放行 nil:C 侧 prots_may_resume 明确接受 data 为 NULL(视为无包可拦,返 ERR_OK),
    // 这里强求 lightuserdata 就比它窄一档,msg.data 缺席时会把整条分发抛崩
    luaL_argexpected(lua, lua_islightuserdata(lua, 2) || lua_isnoneornil(lua, 2),
                     2, "light userdata or nil");
    void *data = lua_touserdata(lua, 2);
    return lpub_rtn_bool(lua, ERR_OK == prots_may_resume(pktype, data));
}
/// <summary>
/// 询问该消息类型对应的 sess 是否可能保留（waiters 摘空后不删除会话表条目）
/// </summary>
/// <param name="mtype" type="integer">消息类型，参考 MSG_TYPE；越界报错</param>
/// <returns type="boolean">true=可能保留（TCP/UDP 等 skid 类长连接场景）；false=不保留</returns>
static int32_t _lcore_message_may_keep(lua_State *lua) {
    msg_type mtype = (msg_type)lpub_check_range(lua, 1, MSG_TYPE_NONE, MSG_TYPE_ALL - 1, MTYPE_OUT_OF_RANGE);
    return lpub_rtn_bool(lua, _message_may_keep(mtype));
}
/// <summary>
/// 取消息类型的名字。给 Lua 侧的 MSG_TYPE 表做钉子用：那张表是按 C 枚举手抄的字面量，
/// 靠这个逐项对名字，C 侧插入新成员导致取值整体后移时测试会红，而不是 _dispatchers 静默错投
/// </summary>
/// <param name="mtype" type="integer">消息类型，参考 MSG_TYPE；越界报错</param>
/// <returns type="string">类型名（"RECV" / "CLOSE" …）；名字表漏填该成员时为空串</returns>
static int32_t _lcore_message_str(lua_State *lua) {
    msg_type mtype = (msg_type)lpub_check_range(lua, 1, MSG_TYPE_NONE, MSG_TYPE_ALL - 1, MTYPE_OUT_OF_RANGE);
    lua_pushstring(lua, _message_str(mtype));
    return 1;
}
// task_list 收集回调：仅存入 C 数组，不调 Lua API，避免 OOM longjmp 绕过 rwlock 解锁。
// 名字必须在锁内拷走：task->name 出锁后随 task 一起可能被释放。拷贝由 _lcore_task_list 押完即 FREE
static void _lcore_task_list_collect(const char *name, name_t handle, void *arg) {
    // 清零承重在 name 上:名字为空时下面那个分支不跑,不清零就是把野指针整块 memcpy 进数组
    _task_entry entry = { 0 };
    entry.handle = handle;
    if (!EMPTYSTR(name)) {
        entry.name = dup_zero(name, strlen(name));
    }
    array_push_back((array_ctx *)arg, &entry);
}
/// <summary>
/// 枚举当前 loader 已注册的所有 task（C 层列表）
/// </summary>
/// <returns type="TaskListItem[]">task 列表；无 task 时为空表。匿名 task（task_new 时名字为空）
/// 只交出 handle、不带 name</returns>
static int32_t _lcore_task_list(lua_State *lua) {
    array_ctx arr;
    array_init(&arr, sizeof(_task_entry), 128);
    loader_task_each(g_loader, _lcore_task_list_collect, &arr);
    lua_newtable(lua);
    _task_entry *entry;
    uint32_t n = array_size(&arr);
    for (uint32_t i = 0; i < n; i++) {
        entry = (_task_entry *)array_at(&arr, (int32_t)i);
        lua_newtable(lua);
        if (NULL != entry->name) {
            lua_pushstring(lua, entry->name);
            lua_setfield(lua, -2, "name");
            FREE(entry->name);
        }
        lua_pushinteger(lua, (lua_Integer)entry->handle);
        lua_setfield(lua, -2, "handle");
        lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    array_free(&arr);
    return 1;
}
/// <summary>
/// 获取全局内存分配/释放统计（MEMORY_CHECK 关闭时全为 0）
/// </summary>
/// <returns type="MemStat">分配计数快照</returns>
static int32_t _lcore_mem_stat(lua_State *lua) {
    uint64_t nalloc = 0, nfree = 0;
    mem_stat(&nalloc, &nfree);
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)nalloc);
    lua_setfield(lua, -2, "nalloc");
    lua_pushinteger(lua, (lua_Integer)nfree);
    lua_setfield(lua, -2, "nfree");
    // 两个出参不是一致快照(见 memory.h), nfree 可能读得比 nalloc 大, 裸减就下溢
    lua_pushinteger(lua, (lua_Integer)((nalloc >= nfree) ? (nalloc - nfree) : 0));
    lua_setfield(lua, -2, "live");
    return 1;
}
/// <summary>
/// 查询本次构建有没有把 SSL 编进来
/// </summary>
/// <returns type="boolean">编了 true；没编 false，此时 cert_register / p12_register 恒返 nil、
/// ssl_min_proto 恒返 false、seclevel / verify 是空操作</returns>
static int32_t _lcore_with_ssl(lua_State *lua) {
    return lpub_rtn_bool(lua, WITH_SSL);
}
#if WITH_SSL
// name 非空时把 cert 目录下的完整路径写进 out；为空则 out 保持调用方给的空串(表示不加载)
static void _cert_path(const char *propath, const char *name, char *out, size_t outlen) {
    if (0 != strlen(name)) {
        SNPRINTF(out, outlen, "%s%s%s%s%s",
            propath, PATH_SEPARATORSTR, CERT_FOLDER, PATH_SEPARATORSTR, name);
    }
}
// 注册结果压栈:成功压 ssl 指针,ssl 没建出来或注册失败一律压 nil。
// 注册失败不必在这里释放 ssl —— evssl_register 每条失败路径都已 evssl_free
static int32_t _push_registered_ssl(lua_State *lua, const char *name, evssl_ctx *ssl) {
    if (NULL != ssl
        && ERR_OK == evssl_register(name, ssl)) {
        lua_pushlightuserdata(lua, ssl);
    } else {
        lua_pushnil(lua);
    }
    return 1;
}
#endif
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
    char capath[PATH_LENS] = { 0 };
    char certpath[PATH_LENS] = { 0 };
    char keypath[PATH_LENS] = { 0 };
    char propath[PATH_LENS] = { 0 };
    if (ERR_OK != global_string(lua, PATH_NAME, propath, sizeof(propath))) {
        return lpub_rtn_nil(lua, 1);
    }
    _cert_path(propath, ca, capath, sizeof(capath));
    _cert_path(propath, cert, certpath, sizeof(certpath));
    _cert_path(propath, key, keypath, sizeof(keypath));
    return _push_registered_ssl(lua, name, evssl_new(capath, certpath, keypath, keytype));
#else
    return lpub_rtn_nil(lua, 1);
#endif
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
    char propath[PATH_LENS] = { 0 };
    if (ERR_OK != global_string(lua, PATH_NAME, propath, sizeof(propath))) {
        return lpub_rtn_nil(lua, 1);
    }
    _cert_path(propath, p12, p12path, sizeof(p12path));
    return _push_registered_ssl(lua, name, evssl_p12_new(p12path, pwd));
#else
    return lpub_rtn_nil(lua, 1);
#endif
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
/// <param name="level" type="integer">安全级别 0-5；越界报错</param>
static int32_t _lcore_ssl_seclevel(lua_State *lua) {
#if WITH_SSL
    LPUB_LUD_ARG(lua, struct evssl_ctx, 1, ssl);
    int32_t level = (int32_t)lpub_check_range(lua, 2, 0, 5, SECLEVEL_OUT_OF_RANGE);
    evssl_seclevel(ssl, level);
#else
    (void)lua;
#endif
    return 0;
}
/// <summary>
/// 设置最低 TLS 协议版本
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="version" type="TLS_VERSION">
///        协议版本：
///            AUTO(0) 不设下限，用库支持的最低版本（OpenSSL 的默认状态，getter 也用 0 表示它）
///            TLS1_VERSION(0x0301)
///            TLS1_1_VERSION(0x0302)
///            TLS1_2_VERSION(0x0303)
///            TLS1_3_VERSION(0x0304)
///        表外的值报错
/// </param>
/// <returns type="boolean">设置成功 true；该版本被当前构建或安全级别排除时 false，此时最低版本
/// 保持原样。不判返回值就会以为下限抬上去了，实际仍在接受更低的版本。WITH_SSL 关闭时恒 false</returns>
static int32_t _lcore_ssl_min_proto(lua_State *lua) {
#if WITH_SSL
    LPUB_LUD_ARG(lua, struct evssl_ctx, 1, ssl);
    lua_Integer version = luaL_checkinteger(lua, 2);
    luaL_argcheck(lua, 0 == version
                  || (version >= TLS1_VERSION && version <= TLS1_3_VERSION),
                  2, TLSVER_OUT_OF_RANGE);
    return lpub_rtn_bool(lua, ERR_OK == evssl_min_proto(ssl, (int32_t)version));
#else
    return lpub_rtn_bool(lua, 0);
#endif
}
/// <summary>
/// 设置 SSL 上下文是否验证对端证书
/// </summary>
/// <param name="evssl" type="lightuserdata">SSL 上下文指针</param>
/// <param name="mod" type="integer">SSL_VERIFY_* 掩码：0 不验证 / SSL_VERIFY_PEER 验对端 / 叠加 SSL_VERIFY_FAIL_IF_NO_PEER_CERT 强制对端出证书(mTLS)</param>
static int32_t _lcore_ssl_verify(lua_State *lua) {
#if WITH_SSL
    LPUB_LUD_ARG(lua, struct evssl_ctx, 1, ssl);
    int32_t mod = (int32_t)luaL_checkinteger(lua, 2);
    evssl_verify(ssl, mod, NULL);
#else
    (void)lua;
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
        { "sock_invalid", _lcore_sock_invalid },

        { "pack_type", _lcore_pack_type },
        { "status", _lcore_status },
        { "bind_task", _lcore_bind_task },
        { "session", _lcore_session },
        { "session_clear", _lcore_session_clear },
        { "http_set_method", _lcore_http_set_method },

        { "may_resume", _lcore_may_resume },
        { "message_may_keep", _lcore_message_may_keep },
        { "message_str", _lcore_message_str },

        { "task_list", _lcore_task_list },
        { "mem_stat", _lcore_mem_stat },

        { "with_ssl", _lcore_with_ssl },
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
