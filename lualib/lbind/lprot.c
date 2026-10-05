#include "lbind/lpub.h"

#define MT_SMTP      "_smtp_ctx"
#define MT_SMTP_MAIL "_smtp_mail_ctx"
// smtp check_codes 一次最多收几个码（栈上数组上限）。SMTP 单条命令的合法应答码就那么几个，
// 给到 8 已远超需要；超了直接报错而不是截断
#define SMTP_MAX_NCODE 8
// Redis 聚合表一次预分配的槽位上限。不是协议上限,只是"预分配到此为止":元素个数由对端声明,
// 超出的部分照常按需增长
#define REDIS_PREALLOC_MAX 4096
// http head_check 的 autoflags 位：组包函数自己会写哪些头，调用方再传一份就要丢
#define HTTP_AUTO_CT    0x01 // Content-Type
#define HTTP_AUTO_FRAME 0x02 // Content-Length 与 Transfer-Encoding
// http 响应组包除头部块、头表与报文体外的预留：状态行 + JSON 的 Content-Type + 帧长头 + 空行
#define HTTP_RESP_RESERVE 128
#define HTTP_JSON_CT "Content-Type: application/json\r\n" // table 报文体自动带的头，同 lib/http

// http head_check 的结果码，按判定先后排
typedef enum http_head_rc {
    HTTP_HEAD_OK = 0,
    HTTP_HEAD_BADKEY,  // 头名不是 token
    HTTP_HEAD_BADTYPE, // 值不是 string / number
    HTTP_HEAD_AUTO,    // 组包函数自己生成的头
    HTTP_HEAD_BADVAL   // 值含 NUL / CR / LF
}http_head_rc;
// http.respond / pack_resp 预检的结果：预检全在分配之前做完，组包阶段只照着写、不会再抛错
typedef struct http_resp_plan {
    int32_t code;
    int32_t nobody;    // 1xx/204/304：不写帧长头也不写报文体
    int32_t headonly;  // 回 HEAD：Content-Length 写真实长度，不写报文体
    int32_t hidx;      // 头表的栈位，没有头表为 0
    int32_t json;      // 报文体是 table：组包时才编成 JSON，body 指向编码输出
    const char *block; // 预渲染好的头部块，没有为 NULL
    const char *body;  // 报文体，没有为 NULL
    size_t bklens;
    size_t blens;
    size_t hlens;      // 头表写出的总字节
}http_resp_plan;
// table 报文体编成 JSON 后交给组包的上下文
typedef struct http_resp_json {
    lua_State *lua;
    http_resp_plan *plan;
    binary_ctx *bw;
}http_resp_json;

