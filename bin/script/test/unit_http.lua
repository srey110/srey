-- lib.http chunked 生产者违约(返回非 string):首块已发出后关连接、不补终止块。请求侧断言服务端收不到
-- 末片(截断的上传不被当成完整请求)且连接被断；响应侧断言客户端拿不到截断的响应、http.post 返回 nil。
-- 同一 task 内起 server(PACK_HTTP 监听)与 client(连回本机):server 对分片请求回固定串,非分片请求回显 body。
-- 另覆盖客户端 chunked 接收段(lib/http.lua 的 srey.syn_slice 循环):server 回真 chunked 响应,
-- client 分传 / 不传 ckfunc 两种走法；以及既无 CL 又无 TE、由连接关闭界定的响应。
-- 不走网络的三段：C 组包 srey.http.pack_resp 与 Lua 原路 _response_lua 逐字节对拍；
-- 请求侧 http.get / post 走 C 组包（srey.http.pack_req，copy=0）与 _http_msg 原路逐字节对拍；
-- 请求侧带头表时帧长头的取舍（抓 srey.syn_send 的入参看线缆字节）。

local srey   = require("lib.srey")
local runner = require("test.runner")
local http   = require("lib.http")
local srey_http = require("srey.http")-- C 绑定层,直接断言 is_token / max_headlens
local utils  = require("srey.utils")
local json   = require("yyjson")

local PORT = 15047
-- HEAD 用例的裸监听端口。测试模块并发跑,端口必须全仓唯一——15048 是 unit_websock 的,
-- 别用 PORT+1 这种推导写法,新端口先 grep 再定
local HEAD_PORT = 15049
local CK_RSP = "chunked-done"
-- n 条互不相同的合法附加头：C 组包对头表有 16 条的缓存，16 / 17 / 20 条分别落在缓存满、刚溢出、续扫三种走法上
local function _heads(n)
    local h = {}
    for i = 1, n do
        h[string.format("X-H%02d", i)] = "v" .. i
    end
    return h
end
local BODY = "hello"
local PROBE = "hdrprobe"
local FRAME = "frameprobe"
local INJ = "a\r\nX-Evil: 1"-- 头值里塞 CRLF：未过滤时会把一条响应劈成两条
local URL_INJ = "/x HTTP/1.1\r\nX-Evil: 1\r\n\r\nGET /y"-- 请求目标里塞 CRLF：未过滤时线缆上是两条请求
local PROBE2 = "afterinj"-- 拒绝之后的回显探针，验证连接没被污染
-- HEAD 用例的响应：带 Content-Length 却不带报文体。PACK_HTTP 那条服务端分支自己算
-- Content-Length，造不出这个形状，只能裸监听手写字节
local HEAD_RSP = "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nX-Head: v\r\n\r\n"
-- 1xx 用例：与 HEAD 共用同一个裸监听，按请求目标分流。一次写出 103 + 200 两条，
-- 客户端不跳过 1xx 的话会把 103 当结果返回，200 留在连接上
local INTERIM_URI = "/interim"
local INTERIM_RSP = "HTTP/1.1 103 Early Hints\r\nLink: </s.css>; rel=preload\r\n\r\n"
    .. "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
-- 由连接关闭界定 body 的用例：同样走那个裸监听，响应既无 CL 又无 TE，服务端写完即关连接
local TILLCLOSE_URI = "/tillclose"
local TILLCLOSE_BODY = "body-till-close"
local TILLCLOSE_RSP = "HTTP/1.1 200 OK\r\nX-Till: 1\r\n\r\n" .. TILLCLOSE_BODY
local N204 = "want204"-- 触发 server 回 204+body，验证 body 与 Content-Length 都被丢弃
local BADBODY = "wantbadbody"-- 触发 server 回一个类型不受支持的 body（数字），验证告警确实打出来
-- 触发 server 回真 chunked 响应(生产者吐两块)。lib.http 的接收段原先零覆盖:现有 /chunked
-- 那条是 HEAD,headonly 下生产者一次都不调,走不到 srey.syn_slice 那个循环
local CKBODY = "wantckbody"
local CK1 = "chunk-one"
local CK2 = "chunk-two-longer"
local BADCK = "wantbadck"-- 触发 server 回 chunked 响应时生产者违约

