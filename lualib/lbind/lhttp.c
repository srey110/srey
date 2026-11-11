#include "lbind/lpub.h"

// http 响应组包除头部块、头表与报文体外的预留：状态行（请求时为 0）+ JSON 的 Content-Type + 帧长头 + 空行
#define HTTP_RESP_RESERVE 128
#define HTTP_JSON_CT "Content-Type: application/json\r\n" // table 报文体自动带的头，同 lib/http
// http 头表预检时最多记下几条，组包照着写；多出的组包时从记下的最后一条接着遍历头表
#define HTTP_HEAD_CACHE 16

// http head_check 的结果码，按判定先后排
typedef enum http_head_rc {
    HTTP_HEAD_OK = 0,
    HTTP_HEAD_BADKEY,  // 头名不是 token，或长度达到 HTTP_HEAD_KEY_MAX
    HTTP_HEAD_BADTYPE, // 值不是 string / number
    HTTP_HEAD_AUTO,    // 组包函数自己生成的头
    HTTP_HEAD_BADVAL   // 值含 NUL / CR / LF
}http_head_rc;
// 预检记下的一条附加头。字符串由头表引用着，预检到组包之间不跑 Lua 代码、不改头表，指针一直有效
typedef struct http_head_ref {
    const char *key;
    const char *val;
    size_t klens;
    size_t vlens;
}http_head_ref;
// http.respond / pack_resp / pack_req 预检的结果：预检全在分配之前做完，组包阶段只照着写、不会再抛错
typedef struct http_resp_plan {
    int32_t code;      // 状态码；0 为请求：不写状态行，请求行放在 block
    int32_t nobody;    // 不写帧长头也不写报文体：1xx/204/304 响应、无报文体的请求
    int32_t headonly;  // 回 HEAD：Content-Length 写真实长度，不写报文体
    int32_t hidx;      // 头表的栈位，没有头表为 0
    int32_t json;      // 报文体是 table：组包时才编成 JSON，body 指向编码输出
    int32_t kidx;      // 头表多于 HTTP_HEAD_CACHE 条时，栈上留的第 HTTP_HEAD_CACHE 条的键的栈位
    const char *block; // 原样写在状态行后的字节（响应是预渲染头部块，请求是请求行），没有为 NULL
    const char *body;  // 报文体，没有为 NULL
    size_t bklens;
    size_t blens;
    size_t hlens;      // 头表写出的总字节
    size_t hcnt;       // 头表条数，前 HTTP_HEAD_CACHE 条记在 heads
    http_head_ref heads[HTTP_HEAD_CACHE];
}http_resp_plan;
// table 报文体编成 JSON 后交给组包的上下文
typedef struct http_resp_json {
    lua_State *lua;
    http_resp_plan *plan;
    binary_ctx *bw;
}http_resp_json;
/// <summary>
/// 将 HTTP 状态码转换为对应的文本描述（如 200 → "OK"）
/// </summary>
/// <param name="code" type="integer">HTTP 状态码，超出 int32 报错</param>
/// <returns type="string">文本描述</returns>
static int32_t _lhttp_code_status(lua_State *lua) {
    int32_t err = lpub_check_i32(lua, 1, "status code out of range");
    lua_pushstring(lua, http_code_status(err));
    return 1;
}
/// <summary>
/// 该状态码的响应是否禁止携带报文体与 Content-Length，判据同 C 层 http_code_nobody
/// </summary>
/// <param name="code" type="integer">HTTP 状态码，超出 int32 报错</param>
/// <returns type="boolean">禁止携带时 true</returns>
static int32_t _lhttp_code_nobody(lua_State *lua) {
    return lpub_rtn_bool(lua, 0 != http_code_nobody(lpub_check_i32(lua, 1, "status code out of range")));
}
/// <summary>
/// 客户端收到的这个包是否 1xx 中间响应，判据同 C 层 http_is_interim
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="boolean">是中间响应时 true</returns>
static int32_t _lhttp_interim(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    return lpub_rtn_bool(lua, 0 != http_is_interim(pack));
}
/// <summary>
/// 返回包的分帧类别，同 C 层 http_chunked。响应既无 Content-Length 又无 TE、或 TE 最后一层不是 chunked 时
/// (RFC 7230 §3.3.3 规则 7 / 规则 3)body 由连接关闭界定，头包本值是 0 而 body 确实按分片投；
/// 判"还有没有后续"要看消息自带的 slice 标记，不能用本值
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="integer">0=非 chunked 整包或由连接关闭界定 body 的头包；1=chunked 头包；
/// 2=数据片(chunked 的数据块与结束块、连接关闭界定的 body 片与末片)</returns>
static int32_t _lhttp_chunked(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    lua_pushinteger(lua, http_chunked(pack));
    return 1;
}
/// <summary>
/// 返回 HTTP 状态行 / 请求行三元组
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="string[]?">3 元数组：响应为 {version, code, message}，请求为 {method, uri, version}；分块中间包返回 nil</returns>
static int32_t _lhttp_status(lua_State *lua) {
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
static int32_t _lhttp_head(lua_State *lua) {
    LPUB_LUD_ARG(lua, struct http_pack_ctx, 1, pack);
    const char *key = lpub_check_cstr(lua, 2);
    size_t vlens;
    char *val = http_header(pack, key, &vlens);
    if (NULL == val) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, val, vlens);
    return 1;
}
/// <summary>
/// 返回所有 HTTP 头部字段。键是报文里的原样头名(不转大小写)；同名头只留最后一个，
/// 要按序取第一个用 http.head
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="table&lt;string,string&gt;?">key→value 表；分块中间包返回 nil</returns>
static int32_t _lhttp_heads(lua_State *lua) {
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
/// </summary>
/// <param name="pack" type="lightuserdata">http_pack_ctx 指针</param>
/// <returns type="lightuserdata?">body 数据指针；空时返回 nil。与本文件各 pack_* 的返回值形状相同但语义相反：
/// 这是 pack 内部的借用指针，随 pack 一起失效，调用方既不拥有它、也不能对它调 utils.ud_free</returns>
/// <returns type="integer">body 字节数；空时为 0</returns>
static int32_t _lhttp_data(lua_State *lua) {
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
static int32_t _lhttp_datastr(lua_State *lua) {
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
static const char *_lhttp_str_arg(lua_State *lua, int32_t idx, size_t *lens) {
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
static int32_t _lhttp_is_token(lua_State *lua) {
    size_t lens;
    const char *s = _lhttp_str_arg(lua, 1, &lens);
    return lpub_rtn_bool(lua, NULL != s && 0 != is_token(s, lens));
}
/// <summary>
/// 判一段头值能否进线格式，规则见 C 层 http_head_val_ok。头名按 is_token 判
/// </summary>
/// <param name="s" type="string">待判头值；非字符串返回 false</param>
/// <returns type="boolean">可以进线格式返回 true</returns>
static int32_t _lhttp_head_val_ok(lua_State *lua) {
    size_t lens;
    const char *s = _lhttp_str_arg(lua, 1, &lens);
    return lpub_rtn_bool(lua, NULL != s && 0 != http_head_val_ok(s, lens));
}
static int32_t _lhttp_head_fail(lua_State *lua, http_head_rc rc) {
    lua_pushnil(lua);
    lua_pushinteger(lua, rc);
    return 2;
}
/// <summary>
/// 一次判完一条附加头能否进报文，依次判：头名是 token 且短于 HTTP_HEAD_KEY_MAX（判据同 router_req_respond，见 http_head_key_check）、
/// 值是 string 或 number、不是组包函数自己生成的头、值不含 NUL/CRLF，第一条不过即停。
/// number 值的写法同 num_str（整数值的浮点按整数写）
/// </summary>
/// <param name="key" type="any">头名；非字符串按头名不合法算</param>
/// <param name="val" type="any">头值</param>
/// <param name="autoflags" type="integer">组包函数自己写的头：http.auto_ct 与 http.auto_frame 的按位或</param>
/// <returns type="string?">可直接写进报文的头值；没通过时为 nil</returns>
/// <returns type="integer">0 通过；1 头名不是 token 或过长；2 值不是 string/number；3 是组包函数自己生成的头；4 值含 NUL/CRLF</returns>
static int32_t _lhttp_head_check(lua_State *lua) {
    int32_t flags = lpub_check_i32(lua, 3, "autoflags out of range");
    size_t klens;
    const char *key = _lhttp_str_arg(lua, 1, &klens);
    http_key_rc krc = http_head_key_check(key, klens, flags);
    if (HTTP_KEY_BAD == krc) {
        return _lhttp_head_fail(lua, HTTP_HEAD_BADKEY);
    }
    int32_t vt = lua_type(lua, 2);
    if (LUA_TSTRING != vt && LUA_TNUMBER != vt) {
        return _lhttp_head_fail(lua, HTTP_HEAD_BADTYPE);
    }
    if (HTTP_KEY_AUTO == krc) {
        return _lhttp_head_fail(lua, HTTP_HEAD_AUTO);
    }
    if (LUA_TNUMBER == vt) {
        int32_t isint;
        lua_Integer iv = lua_tointegerx(lua, 2, &isint);
        if (isint) {
            lua_pushinteger(lua, iv);
            lua_replace(lua, 2);
        }
    }
    size_t vlens;
    const char *val = luaL_tolstring(lua, 2, &vlens);
    if (0 == http_head_val_ok(val, vlens)) {
        return _lhttp_head_fail(lua, HTTP_HEAD_BADVAL);
    }
    lua_pushinteger(lua, HTTP_HEAD_OK);
    return 2;
}
// 头表预检：每条都得是合法头名（见 http_head_key_check，帧长头不收，table 体时 Content-Type 也不收）+ 合法 string 值，顺带累计写出长度。
// 有一条不过就整体退回脚本侧原路（告警与丢弃在那边做），这里不做半截处理。
// 前 HTTP_HEAD_CACHE 条记进 plan->heads；记满时在栈上多留一份那条的键（到函数返回都不弹），头表还有剩下的组包就从它接着遍历
static int32_t _lhttp_resp_heads_check(lua_State *lua, int32_t idx, http_resp_plan *plan) {
    int32_t autoflags = HTTP_AUTO_FRAME | (plan->json ? HTTP_AUTO_CT : 0);
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
        if (HTTP_KEY_OK != http_head_key_check(key, klens, autoflags)
            || 0 == http_head_val_ok(val, vlens)) {
            lua_pop(lua, 2);
            return ERR_FAILED;
        }
        plan->hlens += klens + vlens + 4;
        if (plan->hcnt < HTTP_HEAD_CACHE) {
            plan->heads[plan->hcnt] = (http_head_ref){ key, val, klens, vlens };
        }
        plan->hcnt++;
        lua_pop(lua, 1);
        if (HTTP_HEAD_CACHE == plan->hcnt) {
            lua_pushvalue(lua, -1);// lua_next 只取走栈顶这份
            plan->kidx = lua_gettop(lua) - 1;
        }
    }
    return ERR_OK;
}
// 头表预检结果记进 plan：nil 为没有头表；无元表且每条都合法才接（规则见 _lhttp_resp_heads_check）
static int32_t _lhttp_heads_plan(lua_State *lua, int32_t hidx, http_resp_plan *plan) {
    plan->hidx = 0;
    plan->hlens = 0;
    plan->hcnt = 0;
    int32_t ht = lua_type(lua, hidx);
    if (LUA_TTABLE == ht) {
        // 带元表的表 pairs 可能走 __pairs，遍历顺序与内容都不归这里管
        if (lua_getmetatable(lua, hidx)) {
            lua_pop(lua, 1);
            return ERR_FAILED;
        }
        if (ERR_OK != _lhttp_resp_heads_check(lua, hidx, plan)) {
            return ERR_FAILED;
        }
        plan->hidx = hidx;
    } else if (LUA_TNIL != ht && LUA_TNONE != ht) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 栈位 idx..idx+4 依次是 code、headers、body、headonly、block。只接常见形状：[100, 999] 的整数码、
// nil 或 string 报文体、没有头表时的 table 报文体（1xx/204/304 不看报文体）、无元表的头表、nil 或 string 头部块；
// 其余返回 ERR_FAILED 交回脚本侧原路。带头表的 table 体暂不接（要接时去掉 table 分支的头表条件即可，
// 头表预检已按 json 拒 Content-Type，同 _lhttp_req_check）
static int32_t _lhttp_resp_check(lua_State *lua, int32_t idx, http_resp_plan *plan) {
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
    return _lhttp_heads_plan(lua, idx + 1, plan);
}
// 栈位 idx..idx+2 依次是请求行、headers、body。与 _lhttp_resp_check 同口径，差在两处：无报文体只写空行、
// 不补 Content-Length（同脚本侧"请求 + 无体"）；带头表的 table 体也接，头表里有 Content-Type 才退回原路
static int32_t _lhttp_req_check(lua_State *lua, int32_t idx, http_resp_plan *plan) {
    if (LUA_TSTRING != lua_type(lua, idx)) {
        return ERR_FAILED;
    }
    plan->code = 0;
    plan->nobody = 0;
    plan->headonly = 0;
    plan->json = 0;
    plan->body = NULL;
    plan->blens = 0;
    plan->block = lua_tolstring(lua, idx, &plan->bklens);
    int32_t bt = lua_type(lua, idx + 2);
    if (LUA_TSTRING == bt) {
        plan->body = lua_tolstring(lua, idx + 2, &plan->blens);
    } else if (LUA_TTABLE == bt) {
        plan->json = 1;
    } else if (LUA_TNIL == bt || LUA_TNONE == bt) {
        plan->nobody = 1;
    } else {
        return ERR_FAILED;
    }
    return _lhttp_heads_plan(lua, idx + 1, plan);
}
// 照预检结果组包：先写记下的头，多出的从预检留在栈上的键接着遍历头表，顺序与预检、与脚本侧 pairs 都相同；这里不再抛错
static void _lhttp_resp_write(lua_State *lua, http_resp_plan *plan, binary_ctx *bw) {
    binary_init_write(bw, HTTP_RESP_RESERVE + plan->bklens + plan->hlens + (plan->headonly ? 0 : plan->blens), 0);
    if (0 != plan->code) {
        http_pack_resp(bw, plan->code);
    }
    if (NULL != plan->block) {
        binary_set_binary(bw, plan->block, plan->bklens);
    }
    const http_head_ref *ref;
    for (size_t i = 0; i < plan->hcnt && i < HTTP_HEAD_CACHE; i++) {
        ref = &plan->heads[i];
        binary_set_binary(bw, ref->key, ref->klens);
        binary_set_binary(bw, ": ", sizeof(": ") - 1);
        binary_set_binary(bw, ref->val, ref->vlens);
        binary_set_binary(bw, FLAG_CRLF, CRLF_SIZE);
    }
    if (plan->hcnt > HTTP_HEAD_CACHE) {
        size_t klens, vlens;
        const char *key;
        const char *val;
        lua_pushvalue(lua, plan->kidx);
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
static void _lhttp_resp_json_sink(void *ud, const char *json, size_t lens) {
    http_resp_json *arg = ud;
    arg->plan->body = json;
    arg->plan->blens = lens;
    _lhttp_resp_write(arg->lua, arg->plan, arg->bw);
}
// 照预检结果组包；table 报文体先编成 JSON，编不出来返回 ERR_FAILED（bw 未分配），交回脚本侧原路去报错
static int32_t _lhttp_resp_pack(lua_State *lua, int32_t idx, http_resp_plan *plan, binary_ctx *bw) {
    if (!plan->json) {
        _lhttp_resp_write(lua, plan, bw);
        return ERR_OK;
    }
    http_resp_json arg;
    arg.lua = lua;
    arg.plan = plan;
    arg.bw = bw;
    return lyyjson_encode_sink(lua, idx + 2, _lhttp_resp_json_sink, &arg);
}
/// <summary>
/// 服务端 HTTP 响应整条在 C 里组包并直接发出，字节与 lib/http 的 http.response / response_head 一致。
/// 只接常见形状：[100, 999] 的整数码、nil 或 string 报文体、没有头表时的 table 报文体（编成 JSON 并带
/// Content-Type: application/json，同 lib/http；1xx/204/304 不看报文体）、
/// 无元表且每条都合法（短于 HTTP_HEAD_KEY_MAX 的 token 名、string 值、无 NUL/CRLF、不是 Content-Length / Transfer-Encoding）的头表；
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
static int32_t _lhttp_respond(lua_State *lua) {
    http_resp_plan plan;
    sock_ctx *sk = lpub_is_sock(lua, 1);
    if (NULL == sk
        || ERR_OK != _lhttp_resp_check(lua, 2, &plan)) {
        return lpub_rtn_bool(lua, 0);
    }
    binary_ctx bw;
    if (ERR_OK != _lhttp_resp_pack(lua, 2, &plan, &bw)) {
        return lpub_rtn_bool(lua, 0);
    }
    ev_send(&g_loader->netev, sk, bw.data, bw.offset, 0);
    return lpub_rtn_bool(lua, 1);
}
// 外部字符串的释放回调：缓冲来自 binary_init_write
static void *_lhttp_ext_free(void *ud, void *ptr, size_t osize, size_t nsize) {
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
static int32_t _lhttp_pack_resp(lua_State *lua) {
    http_resp_plan plan;
    binary_ctx bw;
    if (ERR_OK != _lhttp_resp_check(lua, 1, &plan)
        || ERR_OK != _lhttp_resp_pack(lua, 1, &plan, &bw)) {
        return lpub_rtn_nil(lua, 1);
    }
    // 直接把缓冲交给 Lua 当字符串：失败时由 Lua 调回调释放，不会漏
    bw.data[bw.offset] = '\0';
    lua_pushexternalstring(lua, bw.data, bw.offset, _lhttp_ext_free, NULL);
    return 1;
}
/// <summary>
/// 客户端 HTTP 请求整条在 C 里组成一块，字节与 lib/http 的 http.get / head_req / post 原路一致，
/// 供 srey.syn_send(sk, data, size, 0) 零拷贝发出。只接常见形状：nil、string 或 table 报文体（table 编成
/// JSON 并带 Content-Type: application/json，此时头表里不得有 Content-Type）、无元表且每条都合法
/// （短于 HTTP_HEAD_KEY_MAX 的 token 名、string 值、无 NUL/CRLF、不是 Content-Length / Transfer-Encoding）的头表；其余一律不碰，
/// 交给调用方走原来的组包路径（告警、丢头、报错都在那边）。不自动加任何头（含 Host）
/// </summary>
/// <param name="sk" type="userdata">连接标识，只认类型、不发送：不是连接标识时不组包，免得组好的缓冲在 send 报参数错时没人释放</param>
/// <param name="line" type="string">请求行（已含 \r\n），原样写在最前、不做校验：url 的 NUL/CRLF 由调用方先挡。
/// 业务发请求走 lib/http 的 get / head_req / post，别直接调本函数</param>
/// <param name="headers" type="table&lt;string,string&gt;?">附加头部</param>
/// <param name="body" type="string|table&lt;any,any&gt;?">报文体；nil 时到空行为止、不写 Content-Length</param>
/// <returns type="lightuserdata?">整条请求；形状不归它管、sk 不是连接标识、table 报文体编不成 JSON、总长超过 INT32_MAX 时为 nil。
/// 所有权归调用方：交给 copy=0 的发送后由框架释放，不发要自己 utils.ud_free</returns>
/// <returns type="integer?">字节数；第一个返回值为 nil 时也为 nil</returns>
static int32_t _lhttp_pack_req(lua_State *lua) {
    http_resp_plan plan;
    binary_ctx bw;
    if (NULL == lpub_is_sock(lua, 1)
        || ERR_OK != _lhttp_req_check(lua, 2, &plan)
        || ERR_OK != _lhttp_resp_pack(lua, 2, &plan, &bw)) {
        return lpub_rtn_nil(lua, 2);
    }
    // 超了 core.send 会在接管前报参数错，这块就漏了；交回原路由它照旧报错
    if (bw.offset > INT32_MAX) {
        binary_free(&bw);
        return lpub_rtn_nil(lua, 2);
    }
    return lpub_rtn_lud(lua, bw.data, bw.offset);
}
//srey.http
LUAMOD_API int luaopen_http(lua_State *lua) {
    luaL_Reg reg[] = {
        { "code_status", _lhttp_code_status },
        { "code_nobody", _lhttp_code_nobody },
        { "interim", _lhttp_interim },
        { "chunked", _lhttp_chunked },
        { "status", _lhttp_status },
        { "head", _lhttp_head },
        { "heads", _lhttp_heads },
        { "data", _lhttp_data },
        { "datastr", _lhttp_datastr },
        { "is_token", _lhttp_is_token },
        { "head_val_ok", _lhttp_head_val_ok },
        { "head_check", _lhttp_head_check },
        { "respond", _lhttp_respond },
        { "pack_resp", _lhttp_pack_resp },
        { "pack_req", _lhttp_pack_req },
        { NULL, NULL },
    };
    luaL_newlib(lua, reg);
    /// <field name="max_headlens" type="integer">HTTP 接收侧的头部块字节上限，取自 prots_pub.h
    /// 的 HTTP_MAX_HEADLENS；组包侧不按它判，要卡长度的调用方自己拿它比</field>
    lua_pushinteger(lua, (lua_Integer)HTTP_MAX_HEADLENS);
    lua_setfield(lua, -2, "max_headlens");
    /// <field name="auto_ct" type="integer">head_check 的 autoflags 位：Content-Type 由组包函数自己写</field>
    lua_pushinteger(lua, HTTP_AUTO_CT);
    lua_setfield(lua, -2, "auto_ct");
    /// <field name="auto_frame" type="integer">head_check 的 autoflags 位：Content-Length 与 Transfer-Encoding 由组包函数自己写</field>
    lua_pushinteger(lua, HTTP_AUTO_FRAME);
    lua_setfield(lua, -2, "auto_frame");
    return 1;
}