/// <summary>
/// 打包 harbor 跨节点消息
/// </summary>
/// <param name="task" type="integer">目标 task name</param>
/// <param name="call" type="integer">调用类型：非0=call（单向），0=request（双向）</param>
/// <param name="reqtype" type="integer">业务请求类型，取值 [0, UINT16_MAX]，越界报错</param>
/// <param name="data" type="string|lightuserdata|nil">消息内容；nil 表示无数据</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数，取值 [0, INT32_MAX]</param>
/// <returns type="lightuserdata">打包后的数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_harbor_pack(lua_State *lua) {
    name_t task = (name_t)luaL_checkinteger(lua, 1);
    int32_t call = (int32_t)luaL_checkinteger(lua, 2);// 非0=call，0=request
    subtype_t reqtype = lpub_check_u16(lua, 3, REQTYPE_OUT_OF_RANGE);
    size_t size;
    void *data = lpub_opt_buf(lua, 4, &size);
    data = harbor_pack(task, call, reqtype, data, size, &size);
    return lpub_rtn_lud(lua, data, size);
}
//srey.harbor
LUAMOD_API int luaopen_harbor(lua_State *lua) {
    luaL_Reg reg[] = {
        { "pack", _lprot_harbor_pack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 返回配置的 DNS 服务器 IP 地址
/// </summary>
/// <param>无</param>
/// <returns type="string">DNS 服务器 IP</returns>
static int32_t _lprot_dns_ip(lua_State *lua) {
    lua_pushstring(lua, dns_get_ip());
    return 1;
}
/// <summary>
/// 构造 UDP DNS 查询请求包（不含 2 字节长度前缀）
/// </summary>
/// <param name="domain" type="string">查询域名</param>
/// <param name="ipv6" type="integer">1 查询 AAAA 记录，0 查询 A 记录；其余值报错</param>
/// <returns type="string?">DNS 查询二进制字符串；构造失败返回 nil</returns>
/// <returns type="integer?">本次查询的事务 ID，传给 dns.unpack 回验响应；失败时同为 nil，返回值个数恒为 2</returns>
static int32_t _lprot_dns_pack(lua_State *lua) {
    const char *domain = luaL_checkstring(lua, 1);
    int32_t ipv6 = lpub_check_flag(lua, 2);
    char buf[ONEK];
    uint16_t id;
    size_t lens = (size_t)dns_request_pack(buf, domain, ipv6, &id);
    if (0 == lens) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushlstring(lua, buf, lens);
    lua_pushinteger(lua, id);
    return 2;
}
/// <summary>
/// 构造 TCP DNS 查询请求包（含 2 字节大端长度前缀，RFC 1035 §4.2.2 / RFC 7766）
/// </summary>
/// <param name="domain" type="string">查询域名</param>
/// <param name="ipv6" type="integer">1 查询 AAAA 记录，0 查询 A 记录；其余值报错</param>
/// <returns type="string?">含长度前缀的 DNS 查询二进制字符串；构造失败返回 nil</returns>
/// <returns type="integer?">本次查询的事务 ID，传给 dns.unpack 回验响应；失败时同为 nil，返回值个数恒为 2</returns>
static int32_t _lprot_dns_pack_tcp(lua_State *lua) {
    const char *domain = luaL_checkstring(lua, 1);
    int32_t ipv6 = lpub_check_flag(lua, 2);
    char buf[ONEK];
    uint16_t id;
    size_t lens = dns_request_pack_tcp(buf, domain, ipv6, &id);
    if (0 == lens) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushlstring(lua, buf, lens);
    lua_pushinteger(lua, id);
    return 2;
}
/// <summary>
/// 解析 DNS 响应包，提取 IP 地址列表
/// </summary>
/// <param name="pack" type="lightuserdata">DNS 响应数据指针（裸报文，不含 TCP 长度前缀）</param>
/// <param name="packlen" type="integer">响应包字节数，取值 [0, INT32_MAX]</param>
/// <param name="id" type="integer">期望的事务 ID（dns.pack/dns.pack_tcp 返回）；响应事务 ID 不匹配即视为错配/伪造返回 nil</param>
/// <returns type="string[]?">IP 字符串数组；事务 ID 不匹配、解析失败、RCODE 非 0 或响应被截断(TC 位置位)时均返回 nil</returns>
/// <returns type="boolean">第二返回值 nodata：true 表示响应本身完整有效、只是没有任何 A/AAAA 记录
/// （NOERROR/NODATA），换 TCP 重查也是同一结果，调用方不必再试；解析成功时恒为 false</returns>
static int32_t _lprot_dns_unpack(lua_State *lua) {
    LPUB_LUD_ARG(lua, void, 1, pack);
    size_t packlen = lpub_check_lens(lua, 2, INT32_MAX);
    uint16_t id = lpub_check_u16(lua, 3, "transaction id out of range");
    size_t n;
    int32_t nodata = 0;
    dns_ip *ips = dns_parse_pack(pack, packlen, &n, id, &nodata);
    if (NULL == ips) {
        lua_pushnil(lua);
        lua_pushboolean(lua, 0 != nodata ? 1 : 0);
        return 2;
    }
    lua_createtable(lua, (int32_t)n, 0);
    for (size_t i = 0; i < n; i++) {
        ips[i].ip[IP_LENS - 1] = '\0';
        lua_pushstring(lua, ips[i].ip);
        lua_rawseti(lua, -2, (lua_Integer)(i + 1));
    }
    FREE(ips);
    lua_pushboolean(lua, 0);
    return 2;
}
//srey.dns
LUAMOD_API int luaopen_dns(lua_State *lua) {
    luaL_Reg reg[] = {
        { "ip", _lprot_dns_ip },
        { "pack", _lprot_dns_pack },
        { "pack_tcp", _lprot_dns_pack_tcp },
        { "unpack", _lprot_dns_unpack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 打包自定义协议（custz）数据
/// </summary>
/// <param name="pktype" type="integer">协议子类型，只接受 PACK_CUSTZ_FIXED / FLAG / VAR，其余报错</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">打包后的数据指针；载荷超出所选头部能表达的范围时返回 nil</returns>
/// <returns type="integer">数据长度；返回 nil 时为 0</returns>
static int32_t _lprot_custz_pack(lua_State *lua) {
    // 只认三个 custz 子类型：custz_pack 的 default 是 ASSERTAB(0)，传别的进去当场 abort 整个进程
    pack_type pktype = lpub_check_pktype(lua, 1);
    luaL_argcheck(lua, PACK_CUSTZ_FIXED == pktype
                  || PACK_CUSTZ_FLAG == pktype
                  || PACK_CUSTZ_VAR == pktype,
                  1, "expected a PACK_CUSTZ_* pack type");
    void *data;
    size_t size;
    data = lpub_check_buf(lua, 2, &size, NULL);
    data = custz_pack(pktype, data, size, &size);
    if (NULL == data) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    return lpub_rtn_lud(lua, data, size);
}
//srey.custz
LUAMOD_API int luaopen_custz(lua_State *lua) {
    luaL_Reg reg[] = {
        { "pack", _lprot_custz_pack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 解包 WebSocket 帧
/// </summary>
/// <param name="pack" type="lightuserdata">websock_pack_ctx 指针</param>
/// <returns type="WebSocketFrame">含 fin / prot / secprot / secpack / data / size 字段的表；
///   secprot / secpack 仅在存在时填充，data 恒有：它是包尾柔性数组的地址，
///   零长帧下仍是有效指针（size 为 0），可直接连同 size 转手给取 buf 的接口</returns>
static int32_t _lprot_websock_unpack(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct websock_pack_ctx, 1, pack);
    lua_createtable(lua, 0, 6);
    lua_pushinteger(lua, websock_fin(pack));// 是否为最终分片
    lua_setfield(lua, -2, "fin");
    lua_pushinteger(lua, websock_prot(pack));// 帧操作码（文本/二进制/控制帧等）
    lua_setfield(lua, -2, "prot");
    int32_t secprot = websock_secprot(pack);
    if (PACK_NONE != secprot) {
        lua_pushinteger(lua, secprot);// 子协议类型
        lua_setfield(lua, -2, "secprot");
    }
    void *secpack = websock_secpack(pack);
    if (NULL != secpack) {
        lua_pushlightuserdata(lua, secpack);// 子协议数据包指针
        lua_setfield(lua, -2, "secpack");
    }
    size_t lens;
    void *data = websock_data(pack, &lens);
    // 零长帧也照推：websock_data 返的是包尾柔性数组的地址，恒非 NULL，
    // 不设字段的话上层拿到 nil，转手喂给 lpub_check_buf 一族就是
    // "string or light userdata expected" 抛错
    lua_pushlightuserdata(lua, data);
    lua_setfield(lua, -2, "data");
    lua_pushinteger(lua, lens);
    lua_setfield(lua, -2, "size");
    return 1;
}
/// <summary>
/// 解包 WebSocket 帧，同 unpack 但按多返回值给出、不建表
/// </summary>
/// <param name="pack" type="lightuserdata">websock_pack_ctx 指针</param>
/// <returns type="integer">fin：是否为最终分片</returns>
/// <returns type="integer">prot：帧操作码</returns>
/// <returns type="integer?">secprot：子协议类型；没有子协议时为 nil</returns>
/// <returns type="lightuserdata?">secpack：子协议数据包指针；没有时为 nil</returns>
/// <returns type="lightuserdata">data：载荷指针，恒非 nil，理由同 unpack</returns>
/// <returns type="integer">size：载荷字节数</returns>
static int32_t _lprot_websock_frame(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct websock_pack_ctx, 1, pack);
    lua_pushinteger(lua, websock_fin(pack));
    lua_pushinteger(lua, websock_prot(pack));
    int32_t secprot = websock_secprot(pack);
    if (PACK_NONE != secprot) {
        lua_pushinteger(lua, secprot);
    } else {
        lua_pushnil(lua);
    }
    void *secpack = websock_secpack(pack);
    if (NULL != secpack) {
        lua_pushlightuserdata(lua, secpack);
    } else {
        lua_pushnil(lua);
    }
    size_t lens;
    lua_pushlightuserdata(lua, websock_data(pack, &lens));
    lua_pushinteger(lua, lens);
    return 6;
}
/// <summary>
/// 构造 WebSocket 握手请求包（HTTP Upgrade）
/// </summary>
/// <param name="host" type="string?">Host 头字段；nil 表示省略</param>
/// <param name="uri" type="string?">HTTP request-target（path?query）；nil 或空字符串时使用 "/"</param>
/// <param name="secprot" type="string?">Sec-WebSocket-Protocol 字段；nil 表示省略</param>
/// <returns type="lightuserdata?">握手包数据指针；secprot 超长、或 host/uri/secprot 任一含 CR/LF（会拆出额外的 HTTP 头）时返回 nil。
/// 业务通过 srey.send copy=0 接管或 utils.ud_free 释放</returns>
/// <returns type="integer?">数据长度</returns>
/// <returns type="lightuserdata?">握手上下文 hsctx。传给 srey.connect 当 extra 的那一刻所有权才转交框架，此后业务不得 ud_free；
/// 没走到 srey.connect（打完包就不连了、中途出错）时它仍归业务，必须自己 ud_free。
/// 失败时三个返回值都是 nil，个数恒为 3</returns>
static int32_t _lprot_websock_pack_handshake(lua_State *lua) {
    luaL_argcheck(lua, lua_isnoneornil(lua, 1) || LUA_TSTRING == lua_type(lua, 1), 1, "host must be a string");
    luaL_argcheck(lua, lua_isnoneornil(lua, 2) || LUA_TSTRING == lua_type(lua, 2), 2, "uri must be a string");
    luaL_argcheck(lua, lua_isnoneornil(lua, 3) || LUA_TSTRING == lua_type(lua, 3), 3, "secprot must be a string");
    char *host = NULL;
    if (LUA_TSTRING == lua_type(lua, 1)) {
        host = (char *)luaL_checkstring(lua, 1);
    }
    char *uri = NULL;
    if (LUA_TSTRING == lua_type(lua, 2)) {
        uri = (char *)luaL_checkstring(lua, 2);
    }
    char *secprot = NULL;
    if (LUA_TSTRING == lua_type(lua, 3)) {
        secprot = (char *)luaL_checkstring(lua, 3);
    }
    ws_hs_ctx *hsctx;
    char *hspack = websock_pack_handshake(host, uri, secprot, &hsctx);
    if (NULL == hspack) {
        return lpub_rtn_nil(lua, 3);
    }
    lua_pushlightuserdata(lua, hspack);
    lua_pushinteger(lua, strlen(hspack));
    lua_pushlightuserdata(lua, hsctx);
    return 3;
}
/// <summary>
/// 构造 WebSocket Ping 控制帧
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端发送 1，服务端 0）；只收 0/1，其余报错</param>
/// <returns type="lightuserdata?">数据指针；mask 非 0 且取不到 CSPRNG 熵生成掩码 key 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lprot_websock_pack_ping(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    size_t lens;
    void *pack = websock_pack_ping(mask, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket Pong 控制帧
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <returns type="lightuserdata?">数据指针；mask 非 0 且取不到 CSPRNG 熵生成掩码 key 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lprot_websock_pack_pong(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    size_t lens;
    void *pack = websock_pack_pong(mask, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket Close 控制帧
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <returns type="lightuserdata?">数据指针；mask 非 0 且取不到 CSPRNG 熵生成掩码 key 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lprot_websock_pack_close(lua_State *lua) {
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    size_t lens;
    void *pack = websock_pack_close(mask, &lens);
    return lpub_rtn_lud(lua, pack, lens);
}
/// <summary>
/// 构造 WebSocket 文本帧（首帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="fin" type="integer">1 表示完整消息，0 表示后续有 continuation 帧；只收 0/1，其余报错</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">数据指针；mask 非 0 且取不到 CSPRNG 熵生成掩码 key 时返回 nil</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lprot_websock_pack_text(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    int32_t fin = (int32_t)lpub_check_flag(lua, 2);
    data = lpub_check_buf(lua, 3, &dlens, NULL);
    void *pack = websock_pack_text(mask, fin, data, dlens, &dlens);
    return lpub_rtn_lud(lua, pack, dlens);
}
/// <summary>
/// 构造 WebSocket 二进制帧（首帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="fin" type="integer">1 表示完整消息，0 表示后续有 continuation 帧；只收 0/1，其余报错</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">数据指针；返回 nil 的情形同 pack_text</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lprot_websock_pack_binary(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    int32_t fin = (int32_t)lpub_check_flag(lua, 2);
    data = lpub_check_buf(lua, 3, &dlens, NULL);
    void *pack = websock_pack_binary(mask, fin, data, dlens, &dlens);
    return lpub_rtn_lud(lua, pack, dlens);
}
/// <summary>
/// 构造 WebSocket Continuation 帧（分片消息的中间或最后帧）
/// </summary>
/// <param name="mask" type="integer">是否启用掩码（客户端 1，服务端 0）；只收 0/1，其余报错</param>
/// <param name="fin" type="integer">1 表示最后帧（PROT_SLICE_END），0 表示中间帧；只收 0/1，其余报错</param>
/// <param name="data" type="string|lightuserdata">载荷数据；字符串时长度自动取得</param>
/// <param name="size" type="integer?">data 为 lightuserdata 时必填，表示数据字节数</param>
/// <returns type="lightuserdata?">数据指针；返回 nil 的情形同 pack_text</returns>
/// <returns type="integer?">数据长度</returns>
static int32_t _lprot_websock_pack_continua(lua_State *lua) {
    void *data;
    size_t dlens;
    int32_t mask = (int32_t)lpub_check_flag(lua, 1);
    int32_t fin = (int32_t)lpub_check_flag(lua, 2);
    data = lpub_check_buf(lua, 3, &dlens, NULL);
    void *pack = websock_pack_continua(mask, fin, data, dlens, &dlens);
    return lpub_rtn_lud(lua, pack, dlens);
}
/// <summary>
/// 解析 HANDSHAKED 交付的 ws_secprots_ctx，返回匹配到的子协议下标与全部子协议名列表
/// </summary>
/// <param name="spctx" type="lightuserdata">握手交付的 ws_secprots_ctx 指针；仅本协程下次挂起前有效，勿保留</param>
/// <returns type="integer?">匹配到的子协议下标(0 起，-1 表示无)；spctx 为空返回 nil</returns>
/// <returns type="string[]?">全部子协议名列表（1 起）；spctx 为空时同为 nil，返回值个数恒为 2
/// （wbsk.connect 明说了 spctx 可能为 nil，业务确实会走到这条）</returns>
static int32_t _lprot_websock_secprots(lua_State *lua) {
    if (!lua_islightuserdata(lua, 1)) {
        return lpub_rtn_nil(lua, 2);
    }
    ws_secprots_ctx *spctx = lua_touserdata(lua, 1);
    if (NULL == spctx) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushinteger(lua, spctx->index);
    lua_createtable(lua, spctx->cnt, 0);
    for (int32_t i = 0; i < spctx->cnt; i++) {
        lua_pushlstring(lua, spctx->prots[i].data, spctx->prots[i].lens);
        lua_rawseti(lua, -2, i + 1);
    }
    return 2;
}
//srey.websock
LUAMOD_API int luaopen_websock(lua_State *lua) {
    luaL_Reg reg[] = {
        { "unpack", _lprot_websock_unpack },
        { "frame", _lprot_websock_frame },
        { "pack_handshake", _lprot_websock_pack_handshake },
        { "pack_ping", _lprot_websock_pack_ping },
        { "pack_pong", _lprot_websock_pack_pong },
        { "pack_close", _lprot_websock_pack_close },
        { "pack_text", _lprot_websock_pack_text },
        { "pack_binary", _lprot_websock_pack_binary },
        { "pack_continua", _lprot_websock_pack_continua },
        { "secprots", _lprot_websock_secprots },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
/// <summary>
/// 将 HTTP 状态码转换为对应的文本描述（如 200 → "OK"）
/// </summary>
/// <param name="code" type="integer">HTTP 状态码</param>
/// <returns type="string">文本描述</returns>
static int32_t _lprot_http_code_status(lua_State *lua) {
    int32_t err = (int32_t)luaL_checkinteger(lua, 1);
    lua_pushstring(lua, http_code_status(err));
    return 1;
}
/// <summary>
/// 返回 HTTP 包的 Transfer-Encoding: chunked 状态。只认 Transfer-Encoding——响应既无
/// Content-Length 又无 TE 时(RFC 7230 §3.3.3 规则 7，body 由连接关闭界定)本值仍是 0，
/// 而 body 确实按分片投；判"还有没有后续"要看消息自带的 slice 标记，不能用本值
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="integer">0=非分块；1=首包（含 header）；2+ 分块中间/结束块</returns>
static int32_t _lprot_http_chunked(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    lua_pushinteger(lua, http_chunked(pack));
    return 1;
}
/// <summary>
/// 返回 HTTP 状态行 / 请求行三元组
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="string[]?">3 元数组：响应为 {version, code, message}，请求为 {method, uri, version}；分块中间包返回 nil</returns>
static int32_t _lprot_http_status(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    // 分块中间包没有首行，http_status 直接返 NULL（判据收在 C 侧一处，这里不再各判一遍 chunked）
    buf_ctx *buf = http_status(pack);
    if (NULL == buf) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_createtable(lua, 3, 0);
    for (int32_t i = 0; i < 3; i++) {
        lua_pushlstring(lua, buf[i].data, buf[i].lens);
        lua_rawseti(lua, -2, i + 1);
    }
    return 1;
}
/// <summary>
/// 按 key 查找 HTTP 头部字段值
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <param name="key" type="string">header 名（大小写不敏感）</param>
/// <returns type="string?">header 值；分块中间包或字段不存在时返回 nil</returns>
static int32_t _lprot_http_head(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    const char *key = luaL_checkstring(lua, 2);
    size_t vlens;
    char *val = http_header(pack, key, &vlens);
    if (NULL == val) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, val, vlens);
    return 1;
}
/// <summary>
/// 返回所有 HTTP 头部字段
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="table&lt;string,string&gt;?">key→value 表；分块中间包返回 nil</returns>
static int32_t _lprot_http_heads(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    // 分块中间包没有首行也没有头部。契约是返 nil（不是空表），故仍要单独判一次，
    // 用 http_status 是否为 NULL 作判据——与 http_nheader / http_header 收在 C 侧的是同一条
    if (NULL == http_status(pack)) {
        return lpub_rtn_nil(lua, 1);
    }
    uint32_t nhead = http_nheader(pack);
    lua_createtable(lua, 0, (int32_t)nhead);
    http_header_ctx *header;
    for (uint32_t i = 0; i < nhead; i++) {
        header = http_header_at(pack, i);
        lua_pushlstring(lua, header->key.data, header->key.lens);
        lua_pushlstring(lua, header->value.data, header->value.lens);
        lua_rawset(lua, -3);
    }
    return 1;
}
/// <summary>
/// 返回 HTTP body 数据指针和长度。
/// 注意与本文件各 pack_* 的返回值形状相同但语义相反：这里返回的是 pack 内部的**借用**指针，
/// 随 pack 一起失效，调用方既不拥有它、也不能对它调 utils.ud_free
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="lightuserdata?">body 数据指针（借用，勿释放）；空时返回 nil</returns>
/// <returns type="integer">body 字节数；空时为 0</returns>
static int32_t _lprot_http_data(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    size_t lens;
    void *data = http_data(pack, &lens);
    if (0 == lens) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    return lpub_rtn_lud(lua, data, lens);
}
/// <summary>
/// 以 Lua 字符串形式返回 HTTP body 内容
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="string?">body 内容；空时返回 nil</returns>
static int32_t _lprot_http_datastr(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    size_t lens;
    void *data = http_data(pack, &lens);
    if (0 == lens) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, data, lens);
    return 1;
}
// 取只认真字符串的入参，非字符串返 NULL。不能直接 lua_tolstring——它连数字也转
static const char *_lprot_str_arg(lua_State *lua, int32_t idx, size_t *lens) {
    *lens = 0;
    if (LUA_TSTRING != lua_type(lua, idx)) {
        return NULL;
    }
    return lua_tolstring(lua, idx, lens);
}
/// <summary>
/// 是否合法 RFC 7230 token（全部字符为 tchar）；HTTP 头名按此校验
/// </summary>
/// <param name="s" type="string">待判字符串；非字符串或空串返回 false</param>
/// <returns type="boolean">合法返回 true</returns>
static int32_t _lprot_http_is_token(lua_State *lua) {
    size_t lens;
    const char *s = _lprot_str_arg(lua, 1, &lens);
    return lpub_rtn_bool(lua, NULL != s && 0 != is_token(s, lens));
}
/// <summary>
/// 判一段头值能否进线格式，规则见 C 层 http_head_val_ok。头名按 is_token 判
/// </summary>
/// <param name="s" type="string">待判头值；非字符串返回 false</param>
/// <returns type="boolean">可以进线格式返回 true</returns>
static int32_t _lprot_http_head_val_ok(lua_State *lua) {
    size_t lens;
    const char *s = _lprot_str_arg(lua, 1, &lens);
    return lpub_rtn_bool(lua, NULL != s && 0 != http_head_val_ok(s, lens));
}
// 头名是否等于 name(name 为小写字面量)，大小写无关
static int32_t _lprot_key_is(const char *key, size_t klens, const char *name, size_t nlens) {
    return klens == nlens && 0 == memcasecmp(key, name, nlens);
}
static int32_t _lprot_head_fail(lua_State *lua, http_head_rc rc) {
    lua_pushnil(lua);
    lua_pushinteger(lua, rc);
    return 2;
}
/// <summary>
/// 一次判完一条附加头能否进报文，依次判：头名是 token、值是 string 或 number、不是组包函数
/// 自己生成的头、值不含 NUL/CRLF，第一条不过即停。number 值的写法同 num_str（整数值的浮点按整数写）
/// </summary>
/// <param name="key" type="any">头名；非字符串按头名不合法算</param>
/// <param name="val" type="any">头值</param>
/// <param name="autoflags" type="integer">组包函数自己写的头：0x01 Content-Type，0x02 Content-Length 与 Transfer-Encoding</param>
/// <returns type="string?">可直接写进报文的头值；没通过时为 nil</returns>
/// <returns type="integer">0 通过；1 头名不是 token；2 值不是 string/number；3 是组包函数自己生成的头；4 值含 NUL/CRLF</returns>
static int32_t _lprot_http_head_check(lua_State *lua) {
    int32_t flags = (int32_t)luaL_checkinteger(lua, 3);
    size_t klens;
    const char *key = _lprot_str_arg(lua, 1, &klens);
    if (NULL == key || 0 == is_token(key, klens)) {
        return _lprot_head_fail(lua, HTTP_HEAD_BADKEY);
    }
    int32_t vt = lua_type(lua, 2);
    if (LUA_TSTRING != vt && LUA_TNUMBER != vt) {
        return _lprot_head_fail(lua, HTTP_HEAD_BADTYPE);
    }
    if (((HTTP_AUTO_CT & flags) && _lprot_key_is(key, klens, "content-type", sizeof("content-type") - 1))
        || ((HTTP_AUTO_FRAME & flags)
            && (_lprot_key_is(key, klens, "content-length", sizeof("content-length") - 1)
                || _lprot_key_is(key, klens, "transfer-encoding", sizeof("transfer-encoding") - 1)))) {
        return _lprot_head_fail(lua, HTTP_HEAD_AUTO);
    }
    int32_t isint;
    lua_Integer iv;
    if (LUA_TNUMBER == vt) {
        iv = lua_tointegerx(lua, 2, &isint);
        if (isint) {
            lua_pushinteger(lua, iv);
            lua_replace(lua, 2);
        }
    }
    size_t vlens;
    const char *val = luaL_tolstring(lua, 2, &vlens);
    if (0 == http_head_val_ok(val, vlens)) {
        return _lprot_head_fail(lua, HTTP_HEAD_BADVAL);
    }
    lua_pushinteger(lua, HTTP_HEAD_OK);
    return 2;
}
// 头表预检：每条都得是 token 名 + 合法 string 值、且不是帧长头，顺带累计写出长度。
// 有一条不过就整体退回脚本侧原路（告警与丢弃在那边做），这里不做半截处理
static int32_t _lprot_resp_heads_check(lua_State *lua, int32_t idx, size_t *hlens) {
    size_t klens, vlens;
    const char *key;
    const char *val;
    lua_pushnil(lua);
    while (0 != lua_next(lua, idx)) {
        if (LUA_TSTRING != lua_type(lua, -2)
            || LUA_TSTRING != lua_type(lua, -1)) {
            lua_pop(lua, 2);
            return ERR_FAILED;
        }
        key = lua_tolstring(lua, -2, &klens);
        val = lua_tolstring(lua, -1, &vlens);
        if (0 == is_token(key, klens)
            || _lprot_key_is(key, klens, "content-length", sizeof("content-length") - 1)
            || _lprot_key_is(key, klens, "transfer-encoding", sizeof("transfer-encoding") - 1)
            || 0 == http_head_val_ok(val, vlens)) {
            lua_pop(lua, 2);
            return ERR_FAILED;
        }
        *hlens += klens + vlens + 4;
        lua_pop(lua, 1);
    }
    return ERR_OK;
}
// 栈位 idx..idx+4 依次是 code、headers、body、headonly、block。只接常见形状：[100, 999] 的整数码、
// nil 或 string 报文体、没有头表时的 table 报文体（1xx/204/304 不看报文体）、无元表的头表、nil 或 string 头部块；
// 其余返回 ERR_FAILED 交回脚本侧原路。带头表的 table 体不接：业务传的 Content-Type 要在原路被丢掉
static int32_t _lprot_resp_check(lua_State *lua, int32_t idx, http_resp_plan *plan) {
    if (!lua_isinteger(lua, idx)) {
        return ERR_FAILED;
    }
    lua_Integer code = lua_tointeger(lua, idx);
    if (code < 100 || code > 999) {
        return ERR_FAILED;
    }
    plan->code = (int32_t)code;
    plan->nobody = http_code_nobody(plan->code);
    plan->headonly = lua_toboolean(lua, idx + 3);
    plan->body = NULL;
    plan->blens = 0;
    plan->json = 0;
    if (!plan->nobody) {
        int32_t bt = lua_type(lua, idx + 2);
        if (LUA_TSTRING == bt) {
            plan->body = lua_tolstring(lua, idx + 2, &plan->blens);
        } else if (LUA_TTABLE == bt
                   && lua_isnoneornil(lua, idx + 1)) {
            plan->json = 1;
        } else if (LUA_TNIL != bt && LUA_TNONE != bt) {
            return ERR_FAILED;
        }
    }
    plan->block = NULL;
    plan->bklens = 0;
    int32_t kt = lua_type(lua, idx + 4);
    if (LUA_TSTRING == kt) {
        plan->block = lua_tolstring(lua, idx + 4, &plan->bklens);
    } else if (LUA_TNIL != kt && LUA_TNONE != kt) {
        return ERR_FAILED;
    }
    plan->hidx = 0;
    plan->hlens = 0;
    int32_t ht = lua_type(lua, idx + 1);
    if (LUA_TTABLE == ht) {
        // 带元表的表 pairs 可能走 __pairs，遍历顺序与内容都不归这里管
        if (lua_getmetatable(lua, idx + 1)) {
            lua_pop(lua, 1);
            return ERR_FAILED;
        }
        if (ERR_OK != _lprot_resp_heads_check(lua, idx + 1, &plan->hlens)) {
            return ERR_FAILED;
        }
        plan->hidx = idx + 1;
    } else if (LUA_TNIL != ht && LUA_TNONE != ht) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 照预检结果组包。遍历顺序与预检、与脚本侧 pairs 都相同；这里不再抛错
static void _lprot_resp_write(lua_State *lua, http_resp_plan *plan, binary_ctx *bw) {
    size_t klens, vlens;
    const char *key;
    const char *val;
    binary_init_write(bw, HTTP_RESP_RESERVE + plan->bklens + plan->hlens + (plan->headonly ? 0 : plan->blens), 0);
    http_pack_resp(bw, plan->code);
    if (NULL != plan->block) {
        binary_set_binary(bw, plan->block, plan->bklens);
    }
    if (0 != plan->hidx) {
        lua_pushnil(lua);
        while (0 != lua_next(lua, plan->hidx)) {
            key = lua_tolstring(lua, -2, &klens);
            val = lua_tolstring(lua, -1, &vlens);
            binary_set_binary(bw, key, klens);
            binary_set_binary(bw, ": ", sizeof(": ") - 1);
            binary_set_binary(bw, val, vlens);
            binary_set_binary(bw, FLAG_CRLF, CRLF_SIZE);
            lua_pop(lua, 1);
        }
    }
    if (plan->json) {
        binary_set_binary(bw, HTTP_JSON_CT, sizeof(HTTP_JSON_CT) - 1);
    }
    if (plan->nobody) {
        http_pack_end(bw);
    } else if (plan->headonly) {
        binary_set_binary(bw, "Content-Length: ", sizeof("Content-Length: ") - 1);
        binary_set_uint(bw, (uint64_t)plan->blens, 10);
        binary_set_binary(bw, FLAG_CRLF, CRLF_SIZE);
        http_pack_end(bw);
    } else {
        http_pack_content(bw, (void *)plan->body, plan->blens);
    }
}
// lyyjson_encode_sink 的回调：拿到 JSON 后照预检结果组包
static void _lprot_resp_json_sink(void *ud, const char *json, size_t lens) {
    http_resp_json *arg = ud;
    arg->plan->body = json;
    arg->plan->blens = lens;
    _lprot_resp_write(arg->lua, arg->plan, arg->bw);
}
// 照预检结果组包；table 报文体先编成 JSON，编不出来返回 ERR_FAILED（bw 未分配），交回脚本侧原路去报错
static int32_t _lprot_resp_pack(lua_State *lua, int32_t idx, http_resp_plan *plan, binary_ctx *bw) {
    if (!plan->json) {
        _lprot_resp_write(lua, plan, bw);
        return ERR_OK;
    }
    http_resp_json arg;
    arg.lua = lua;
    arg.plan = plan;
    arg.bw = bw;
    return lyyjson_encode_sink(lua, idx + 2, _lprot_resp_json_sink, &arg);
}
/// <summary>
/// 服务端 HTTP 响应整条在 C 里组包并直接发出，字节与 lib/http 的 http.response / response_head 一致。
/// 只接常见形状：[100, 999] 的整数码、nil 或 string 报文体、没有头表时的 table 报文体（编成 JSON 并带
/// Content-Type: application/json，同 lib/http；1xx/204/304 不看报文体）、
/// 无元表且每条都合法（token 名、string 值、无 NUL/CRLF、不是 Content-Length / Transfer-Encoding）的头表；
/// 其余一律不碰，交给调用方走原来的组包路径（告警与丢弃都在那边）
/// </summary>
/// <param name="sk" type="userdata">连接标识</param>
/// <param name="code" type="integer">状态码</param>
/// <param name="headers" type="table&lt;string,string&gt;?">附加头部</param>
/// <param name="body" type="string|table&lt;any,any&gt;?">报文体</param>
/// <param name="headonly" type="boolean?">回 HEAD 请求：Content-Length 写真实长度，不发报文体</param>
/// <param name="block" type="string?">预渲染好的头部块（每行已含 \r\n），原样写在状态行后、不做校验</param>
/// <returns type="boolean">true 已组包并投递（发送失败不回报，同 http.response）；
/// false 形状不归它管、sk 不是连接标识或 table 报文体编不成 JSON，什么都没做</returns>
static int32_t _lprot_http_respond(lua_State *lua) {
    http_resp_plan plan;
    sock_ctx *sk = lpub_is_sock(lua, 1);
    if (NULL == sk
        || ERR_OK != _lprot_resp_check(lua, 2, &plan)) {
        return lpub_rtn_bool(lua, 0);
    }
    binary_ctx bw;
    if (ERR_OK != _lprot_resp_pack(lua, 2, &plan, &bw)) {
        return lpub_rtn_bool(lua, 0);
    }
    ev_send(&g_loader->netev, sk, bw.data, bw.offset, 0);
    return lpub_rtn_bool(lua, 1);
}
// 外部字符串的释放回调：缓冲来自 binary_init_write
static void *_lprot_ext_free(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    (void)nsize;
    FREE(ptr);
    return NULL;
}
/// <summary>
/// 同 respond 的组包，但不发送，把整条报文作为字符串返回；单测断言组包字节用
/// </summary>
/// <param name="code" type="integer">状态码</param>
/// <param name="headers" type="table&lt;string,string&gt;?">附加头部</param>
/// <param name="body" type="string|table&lt;any,any&gt;?">报文体，同 respond</param>
/// <param name="headonly" type="boolean?">同 respond</param>
/// <param name="block" type="string?">同 respond</param>
/// <returns type="string?">整条报文；形状不归它管（同 respond 返回 false 的情形）时为 nil</returns>
static int32_t _lprot_http_pack_resp(lua_State *lua) {
    http_resp_plan plan;
    binary_ctx bw;
    if (ERR_OK != _lprot_resp_check(lua, 1, &plan)
        || ERR_OK != _lprot_resp_pack(lua, 1, &plan, &bw)) {
        return lpub_rtn_nil(lua, 1);
    }
    // 直接把缓冲交给 Lua 当字符串：失败时由 Lua 调回调释放，不会漏
    bw.data[bw.offset] = '\0';
    lua_pushexternalstring(lua, bw.data, bw.offset, _lprot_ext_free, NULL);
    return 1;
}
//srey.http
LUAMOD_API int luaopen_http(lua_State *lua) {
    luaL_Reg reg[] = {
        { "code_status", _lprot_http_code_status },
        { "chunked", _lprot_http_chunked },
        { "status", _lprot_http_status },
        { "head", _lprot_http_head },
        { "heads", _lprot_http_heads },
        { "data", _lprot_http_data },
        { "datastr", _lprot_http_datastr },
        { "is_token", _lprot_http_is_token },
        { "head_val_ok", _lprot_http_head_val_ok },
        { "head_check", _lprot_http_head_check },
        { "respond", _lprot_http_respond },
        { "pack_resp", _lprot_http_pack_resp },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    /// <field name="max_headlens" type="integer">HTTP 接收侧的头部块字节上限，取自 prots_pub.h
    /// 的 HTTP_MAX_HEADLENS；组包侧不按它判，要卡长度的调用方自己拿它比</field>
    lua_pushinteger(lua, (lua_Integer)HTTP_MAX_HEADLENS);
    lua_setfield(lua, -2, "max_headlens");
    return 1;
}
// 内部辅助：构造 Redis 聚合类型（array/set/map/push/attr）的空容器表。
// 元素随后由上层追加进这张表：array/set/push 落数组部分，map/attr 按 key 落哈希部分，
// 故按 ismap 分开预分配，省掉从 0 起逐次翻倍的 rehash。extra 是额外留给哨兵字段的哈希槽。
// 预分配量卡 REDIS_PREALLOC_MAX：nelem 是对端声明的数字，超出的部分照常增长
static void _lprot_redis_agg(lua_State *lua, int64_t nelem, int32_t ismap, int32_t extra) {
    int32_t pre = 0;
    if (nelem > 0) {
        pre = (int32_t)(nelem > REDIS_PREALLOC_MAX ? REDIS_PREALLOC_MAX : nelem);
    }
    lua_createtable(lua, 0 != ismap ? 0 : pre, (0 != ismap ? pre : 0) + extra);
}
// 压一个节点的值：标量原样压，聚合压空容器（见 _lprot_redis_agg）。返回聚合类型名，标量返回 NULL
static const char *_lprot_redis_push(lua_State *lua, redis_pack_ctx *pk, int32_t extra) {
    switch (pk->prot) {
    case RESP_STRING:// 简单字符串
    case RESP_ERROR:// 错误字符串
    case RESP_BSTRING:// 批量字符串
    case RESP_BERROR:// 批量错误
    case RESP_VERB:// 带类型的字符串
        if (pk->len < 0) {
            lua_pushnil(lua);
        } else if (0 == pk->len) {
            lua_pushstring(lua, "");
        } else {
            lua_pushlstring(lua, pk->data, (size_t)pk->len);
        }
        return NULL;
    case RESP_INTEGER:// 整数
        lua_pushinteger(lua, pk->ival);
        return NULL;
    case RESP_BIGNUM:// 大整数(任意精度,以字符串返回)
        lua_pushlstring(lua, pk->data, (size_t)pk->len);
        return NULL;
    case RESP_BOOL:// 布尔值
        lua_pushboolean(lua, (int32_t)pk->ival);
        return NULL;
    case RESP_DOUBLE:// 浮点数
        lua_pushnumber(lua, pk->dval);
        return NULL;
    case RESP_ARRAY:// 数组
        _lprot_redis_agg(lua, pk->nelem, 0, extra);
        return "array";
    case RESP_SET:// 集合
        _lprot_redis_agg(lua, pk->nelem, 0, extra);
        return "set";
    case RESP_PUSHE:// 推送消息
        _lprot_redis_agg(lua, pk->nelem, 0, extra);
        return "push";
    case RESP_MAP:// 映射
        _lprot_redis_agg(lua, pk->nelem, 1, extra);
        return "map";
    case RESP_ATTR:// 属性
        _lprot_redis_agg(lua, pk->nelem, 1, extra);
        return "attr";
    case RESP_NIL:// Null 值
    default:
        lua_pushnil(lua);
        return NULL;
    }
}
/// <summary>
/// 解析一个 Redis RESP 节点的值
/// </summary>
/// <param name="pk" type="lightuserdata?">redis_pack_ctx 节点指针；nil 时返回 nil</param>
/// <returns type="string|integer|number|boolean|nil|RedisAggValue">标量直接返回；聚合类型（array/set/map/push/attr）返回 RedisAggValue</returns>
static int32_t _lprot_redis_value(lua_State *lua) {
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type || LUA_TNONE == type) {
        return lpub_rtn_nil(lua, 1);
    }
    LUACHECK_LUDATA_OPT(lua, 1);
    redis_pack_ctx *pk = lua_touserdata(lua, 1);
    if (NULL == pk) {
        return lpub_rtn_nil(lua, 1);
    }
    const char *kind = _lprot_redis_push(lua, pk, 2);
    if (NULL != kind) {
        lua_pushstring(lua, kind);
        lua_setfield(lua, -2, "resp_type");
        lua_pushinteger(lua, pk->nelem);
        lua_setfield(lua, -2, "resp_nelem");
    }
    return 1;
}
/// <summary>
/// 一次取齐一个 Redis RESP 节点：值、聚合类型、元素计数、下一节点。聚合返回的是不带
/// resp_type / resp_nelem 哨兵的空容器，类型与计数从第 2、3 个返回值拿
/// </summary>
/// <param name="pk" type="lightuserdata?">redis_pack_ctx 节点指针；nil 时 4 个返回值全是 nil</param>
/// <returns type="string|integer|number|boolean|RedisAggPayload|nil">节点值；聚合为按 nelem 预分配的空表</returns>
/// <returns type="string?">聚合类型名 array/set/push/map/attr；标量为 nil</returns>
/// <returns type="integer?">聚合元素计数（-1 为 RESP3 的 nil 聚合）；标量为 nil</returns>
/// <returns type="lightuserdata?">下一个节点指针；没有后续节点为 nil</returns>
static int32_t _lprot_redis_node(lua_State *lua) {
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type || LUA_TNONE == type) {
        return lpub_rtn_nil(lua, 4);
    }
    LUACHECK_LUDATA_OPT(lua, 1);
    redis_pack_ctx *pk = lua_touserdata(lua, 1);
    if (NULL == pk) {
        return lpub_rtn_nil(lua, 4);
    }
    const char *kind = _lprot_redis_push(lua, pk, 0);
    if (NULL == kind) {
        lua_pushnil(lua);
        lua_pushnil(lua);
    } else {
        lua_pushstring(lua, kind);
        lua_pushinteger(lua, pk->nelem);
    }
    if (NULL == pk->next) {
        lua_pushnil(lua);
    } else {
        lua_pushlightuserdata(lua, pk->next);
    }
    return 4;
}
/// <summary>
/// 获取 Redis RESP 链表中下一个节点指针
/// </summary>
/// <param name="pk" type="lightuserdata?">redis_pack_ctx 节点指针；nil 时返回 nil</param>
/// <returns type="lightuserdata?">下一个节点指针；无后续节点返回 nil</returns>
static int32_t _lprot_redis_next(lua_State *lua) {
    int32_t type = lua_type(lua, 1);
    if (LUA_TNIL == type || LUA_TNONE == type) {
        return lpub_rtn_nil(lua, 1);
    }
    LUACHECK_LUDATA_OPT(lua, 1);
    redis_pack_ctx *pk = lua_touserdata(lua, 1);
    if (NULL == pk
        || NULL == pk->next) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlightuserdata(lua, pk->next);
    return 1;
}
/// <summary>
/// 将命令及参数编成 RESP 请求（array of bulk string）。参数按 tostring 转串，数字照 num_str 的规则：
/// 整数值的浮点按整数写，其余浮点同 tostring；nil 编成空 bulk 并打一条告警
/// </summary>
/// <param name="..." type="any">命令名及参数，如 redis.pack("SET", "key", "value")</param>
/// <returns type="string">RESP 编码后的请求字符串</returns>
static int32_t _lprot_redis_pack(lua_State *lua) {
    int32_t n = lua_gettop(lua);
    int32_t i;
    int32_t type;
    int32_t isint;
    lua_Integer iv;
    size_t lens;
    size_t k;
    const char *s;
    char *p;
    char nbuf[LUA_N2SBUFFSZ];
    luaL_Buffer lbuf;
    // 数字、字符串、nil 之外的参数先就地换成 tostring 的结果：luaL_tolstring 要压栈，不能夹在缓冲写入中间
    for (i = 1; i <= n; i++) {
        type = lua_type(lua, i);
        if (LUA_TNIL != type && LUA_TNUMBER != type && LUA_TSTRING != type) {
            luaL_tolstring(lua, i, NULL);
            lua_replace(lua, i);
        }
    }
    luaL_buffinit(lua, &lbuf);
    p = luaL_prepbuffsize(&lbuf, 24);
    p[0] = '*';
    k = 1 + u64tostr(p + 1, (uint64_t)n, 10);
    p[k++] = '\r';
    p[k++] = '\n';
    luaL_addsize(&lbuf, k);
    for (i = 1; i <= n; i++) {
        if (lua_isnil(lua, i)) {
            LOG_WARN("redis.pack: nil argument #%d, encoded as empty bulk string", i);
            luaL_addlstring(&lbuf, "$0\r\n\r\n", 6);
            continue;
        }
        if (LUA_TNUMBER == lua_type(lua, i)) {
            iv = lua_tointegerx(lua, i, &isint);
            if (isint) {
                lens = i64tostr(nbuf, (int64_t)iv, 10);
            } else {
                lens = (size_t)lua_numbertocstring(lua, i, nbuf) - 1;
            }
            s = nbuf;
        } else {
            s = lua_tolstring(lua, i, &lens);
        }
        // "$" + 最多 20 位长度 + 两个 CRLF
        p = luaL_prepbuffsize(&lbuf, lens + 25);
        p[0] = '$';
        k = 1 + u64tostr(p + 1, (uint64_t)lens, 10);
        p[k++] = '\r';
        p[k++] = '\n';
        memcpy(p + k, s, lens);
        k += lens;
        p[k++] = '\r';
        p[k++] = '\n';
        luaL_addsize(&lbuf, k);
    }
    luaL_pushresult(&lbuf);
    return 1;
}
//srey.redis
LUAMOD_API int luaopen_redis(lua_State *lua) {
    luaL_Reg reg[] = {
        { "value", _lprot_redis_value },
        { "next", _lprot_redis_next },
        { "node", _lprot_redis_node },
        { "pack", _lprot_redis_pack },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    return 1;
}
//srey.smtp
/// <summary>
/// 创建 SMTP 客户端上下文（不立即建立连接）
/// </summary>
/// <param name="ip" type="string">SMTP 服务器 IP</param>
/// <param name="port" type="integer">SMTP 服务器端口</param>
/// <param name="evssl" type="lightuserdata|nil">SSL 上下文；nil 表示明文</param>
/// <param name="user" type="string">认证用户名，最长 63 字节</param>
/// <param name="psw" type="string">认证密码，最长 63 字节</param>
/// <returns type="_smtp_ctx?">SMTP 对象；ip / user / psw 任一超长返回 nil
/// （同 mysql.new / pgsql.new / mongo.new，不静默截断）</returns>
static int32_t _lprot_smtp_new(lua_State *lua) {
    const char *ip = luaL_checkstring(lua, 1);
    uint16_t port = lpub_check_u16(lua, 2, PORT_OUT_OF_RANGE);
    struct evssl_ctx *evssl = lpub_check_evssl(lua, 3);
    const char *user = luaL_checkstring(lua, 4);
    const char *psw = luaL_checkstring(lua, 5);
    smtp_ctx **ud = (smtp_ctx **)lpub_push_ud(lua, NULL, MT_SMTP);
    smtp_ctx *smtp;
    MALLOC(smtp, sizeof(smtp_ctx));
    if (ERR_OK != smtp_init(smtp, ip, port, evssl, user, psw)) {
        FREE(smtp);
        return lpub_rtn_nil(lua, 1);
    }
    ATOMIC_SET_RELAXED(&smtp->ref, 1);// Lua 持有者份额
    *ud = smtp;
    return 1;
}
/// <summary>
/// 发送 QUIT 命令并清理 SMTP 连接上下文（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns>无</returns>
static int32_t _lprot_smtp_free(lua_State *lua) {
    smtp_ctx **ud = luaL_checkudata(lua, 1, MT_SMTP);
    smtp_ctx *smtp = *ud;
    if (NULL == smtp) {
        return 0;
    }
    if (NULL != smtp->task && !sock_is_invalid(&smtp->sk)) {
        char *cmd = smtp_pack_quit();
        ev_send(&smtp->task->loader->netev, &smtp->sk, cmd, strlen(cmd), 0);
        // 主动关连接：触发该 socket 的 udfree 释放事件侧份额，否则弃用的活连接块滞留至对端关
        ev_close(&smtp->task->loader->netev, &smtp->sk);
    }
    *ud = NULL;
    // 密码不在这里擦：ev_close 只是投命令，网络线程可能正读着它组认证串。
    // 擦除已挪进 PROT_REF_RELEASE，由最后一个放手的线程整块抹掉
    PROT_REF_RELEASE(smtp);
    return 0;
}
/// <summary>
/// 返回 SMTP 连接的 fd 和 skid
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="userdata">连接标识；失败时其 valid 字段为 false</returns>
static int32_t _lprot_smtp_sock_id(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    smtp_ctx *smtp = *ud;
    lpub_push_sock_slot(lua, 1, &smtp->sk);
    return 1;
}
/// <summary>
/// 尝试建立 SMTP 连接（异步）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="boolean">发起成功 true，失败 false</returns>
static int32_t _lprot_smtp_try_connect(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    LPUB_CUR_TASK(lua, task);
    int32_t rtn = smtp_try_connect(task, *ud, 1);
    return lpub_rtn_bool(lua, ERR_OK == rtn);
}
/// <summary>
/// 检查 SMTP 响应包的状态码是否匹配指定 code
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="pack" type="lightuserdata">SMTP 响应包指针</param>
/// <param name="code" type="string">期望状态码（如 "220"、"250"）</param>
/// <returns type="boolean">匹配 true，否则 false</returns>
static int32_t _lprot_smtp_check_code(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    LPUB_LUD_ARG(lua, char, 2, pack);
    const char *code = luaL_checkstring(lua, 3);
    return lpub_rtn_bool(lua, ERR_OK == smtp_check_code(pack, code));
}
/// <summary>
/// 检查 SMTP 响应包的状态码是否命中 codes 里任意一个。用于一条命令有多个合法应答的场合，
/// 如 RCPT TO 的 250(已接受) 与 251(已接受但将转发)。全不中才落一条告警
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="pack" type="lightuserdata">SMTP 响应包指针</param>
/// <param name="codes" type="string[]">期望状态码数组，如 { "250", "251" }；空表恒返 false</param>
/// <returns type="boolean">命中其中之一 true，否则 false</returns>
static int32_t _lprot_smtp_check_codes(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    LPUB_LUD_ARG(lua, char, 2, pack);
    luaL_checktype(lua, 3, LUA_TTABLE);
    lua_Integer ncode = (lua_Integer)lua_rawlen(lua, 3);
    luaL_argcheck(lua, ncode >= 0 && ncode <= SMTP_MAX_NCODE, 3, "too many codes");
    const char *codes[SMTP_MAX_NCODE];
    for (lua_Integer i = 0; i < ncode; i++) {
        lua_rawgeti(lua, 3, i + 1);
        // 必须已经是字符串:luaL_checkstring 会把数字就地转成串,而那种串只由栈槽锚定,
        // pop 之后可能被回收,codes[i] 就成了悬空指针。真字符串由表持有,pop 后仍有效
        luaL_argcheck(lua, LUA_TSTRING == lua_type(lua, -1), 3, "codes must be strings");
        codes[i] = lua_tostring(lua, -1);
        lua_pop(lua, 1);
    }
    return lpub_rtn_bool(lua, ERR_OK == smtp_check_codes(pack, codes, (size_t)ncode));
}
/// <summary>
/// 检查 SMTP 响应包的应答码是否为 250。只认这一个码，不是判整个 2xx 段——
/// 251/252 这类同样表示"已接受"的应答会被判失败，要认它们得用 check_codes
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="pack" type="lightuserdata">SMTP 响应包指针</param>
/// <returns type="boolean">应答码为 250 返回 true，否则 false</returns>
static int32_t _lprot_smtp_check_ok(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    LPUB_LUD_ARG(lua, char, 2, pack);
    return lpub_rtn_bool(lua, ERR_OK == smtp_check_ok(pack));
}
/// <summary>
/// 构造 SMTP RSET 重置命令
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_smtp_pack_reset(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    char *cmd = smtp_pack_reset();
    return lpub_rtn_lud(lua, (void *)cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP MAIL FROM 命令；地址走 CRLF 注入防御
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="from" type="string">发件人邮箱地址</param>
/// <returns type="lightuserdata?">命令数据指针；地址含 CRLF 时返回 nil</returns>
/// <returns type="integer">数据长度；地址含 CRLF 时为 0</returns>
static int32_t _lprot_smtp_pack_from(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    const char *from = luaL_checkstring(lua, 2);
    char *cmd = smtp_pack_from(from);
    if (NULL == cmd) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    return lpub_rtn_lud(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP RCPT TO 命令；地址走 CRLF 注入防御
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <param name="rcpt" type="string">收件人邮箱地址</param>
/// <returns type="lightuserdata?">命令数据指针；地址含 CRLF 时返回 nil</returns>
/// <returns type="integer">数据长度；地址含 CRLF 时为 0</returns>
static int32_t _lprot_smtp_pack_rcpt(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    const char *rcpt = luaL_checkstring(lua, 2);
    char *cmd = smtp_pack_rcpt(rcpt);
    if (NULL == cmd) {
        lua_pushnil(lua);
        lua_pushinteger(lua, 0);
        return 2;
    }
    return lpub_rtn_lud(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP DATA 命令（开始传输邮件内容）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_smtp_pack_data(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    char *cmd = smtp_pack_data();
    return lpub_rtn_lud(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP QUIT 断连命令
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_smtp_pack_quit(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    char *cmd = smtp_pack_quit();
    return lpub_rtn_lud(lua, cmd, strlen(cmd));
}
/// <summary>
/// 构造 SMTP NOOP 心跳命令（保持连接）
/// </summary>
/// <param name="self" type="userdata">SMTP 对象</param>
/// <returns type="lightuserdata">命令数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lprot_smtp_pack_ping(lua_State *lua) {
    LPUB_UD_ARG(lua, smtp_ctx, MT_SMTP, ud, "smtp freed");
    char *cmd = smtp_pack_ping();
    return lpub_rtn_lud(lua, cmd, strlen(cmd));
}
LUAMOD_API int luaopen_smtp(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lprot_smtp_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "try_connect", _lprot_smtp_try_connect },
        { "check_code",_lprot_smtp_check_code },
        { "check_codes",_lprot_smtp_check_codes },
        { "check_ok", _lprot_smtp_check_ok },
        { "pack_reset", _lprot_smtp_pack_reset },
        { "pack_from", _lprot_smtp_pack_from },
        { "pack_rcpt", _lprot_smtp_pack_rcpt },
        { "pack_data", _lprot_smtp_pack_data },
        { "pack_quit", _lprot_smtp_pack_quit },
        { "pack_ping", _lprot_smtp_pack_ping },
        { "sock_id", _lprot_smtp_sock_id },
        { "__gc", _lprot_smtp_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_SMTP, reg_new, reg_func);
    return 1;
}
//srey.smtp.mail
/// <summary>
/// 创建邮件上下文，用于组装邮件内容
/// </summary>
/// <param>无</param>
/// <returns type="_smtp_mail_ctx">邮件对象</returns>
static int32_t _lprot_mail_new(lua_State *lua) {
    mail_ctx **ud = (mail_ctx **)lpub_push_ud(lua, NULL, MT_SMTP_MAIL);
    mail_ctx *mail;
    MALLOC(mail, sizeof(mail_ctx));
    mail_init(mail);
    *ud = mail;
    return 1;
}
/// <summary>
/// 释放邮件上下文内部资源（绑定为 __gc，由 Lua GC 自动调用）；重复调用安全
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lprot_mail_free(lua_State *lua) {
    mail_ctx **ud = luaL_checkudata(lua, 1, MT_SMTP_MAIL);
    if (NULL == *ud) {
        return 0;
    }
    mail_free(*ud);
    FREE(*ud);
    return 0;
}
/// <summary>
/// 设置邮件是否需要回执
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="reply" type="integer?">0 不需要，其他值请求回执；nil 视为 0</param>
/// <returns>无</returns>
static int32_t _lprot_mail_reply(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    // 判 isnoneornil 而不是只判 LUA_TNIL:文档写的是"nil 视为 0",而 mail:reply() 不带参数时
    // 类型是 LUA_TNONE,只判 TNIL 会落到 luaL_checkinteger 上报"number expected, got no value"
    int32_t reply = lua_isnoneornil(lua, 2) ? 0 : (0 != luaL_checkinteger(lua, 2));
    mail_reply(*ud, reply);
    return 0;
}
/// <summary>
/// 设置邮件主题
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="subject" type="string">邮件主题</param>
/// <returns>无</returns>
static int32_t _lprot_mail_subject(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *subject = luaL_checkstring(lua, 2);
    mail_subject(*ud, subject);
    return 0;
}
/// <summary>
/// 设置邮件纯文本正文内容
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="msg" type="string">纯文本正文</param>
/// <returns>无</returns>
static int32_t _lprot_mail_msg(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *msg = luaL_checkstring(lua, 2);
    mail_msg(*ud, msg);
    return 0;
}
/// <summary>
/// 设置邮件 HTML 正文内容
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="html" type="string">HTML 正文</param>
/// <returns>无</returns>
static int32_t _lprot_mail_html(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *html = luaL_checkstring(lua, 2);
    mail_html(*ud, html, strlen(html));
    return 0;
}
/// <summary>
/// 设置发件人姓名和邮箱地址
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="name" type="string">发件人显示名</param>
/// <param name="email" type="string">发件人邮箱地址</param>
/// <returns>无</returns>
static int32_t _lprot_mail_from(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *name = luaL_checkstring(lua, 2);
    const char *email = luaL_checkstring(lua, 3);
    mail_from(*ud, name, email);
    return 0;
}
/// <summary>
/// 取发件人邮箱地址
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns type="string">发件人邮箱；没设过发件人时为空串。交出的是存量值——CRLF 已被剔除、
///   超出 mail_addr.addr 容量的部分已被截掉，与调 from 时传进来的原串可能不同</returns>
static int32_t _lprot_mail_from_get(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    lua_pushstring(lua, (*ud)->from.addr);
    return 1;
}
/// <summary>
/// 添加收件人地址
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="email" type="string">收件人邮箱</param>
/// <param name="type" type="integer">收件人类型，取值 [TO, BCC]（1/2/3），越界报错</param>
/// <returns>无</returns>
static int32_t _lprot_mail_addrs_add(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *email = luaL_checkstring(lua, 2);
    mail_addr_type type = (mail_addr_type)lpub_check_range(lua, 3, TO, BCC, "mail address type out of range (TO/CC/BCC)");
    mail_addrs_add(*ud, email, type);
    return 0;
}
/// <summary>
/// 取全部收件人邮箱，含 TO / CC / BCC，按加入顺序
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns type="string[]">收件人邮箱数组；一个都没加时为空表。每个元素的口径同 from_get</returns>
static int32_t _lprot_mail_addrs_get(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    uint32_t n = maddr_arr_size(&(*ud)->addrs);
    lua_createtable(lua, (int32_t)n, 0);
    for (uint32_t i = 0; i < n; i++) {
        lua_pushstring(lua, maddr_arr_at(&(*ud)->addrs, (int32_t)i)->addr);
        lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
}
/// <summary>
/// 清空所有收件人列表
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lprot_mail_addrs_clear(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    mail_addrs_clear(*ud);
    return 0;
}
/// <summary>
/// 添加附件文件
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <param name="file" type="string">附件文件路径</param>
/// <returns>无</returns>
static int32_t _lprot_mail_attach_add(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    const char *file = luaL_checkstring(lua, 2);
    mail_attach_add(*ud, file);
    return 0;
}
/// <summary>
/// 清空所有附件列表
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lprot_mail_attach_clear(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    mail_attach_clear(*ud);
    return 0;
}
/// <summary>
/// 清空邮件上下文：内容（主题 / 正文 / 收件人 / 附件）与 reply 标志一并还原到 mail_init 后的状态。
/// 注意 reply 会被复位为 1，同一对象复用时若需 No-Reply 须在每次 clear 后重新调 reply(0)
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns>无</returns>
static int32_t _lprot_mail_clear(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    mail_clear(*ud);
    return 0;
}
/// <summary>
/// 将邮件上下文序列化为 MIME 格式内容字符串
/// </summary>
/// <param name="self" type="userdata">邮件对象</param>
/// <returns type="lightuserdata?">MIME 字符串指针；生成 MIME boundary 取不到熵时返回 nil</returns>
/// <returns type="integer?">字符串长度；失败时两个返回值都是 nil，个数恒为 2</returns>
static int32_t _lprot_mail_pack(lua_State *lua) {
    LPUB_UD_ARG(lua, mail_ctx, MT_SMTP_MAIL, ud, "mail already freed");
    char *content = mail_pack(*ud);
    if (NULL == content) {
        return lpub_rtn_lud(lua, NULL, 0);
    }
    return lpub_rtn_lud(lua, content, strlen(content));
}
LUAMOD_API int luaopen_mail(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lprot_mail_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "reply", _lprot_mail_reply },
        { "subject",  _lprot_mail_subject },
        { "msg",  _lprot_mail_msg },
        { "html",  _lprot_mail_html },
        { "from",  _lprot_mail_from },
        { "from_get",  _lprot_mail_from_get },
        { "addrs_add",  _lprot_mail_addrs_add },
        { "addrs_get",  _lprot_mail_addrs_get },
        { "addrs_clear",  _lprot_mail_addrs_clear },
        { "attach_add",  _lprot_mail_attach_add },
        { "attach_clear",  _lprot_mail_attach_clear },
        { "clear",  _lprot_mail_clear },
        { "pack",  _lprot_mail_pack },
        { "__gc", _lprot_mail_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_SMTP_MAIL, reg_new, reg_func);
    return 1;
}