-- WARN 打桩，用来断言"响应带了不支持的 body 类型时确实告警"。
-- 每个 unit 模块是独立 task（各有各的 lua_State），改全局波及不到别的模块。
-- 代价是本模块日志里的文件/行号会偏一层——log.lua 让 C 侧按固定栈层(3)取调用位置，这里多垫了一帧
local warn_log = {}
local _warn_raw = WARN
WARN = function(fmt, ...)
    local ok, s = pcall(string.format, fmt, ...)
    warn_log[#warn_log + 1] = ok and s or tostring(fmt)
    _warn_raw(fmt, ...)
end
-- warn_log 里有没有含 sub 的一条
local function _warned(sub)
    for i = 1, #warn_log do
        if nil ~= string.find(warn_log[i], sub, 1, true) then
            return true
        end
    end
    return false
end
-- 单条头就把头部块顶过 HTTP_MAX_HEADLENS(4096)：组包侧不卡长度，该原样发出
local BIG = string.rep("b", 4096)

-- chunked 响应的生产者:吐两块再返回 nil 终止
local function _ck_producer(state)
    state.n = state.n + 1
    if 1 == state.n then
        return CK1
    end
    if 2 == state.n then
        return CK2
    end
    return nil
end

-- 第 1 块正常发出(对端已进入 chunked 报文体),第 2 块返回非 string 触发违约；请求、响应两侧共用
local function _bad_producer(state)
    state.n = state.n + 1
    if 1 == state.n then
        return "aa"
    end
    return 42
end

-- 裸连接（PACK_TYPE.NONE 不分帧）上发一条请求并累积到出现 term 为止。
-- 一次 RECV 未必是整条响应，只取第一段的话会把"没收全"误报成"服务端过滤掉了"
---@param sk userdata 连接标识
---@param req string 完整请求报文
---@param term string 收齐判据（如 "\r\n\r\n"）
---@return string txt 累积到的全部字节
local function _raw_probe(sk, req, term)
    local txt = ""
    local rsp, rlen = srey.syn_send(sk, req, #req, 1)
    while rsp do
        txt = txt .. srey.ud_str(rsp, rlen)
        if string.find(txt, term, 1, true) then
            break
        end
        rsp, rlen = srey.syn_recv(sk)
    end
    return txt
end

srey.startup(function()
runner.run(function(t)
    -- 服务端收到的分片请求末片数、服务端这头断开的连接数，违约用例靠它们断言
    local nsrv_end, nsrv_close = 0, 0
    srey.on_closed(function(_, _, client)
        if 0 == client then
            nsrv_close = nsrv_close + 1
        end
    end)
    srey.on_recved(function(pktype, sk, client, slice, data, size)
        -- 只处理 accept 来的连接;客户端侧响应由 syn_send 的等待者接走。
        -- 不能按 fd 值排除客户端 socket:客户端 fd 关掉后号会被后续 accept 复用,
        -- 闭包里的旧值就把服务端自己的 socket 当成客户端放过去,于是永不回应、对端等满超时
        if 0 ~= client then
            return
        end
        if PACK_TYPE.NONE == pktype then
            -- 裸监听侧，三个用例共用：按请求目标分流
            local req = srey.ud_str(data, size)
            if nil ~= string.find(req, INTERIM_URI, 1, true) then
                srey.send(sk, INTERIM_RSP, #INTERIM_RSP, 1)
            elseif nil ~= string.find(req, TILLCLOSE_URI, 1, true) then
                srey.send(sk, TILLCLOSE_RSP, #TILLCLOSE_RSP, 1)
                srey.close(sk)-- body 的结尾只由关闭界定
            else
                srey.send(sk, HEAD_RSP, #HEAD_RSP, 1)
            end
            return
        end
        if 0 ~= slice then
            if 0 ~= (slice & 4) then-- PROT_SLICE_END:分片请求收齐才回
                nsrv_end = nsrv_end + 1
                http.response(sk, 200, nil, CK_RSP)
            end
            return
        end
        local st = http.status(data)
        if st and "HEAD" == st[1] then
            if "/chunked" == st[2] then
                -- body 是生产者函数：长度未知省掉 CL，但 GET 会发的 TE 得照发
                http.response_head(sk, 200, nil, function() return nil end)
            else
                -- info 传 nil：验 HEAD 分支照样补 Content-Length: 0
                http.response_head(sk, 200, { ["X-Head-Probe"] = "v" }, nil)
            end
            return
        end
        local body = http.datastr(data)
        if PROBE == body then
            -- 六种头一次过：合法 token / 值含 CRLF / 名非 token / 顶过 HTTP_MAX_HEADLENS /
            -- 整数值浮点 / __tostring 会抛的值；且不带 body。
            -- 最后那个若照旧走 tostring，异常会越过整条校验链把这次响应整个吞掉
            http.response(sk, 200, { ["X-Ok"] = "v", ["X-Inj"] = INJ, ["Bad Key"] = "x", ["X-Big"] = BIG,
                                           ["X-Num"] = 3600000 / 1000,
                                           ["X-Raiser"] = setmetatable({}, { __tostring = function() error("boom") end }) })
            return
        end
        if FRAME == body then
            -- 带 body 的响应，调用方同时塞进本函数自己会生成的两条帧长头。
            -- 大小写故意写乱：HTTP 头名大小写无关，判定不能只认标准写法。
            -- 放行的话线缆上会是 TE 叠一条自造 CL 再叠调用方那条 CL，走私形态
            http.response(sk, 200,
                { ["transfer-ENCODING"] = "chunked", ["Content-Length"] = "999", ["X-Keep"] = "1" }, BODY)
            return
        end
        if N204 == body then
            -- 204 禁带报文体：故意塞一个 body，验证被 http.response 丢弃
            http.response(sk, 204, nil, "dropped")
            return
        end
        if CKBODY == body then
            -- info 传函数即 chunked:每次调它取一块,返 nil 终止
            http.response(sk, 200, nil, _ck_producer, { n = 0 })
            return
        end
        if BADCK == body then
            http.response(sk, 200, nil, _bad_producer, { n = 0 })
            return
        end
        if BADBODY == body then
            -- body 是数字：三个 body 分支都不命中，按无 body 发走并告警
            http.response(sk, 200, nil, 42)
            return
        end
        http.response(sk, 200, nil, (body and #body > 0) and body or "ok")
    end)

    local lid = srey.listen(PACK_TYPE.HTTP, SSL_NAME.NONE, "0.0.0.0", PORT)
    t:check(ERR_FAILED ~= lid, "listen " .. PORT)
    if ERR_FAILED == lid then
        return
    end
    local cli_sk
    cli_sk = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", PORT)
    t:check(cli_sk and cli_sk.valid, "connect")
    if not cli_sk or not cli_sk.valid then
        srey.unlisten(lid)
        return
    end

    -- 请求侧违约：首块 "aa" 已发出，连接由 http.post 关掉。等服务端看到断开再数末片
    local vsk = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", PORT)
    if t:check(vsk and vsk.valid, "违约用例 connect") then
        local end0, close0 = nsrv_end, nsrv_close
        t:eq(nil, http.post(vsk, "/", nil, nil, _bad_producer, { n = 0 }), "请求侧生产者违约返回 nil")
        for _ = 1, 150 do
            if nsrv_close > close0 then
                break
            end
            srey.sleep(20)
        end
        t:eq(close0 + 1, nsrv_close, "请求侧违约后连接被断")
        t:eq(end0, nsrv_end, "服务端没收到末片(截断的请求没被当成完整的)")
    end
    -- 响应侧违约：服务端首块已发出就关连接，客户端收不到终止块，按失败返回而不是拿到截断的 body
    local rsk = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", PORT)
    if t:check(rsk and rsk.valid, "响应侧违约用例 connect") then
        t:eq(nil, http.post(rsk, "/", nil, nil, BADCK), "响应侧生产者违约：客户端拿不到截断的响应")
        srey.close(rsk)
    end

    -- url 里塞 CRLF 就是请求拆分：放行的话对端会看到两条完整请求、回两条响应，
    -- 多出来那条被下一次请求的等待者接走，连接从此错开一位。
    -- 必须在写 socket 之前就拒，故断言"下一次正常请求仍拿到属于自己的响应"
    t:eq(nil, http.get(cli_sk, URL_INJ), "url 含 CRLF 的 GET 被拒")
    t:eq(nil, http.post(cli_sk, URL_INJ, nil, nil, BODY), "url 含 CRLF 的 POST 被拒")
    t:eq(nil, http.get(cli_sk, "/x\0y"), "url 含 NUL 被拒")
    local after = http.post(cli_sk, "/", nil, nil, PROBE2)
    t:check(nil ~= after, "被拒后连接仍可用(一个字节都没写出去)")
    if after then
        t:eq(PROBE2, after.data, "拒绝的请求未在连接上留下任何残留")
    end
    -- 请求走 C 组包的两种体：table 编成 JSON（带头表）、接近 Content-Length 上限的 string 体（服务端回显 body）
    local jbody = { a = 1, s = "x" }
    local jp = http.post(cli_sk, "/", { ["X-T"] = "1" }, nil, jbody)
    t:eq(json.encode(jbody), jp and jp.data, "table 体经 C 组包发出，服务端收到同一份 JSON")
    local big = string.rep("q", 60000)-- HTTP_MAX_CONTENT_LENS 是 65535，64KB 会被服务端拒
    local bp = http.post(cli_sk, "/", nil, nil, big)
    t:eq(big, bp and bp.data, "60000 字节 string 体经 C 组包整条发出")

    -- lib.http 的 chunked 接收段:不传 ckfunc 时逐块累积再拼进 pack.data
    local ckp = http.post(cli_sk, "/", nil, nil, CKBODY)
    t:check(nil ~= ckp, "chunked 响应收到")
    if ckp then
        t:eq(CK1 .. CK2, ckp.data, "两块 chunk 按序拼进 pack.data")
        t:eq(#CK1 + #CK2, ckp.cksize, "cksize 累加两块的字节数")
    end
    -- 传 ckfunc 时逐块回调、不落 pack.data。只断言最后一次回调的 fin 而不数回调次数:
    -- 终止块是否单独成一次回调由 C 侧的分片边界决定,不该在这里锁死
    local got, fins = {}, {}
    local ckp2 = http.post(cli_sk, "/", nil, function(fin, d, sz)
        if d and sz > 0 then
            got[#got + 1] = srey.ud_str(d, sz)
        end
        fins[#fins + 1] = fin
    end, CKBODY)
    t:check(nil ~= ckp2, "带 ckfunc 的 chunked 响应收到")
    if ckp2 then
        t:eq(2, #got, "ckfunc 收到两块非空 chunk")
        t:eq(CK1, got[1], "ckfunc 第一块内容")
        t:eq(CK2, got[2], "ckfunc 第二块内容")
        t:eq(true, fins[#fins], "末次回调 fin=true")
        t:eq(#CK1 + #CK2, ckp2.cksize, "带 ckfunc 时 cksize 同样累加")
        t:eq(nil, ckp2.data, "带 ckfunc 时不落 pack.data")
    end

    -- 裸连接读原始字节：srey 自己的解析器对"无 CL 无 chunked"是宽容的，
    -- 用解析后的 pack 断言区分不出 Content-Length 有没有发出去，只能看线缆上的字节
    local raw_sk
    raw_sk = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", PORT)
    t:check(raw_sk and raw_sk.valid, "raw connect")
    if raw_sk and raw_sk.valid then
        local req = string.format("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n\r\n%s",
                                  #PROBE, PROBE)
        -- PACK_TYPE.NONE 不分帧,一次 RECV 未必是整条响应；累积到空行(头结束)为止再断言。
        -- 直接拿第一段就断言的话,被 TCP 切开时会把"没收全"误报成"注入头已被过滤"
        local txt = _raw_probe(raw_sk, req, "\r\n\r\n")
        t:check(nil ~= string.find(txt, "\r\n\r\n", 1, true), "raw 探针收到完整响应头")
        if string.find(txt, "\r\n\r\n", 1, true) then
            t:check(nil == string.find(txt, "X-Evil", 1, true), "值含 CRLF 的头被丢弃,报文未被劈成两条")
            t:check(nil == string.find(txt, "Bad Key", 1, true), "名非 RFC7230 token 的头被丢弃")
            t:check(nil ~= string.find(txt, "X-Big: " .. BIG, 1, true), "顶过 HTTP_MAX_HEADLENS 的头原样发出(组包侧不卡)")
            t:check(nil ~= string.find(txt, "X-Ok: v", 1, true), "合法头正常发出(未误伤)")
            t:check(nil ~= string.find(txt, "X-Num: 3600\r\n", 1, true), "整数值浮点头写成 3600 而不是 3600.0")
            t:check(nil == string.find(txt, "X-Raiser", 1, true), "__tostring 会抛的头值被丢弃")
            t:check(nil ~= string.find(txt, "Content-Length: 0", 1, true), "无 body 的响应带 Content-Length: 0")
        end
        -- 同一条裸连接再发一次：带 body 且调用方自带帧长头，验证不会造出走私形态。
        -- 这次要读到 body 结束(空行 + BODY)，不能只读到头结束
        local freq = string.format("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n\r\n%s",
                                   #FRAME, FRAME)
        local ftxt = _raw_probe(raw_sk, freq, "\r\n\r\n" .. BODY)
        t:check(nil ~= string.find(ftxt, "\r\n\r\n" .. BODY, 1, true), "帧长探针收到完整响应")
        if nil ~= string.find(ftxt, "\r\n\r\n" .. BODY, 1, true) then
            local ncl = select(2, string.gsub(string.lower(ftxt), "content%-length:", ""))
            t:eq(1, ncl, "线缆上恰好一条 Content-Length(调用方那条被丢弃)")
            t:check(nil == string.find(string.lower(ftxt), "transfer%-encoding"),
                    "调用方自带的 Transfer-Encoding 被丢弃(未与 CL 叠成走私形态)")
            t:check(nil ~= string.find(ftxt, "Content-Length: " .. #BODY, 1, true),
                    "发出的是本函数按 body 算的长度,不是调用方那个 999")
            t:check(nil ~= string.find(ftxt, "X-Keep: 1", 1, true), "同批的普通头未受牵连")
        end
        -- 同一条裸连接再发一次：204 响应必须既无 Content-Length 也无 body（RFC 7230 §3.3.2）。
        -- 204 无 body，读到头结束(空行)就是整条响应
        local nreq = string.format("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n\r\n%s",
                                   #N204, N204)
        local ntxt = _raw_probe(raw_sk, nreq, "\r\n\r\n")
        -- 守卫与它的 companion 用同一个判据：换成 "收到了 \r\n\r\n" 的话，
        -- 探针超时收不到响应时下面两条会被静默跳过，一条 FAIL 都不出
        if t:check(nil ~= string.find(ntxt, " 204 ", 1, true), "204 探针收到 204 响应") then
            t:check(nil == string.find(string.lower(ntxt), "content%-length"),
                    "204 响应不带 Content-Length")
            t:check(nil == string.find(ntxt, "dropped", 1, true), "204 的 body 被丢弃")
        end
        -- 同一条裸连接再来一次：响应的 body 类型不受支持（数字）。线缆上要和"无 body 的响应"
        -- 一模一样（Content-Length: 0，无体），但必须打出告警——否则调用方从返回值上看不出
        -- body 没发出去。以前那句 WARN 排在 else 分支里，被 `rsp and not nocl` 那支挡住，
        -- 响应侧永远不触发
        for i = #warn_log, 1, -1 do
            warn_log[i] = nil-- 先清空，断言只认这一趟打出来的
        end
        local breq = string.format("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n\r\n%s",
                                   #BADBODY, BADBODY)
        local btxt = _raw_probe(raw_sk, breq, "\r\n\r\n")
        t:check(nil ~= string.find(btxt, " 200 ", 1, true), "不支持的 body 类型仍回 200")
        t:check(nil ~= string.find(btxt, "Content-Length: 0", 1, true),
                "不支持的 body 类型按无 body 发出(Content-Length: 0)")
        t:check(nil == string.find(btxt, "42", 1, true), "那个数字没被当成 body 发出去")
        t:check(_warned("unsupported body type"), "不支持的 body 类型打出了告警")
        -- 同一条裸连接再来一次：HEAD + info 为 nil。头必须与同一资源的 GET 一致，
        -- GET 那条走"响应无 body"分支会写 Content-Length: 0，HEAD 不能因为不发体就把它省掉
        local hreq = "HEAD / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
        local htxt = _raw_probe(raw_sk, hreq, "\r\n\r\n")
        t:check(nil ~= string.find(htxt, " 200 ", 1, true), "HEAD 探针收到 200 响应")
        t:check(nil ~= string.find(htxt, "Content-Length: 0", 1, true),
                "HEAD 无 body 时照写 Content-Length: 0(与同资源的 GET 一致)")
        t:check(nil ~= string.find(htxt, "X-Head-Probe: v", 1, true), "HEAD 响应的普通头照常发出")
        -- 再来一次：body 是生产者函数。长度未知省掉 CL 是有意的，但 GET 那条会发
        -- Transfer-Encoding，HEAD 省掉它两个方法的头就对不上了
        local creq = "HEAD /chunked HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
        local ctxt = _raw_probe(raw_sk, creq, "\r\n\r\n")
        t:check(nil ~= string.find(ctxt, " 200 ", 1, true), "HEAD 生产者函数体收到 200 响应")
        t:check(nil ~= string.find(ctxt, "Transfer-Encoding: chunked", 1, true),
                "HEAD 的生产者函数体照发 Transfer-Encoding(与同资源的 GET 一致)")
        t:check(nil == string.find(ctxt, "Content-Length", 1, true),
                "长度未知时不写 Content-Length")
        srey.close(raw_sk)
    end

    -- token 判定与头部块上限都取自 C，不再在 Lua 重抄一份
    t:check(srey_http.is_token("X-Ok"), "is_token 认合法 token")
    t:check(not srey_http.is_token("Bad Key"), "is_token 拒含空格的头名")
    t:check(not srey_http.is_token(""), "is_token 拒空串")
    t:check(not srey_http.is_token(1), "is_token 拒非字符串(数字当不了头名)")
    t:eq(4096, srey_http.max_headlens, "max_headlens 取自 prots_pub.h 的 HTTP_MAX_HEADLENS")
    -- 组包侧不按它判(见上面 X-Big 那条)，这里只确认常量值没与 C 分叉
    -- 附加头名长度上限同 C 侧 router_req_respond(HTTP_HEAD_KEY_MAX = 128，不含)：127 字节放行、128 字节拒
    local _, hrc = srey_http.head_check(string.rep("k", 127), "v", 0)
    t:eq(0, hrc, "127 字节的头名放行")
    _, hrc = srey_http.head_check(string.rep("k", 128), "v", 0)
    t:eq(1, hrc, "128 字节的头名按头名不合法拒")
    t:eq(nil, srey_http.pack_resp(200, { [string.rep("k", 128)] = "v" }, "b"), "超长头名 C 组包不接")

    -- C 组包与 Lua 原路逐字节对拍：同一形状走 C(srey.http.respond)还是走 _response_lua，
    -- 取决于头值类型、码、body 类型，两边发出的字节必须一样。pack_resp 是 respond 的不发送版；
    -- Lua 原路把 srey.send 临时换成抓包（整段同步执行，换回之前没有别的协程插进来）
    do
        local PLAIN = "Content-Type: text/plain; charset=utf-8\r\n"
        local function lua_bytes(headonly, code, block, headers, body)
            local cap = {}
            local send = srey.send
            srey.send = function(_, data)
                cap[#cap + 1] = data
                return true
            end
            local ok = pcall(http._response_lua, headonly, "SK", code, block, headers, body)
            srey.send = send
            return ok and table.concat(cap) or nil
        end
        -- { 码, 头表, 报文体, 头部块 }，每种再分 GET / HEAD 两次
        local cases = {
            { 200, nil, "hello" },
            { 200, nil, "" },
            { 200, nil, nil },
            { 200, { ["X-A"] = "1", ["Content-Type"] = "text/plain" }, "body" },
            { 200, { ["X-A"] = "1" }, nil },
            { 200, {}, "empty-headers" },
            { 200, nil, { a = 1, b = { 1, 2, 3 }, c = "x/y\"\\\n中", d = 1.5, e = true, g = 3.0 } },
            { 200, nil, {} },
            { 200, nil, { 1, 2, 3 } },
            { 200, nil, "body", PLAIN },
            { 200, nil, nil, PLAIN },
            { 200, nil, { k = "v" }, PLAIN },
            { 200, { ["X-B"] = "2" }, "body", PLAIN },
            { 201, nil, string.rep("p", 100000) },
            { 100, nil, "x" },
            { 101, { Upgrade = "websocket" }, nil },
            { 204, nil, "x" },
            { 204, { ["X-C"] = "3" }, { a = 1 } },
            { 304, nil, nil },
            { 304, nil, "abc", PLAIN },
            { 404, nil, "Not Found\n", PLAIN },
            { 599, nil, "u" },
            { 200, _heads(16), "body" },
            { 200, _heads(17), "body" },
            { 200, _heads(20), nil },
        }
        local nmis = 0
        local cb, lb
        for i, c in ipairs(cases) do
            for _, headonly in ipairs({ false, true }) do
                cb = srey_http.pack_resp(c[1], c[2], c[3], headonly, c[4])
                lb = lua_bytes(headonly, c[1], c[4], c[2], c[3])
                if nil == cb or cb ~= lb then
                    nmis = nmis + 1
                    t:fail(string.format("pack_resp 对拍 #%d headonly=%s: C=%q Lua=%q",
                                         i, tostring(headonly), tostring(cb), tostring(lb)))
                end
            end
        end
        t:eq(0, nmis, "上面每种形状 C 都接手，且与 _response_lua 逐字节一致")
        -- C 不接的形状返回 nil，交回 Lua 原路（告警、丢头、报错都在那边）
        t:eq(nil, srey_http.pack_resp(200, { ["X-N"] = 5 }, "b"), "数字头值 C 不接")
        t:eq(nil, srey_http.pack_resp(200, { ["X-N"] = "5" }, { a = 1 }), "带头表的 table 体 C 不接")
        t:eq(nil, srey_http.pack_resp(200.0, nil, "b"), "浮点码 C 不接")
        t:eq(nil, srey_http.pack_resp(99, nil, "b"), "码 < 100 C 不接")
        t:eq(nil, srey_http.pack_resp(200, setmetatable({}, {}), "b"), "带元表的头表 C 不接")
        t:eq(nil, srey_http.pack_resp(200, nil, { f = print }), "编不成 JSON 的 table 体 C 不接")
        t:eq(nil, srey_http.pack_resp(200, { ["Content-Length"] = "3" }, "b"), "帧长头 C 不接")
        t:eq(nil, srey_http.pack_resp(200, { ["X"] = "a\r\nb" }, "b"), "头值含 CRLF C 不接")
        t:eq(nil, srey_http.pack_resp(200, nil, 5), "数字体 C 不接")
        t:eq(nil, srey_http.pack_resp(200, nil, "x", false, 5), "非字符串头部块 C 不接")
        -- 非法头排在遍历序第 17 条（缓存已满之后才校验失败）：照样整表不接。遍历中改已有字段的值是允许的
        local bad = _heads(20)
        bad["X-Bad"] = "ok"
        local nth = 0
        for k in pairs(bad) do
            nth = nth + 1
            if 17 == nth then
                bad[k] = "a\r\nb"
            end
        end
        t:eq(nil, srey_http.pack_resp(200, bad, "b"), "第 17 条头非法 C 不接")
    end

    -- 请求侧 C 组包与原路对拍：http.get / post 先试 srey.http.pack_req，接手的形状以 copy=0 交给 syn_send，
    -- 字节必须与 _http_msg 原路一样。srey.syn_send 临时换成抓包（整段同步执行）；sk 传字符串 "SK" 时
    -- pack_req 认不出连接标识，请求交回原路，以此取原路字节
    do
        local SOCK = srey.sock_invalid()-- 连接标识类型对就行，抓包不发送
        -- 调 fn(sk, ...)，返回交给 syn_send 的字节与 copy；copy=0 的缓冲当场释放
        local function api_bytes(sk, fn, ...)
            local cap, ccopy
            local syn = srey.syn_send
            srey.syn_send = function(_, data, size, copy)
                ccopy = copy
                if 0 == copy then
                    cap = srey.ud_str(data, size)
                    utils.ud_free(data)
                else
                    cap = data
                end
                return nil
            end
            local ok = pcall(fn, sk, ...)
            srey.syn_send = syn
            return ok and cap or nil, ccopy
        end
        local jt = { a = 1, b = { 1, 2, 3 }, c = "x/y\"\\\n中", d = 1.5, e = true, g = 3.0 }
        -- { 方法, url, 头表, 报文体 }；GET 走 http.get，POST 走 http.post
        local cases = {
            { "GET", "/", nil, nil },
            { "GET", "/a?b=c&d=%20", { ["X-A"] = "1" }, nil },
            { "GET", "/", {}, nil },
            { "GET", "/", { Host = "127.0.0.1:8080", ["Content-Type"] = "text/plain" }, nil },
            { "POST", "/", nil, nil },
            { "POST", "/", nil, "hello" },
            { "POST", "/", nil, "" },
            { "POST", "/", { ["X-A"] = "1", ["Content-Type"] = "text/plain" }, "body" },
            { "POST", "/p", nil, string.rep("p", 100000) },
            { "POST", "/", nil, jt },
            { "POST", "/", nil, {} },
            { "POST", "/", nil, { 1, 2, 3 } },
            { "POST", "/", { Authorization = "Bearer t", ["X-B"] = "2" }, { k = "v" } },
            { "GET", "/", _heads(16), nil },
            { "POST", "/", _heads(17), "body" },
            { "POST", "/", _heads(17), { k = "v" } },
        }
        local nmis = 0
        local cb, cc, lb
        for i, c in ipairs(cases) do
            if "GET" == c[1] then
                cb, cc = api_bytes(SOCK, http.get, c[2], c[3])
                lb = api_bytes("SK", http.get, c[2], c[3])
            else
                cb, cc = api_bytes(SOCK, http.post, c[2], c[3], nil, c[4])
                lb = api_bytes("SK", http.post, c[2], c[3], nil, c[4])
            end
            if 0 ~= cc or nil == cb or cb ~= lb then
                nmis = nmis + 1
                t:fail(string.format("pack_req 对拍 #%d: copy=%s C=%q Lua=%q", i, tostring(cc), tostring(cb), tostring(lb)))
            end
        end
        t:eq(0, nmis, "上面每种形状 http.get / post 都走 C 组包(copy=0)，且与 _http_msg 原路逐字节一致")
        -- 回落形状：数字头值 C 不接，http.post 照旧走原路(copy=1)，字节同原路
        local fb, fc = api_bytes(SOCK, http.post, "/", { ["X-N"] = 5 }, nil, "b")
        t:eq(1, fc, "C 不接的形状回落原路，copy=1")
        t:check(nil ~= fb and fb == api_bytes("SK", http.post, "/", { ["X-N"] = 5 }, nil, "b"), "回落的字节同原路")
        -- C 不接的形状返回 nil，交回原路（告警、丢头、放行帧长头、报错都在那边）
        local L = "POST / HTTP/1.1\r\n"
        t:eq(nil, srey_http.pack_req("SK", L, nil, "b"), "sk 不是连接标识 C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, nil, nil, "b"), "请求行不是字符串 C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, { ["X-N"] = 5 }, "b"), "数字头值 C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, setmetatable({}, {}), "b"), "带元表的头表 C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, "x", "b"), "头表不是表 C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, { ["Content-Length"] = "0" }, nil), "无体带 CL C 不接(原路放行)")
        t:eq(nil, srey_http.pack_req(SOCK, L, { ["transfer-ENCODING"] = "chunked" }, "b"), "帧长头 C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, { ["content-TYPE"] = "text/plain" }, { a = 1 }), "table 体带 CT C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, { X = "a\r\nb" }, "b"), "头值含 CRLF C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, { ["Bad Key"] = "x" }, "b"), "头名非 token C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, nil, { f = print }), "编不成 JSON 的 table 体 C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, nil, function() return nil end), "生产者函数体 C 不接")
        t:eq(nil, srey_http.pack_req(SOCK, L, nil, 5), "数字体 C 不接")
        -- 所有权：C 组好的缓冲交给真 syn_send(copy=0) 后，发送失败、挂不起抛错两条路都由框架释放
        -- （漏了退出时 _memcheck 报 not free，srey -d 退出码非 0）
        t:eq(nil, http.get(SOCK, "/"), "无效连接：缓冲由 ev_send 失败路径释放")
        local ok = true
        table.sort({ 2, 1 }, function(a, b)
            ok = pcall(http.get, SOCK, "/")
            return a < b
        end)
        t:eq(false, ok, "挂不起时 syn_send 先释放缓冲再抛")
    end

    -- 请求侧带头表：有 body 时帧长头由本模块写、调用方给的丢掉；无 body 时放行调用方给的，
    -- 一刀切会让 post(..., {Content-Length="0"}) 发出既无 CL 也无 TE 的请求。
    -- srey.syn_send 临时换成抓包并返回 nil，请求当失败返回，不碰网络
    do
        local function req_bytes(fn, ...)
            local cap
            local syn = srey.syn_send
            srey.syn_send = function(_, data)
                cap = data
                return nil
            end
            local ok = pcall(fn, ...)
            srey.syn_send = syn
            return ok and cap or ""
        end
        local function count(s, pat)
            return select(2, string.gsub(string.lower(s), pat, ""))
        end
        local w = req_bytes(http.post, "SK", "/", { ["Content-Length"] = "0", ["X-A"] = "1" })
        t:eq(1, count(w, "content%-length: 0\r\n"), "无 body 的请求放行调用方的 Content-Length")
        t:check(nil ~= w:find("X-A: 1\r\n", 1, true), "无 body 的请求普通头照发")
        t:check("\r\n\r\n" == w:sub(-4), "无 body 的请求到空行为止")
        w = req_bytes(http.post, "SK", "/",
                      { ["Content-Length"] = "999", ["transfer-ENCODING"] = "chunked", ["X-K"] = "1" }, nil, BODY)
        t:eq(1, count(w, "content%-length:"), "带 body 的请求只有本模块那条 Content-Length")
        local tail = "Content-Length: " .. #BODY .. "\r\n\r\n" .. BODY
        t:eq(tail, w:sub(-#tail), "长度按 body 算，body 紧随空行")
        t:eq(0, count(w, "transfer%-encoding"), "带 body 的请求丢掉调用方的 Transfer-Encoding")
        t:check(nil ~= w:find("X-K: 1\r\n", 1, true), "同批普通头未受牵连")
        w = req_bytes(http.post, "SK", "/", { ["Content-Type"] = "text/plain" }, nil, { a = 1 })
        t:eq(1, count(w, "content%-type:"), "table 体的请求只有本模块那条 Content-Type")
        t:check(nil ~= w:find("Content-Type: application/json\r\n", 1, true), "table 体的请求写 JSON 的 Content-Type")
        w = req_bytes(http.post, "SK", "/", { ["Transfer-Encoding"] = "gzip" }, nil, function() return nil end)
        t:eq(1, count(w, "transfer%-encoding:"), "生产者体的请求只有本模块那条 Transfer-Encoding")
        t:check(nil == w:find("gzip", 1, true), "生产者体的请求丢掉调用方的 Transfer-Encoding")
        w = req_bytes(http.get, "SK", "/", { ["X-N"] = 3600000 / 1000 })
        t:check(nil ~= w:find("X-N: 3600\r\n", 1, true), "请求头的整数值浮点按整数写")
    end

    -- HEAD：响应带 Content-Length 却无报文体，解包侧靠 core.http_set_method 才认得出。
    -- 漏登记的话这里会挂在等 1234 字节报文体上直到 netread 超时，拿到 nil
    local hlid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", HEAD_PORT)
    t:check(ERR_FAILED ~= hlid, "listen " .. HEAD_PORT)
    if ERR_FAILED ~= hlid then
        local head_sk
        head_sk = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", HEAD_PORT)
        t:check(head_sk and head_sk.valid, "head connect")
        if head_sk and head_sk.valid then
            local hp = http.head_req(head_sk, "/")
            t:check(nil ~= hp, "HEAD 拿到响应(没挂在等报文体上)")
            if hp then
                t:eq("v", hp.heads and hp.heads["X-Head"], "HEAD 响应头可读")
                t:check(nil == hp.data or 0 == #hp.data, "HEAD 响应无报文体")
            end
            -- 登记只对紧随那一条生效,第二次得重新登记(http.head_req 已代劳);同一连接连发验证这点
            t:check(nil ~= http.head_req(head_sk, "/again"), "同一连接第二次 HEAD 仍拿到响应")
            srey.close(head_sk)
        end

        -- 1xx 中间响应：服务端先回 103 再回 200（RFC 7231 §6.2，Cloudflare/Fastly 默认开
        -- 103 Early Hints）。C 侧把 1xx 当独立完整消息投出，客户端不跳过就会返回 103、
        -- 把 200 留在连接上，此后同一 keep-alive 连接每次请求都错一格
        local itm_sk
        itm_sk = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", HEAD_PORT)
        t:check(itm_sk and itm_sk.valid, "interim connect")
        if itm_sk and itm_sk.valid then
            local ip = http.get(itm_sk, INTERIM_URI)
            t:check(nil ~= ip, "1xx 之后拿到最终响应")
            if ip then
                t:eq("200", ip.status and ip.status[2], "跳过 103，返回的是 200 而不是中间响应")
                t:eq("hello", ip.data, "最终响应的 body 完整")
            end
            srey.close(itm_sk)
        end

        -- 既无 Content-Length 又无 Transfer-Encoding 的响应(RFC 7230 §3.3.3 规则 7)：C 侧按分片投，
        -- 末片由关闭事件补，客户端与 chunked 同走 syn_slice 循环
        local tc_sk = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", HEAD_PORT)
        t:check(tc_sk and tc_sk.valid, "tillclose connect")
        if tc_sk and tc_sk.valid then
            local tp = http.get(tc_sk, TILLCLOSE_URI)
            t:check(nil ~= tp, "由连接关闭界定的响应收到")
            if tp then
                t:eq(0, tp.chunked, "无 TE 时 chunked 为 0")
                t:eq("1", tp.heads and tp.heads["X-Till"], "响应头可读")
                t:eq(TILLCLOSE_BODY, tp.data, "body 收到连接关闭为止")
                t:eq(#TILLCLOSE_BODY, tp.cksize, "cksize 按分片累加")
            end
            srey.close(tc_sk)
        end
        srey.unlisten(hlid)
    end

    srey.close(cli_sk)
    srey.unlisten(lid)-- 释放端口给后续测试
end)
end)
