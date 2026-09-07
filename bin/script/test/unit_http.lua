-- lib.http 客户端 chunked 违约路径:生产者返回非 string 时,http.post 须先把对端必回的响应收掉再返回 nil。
-- 漏收则残留响应会被同一连接上的下一次请求错认——_wait_net_recv 按 skid 匹配,不区分是哪次请求。
-- 同一 task 内起 server(PACK_HTTP 监听)与 client(连回本机):server 对分片请求回固定串,非分片请求回显 body。
-- 另覆盖客户端 chunked 接收段(lib/http.lua 的 srey.syn_slice 循环):server 回真 chunked 响应,
-- client 分传 / 不传 ckfunc 两种走法。

local srey   = require("lib.srey")
local runner = require("test.runner")
local http   = require("lib.http")
local srey_http = require("srey.http")-- C 绑定层,直接断言 is_token / max_headlens

local PORT = 15047
-- HEAD 用例的裸监听端口。测试模块并发跑,端口必须全仓唯一——15048 是 unit_websock 的,
-- 别用 PORT+1 这种推导写法,新端口先 grep 再定
local HEAD_PORT = 15049
local CK_RSP = "chunked-done"
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
local N204 = "want204"-- 触发 server 回 204+body，验证 body 与 Content-Length 都被丢弃
local BADBODY = "wantbadbody"-- 触发 server 回一个类型不受支持的 body（数字），验证告警确实打出来
-- 触发 server 回真 chunked 响应(生产者吐两块)。lib.http 的接收段原先零覆盖:现有 /chunked
-- 那条是 HEAD,headonly 下生产者一次都不调,走不到 srey.syn_slice 那个循环
local CKBODY = "wantckbody"
local CK1 = "chunk-one"
local CK2 = "chunk-two-longer"

-- WARN 打桩，用来断言"响应带了不支持的 body 类型时确实告警"。
-- 每个 unit 模块是独立 task（各有各的 lua_State），改全局波及不到别的模块。
-- 代价是本模块日志里的文件/行号会偏一层——_log 用 debug.getinfo(3) 取调用位置，这里多垫了一帧
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

-- 第 1 块正常发出(让对端收到一条语法完整的 chunked 请求),第 2 块返回非 string 触发违约
local function _bad_producer(state)
    state.n = state.n + 1
    if 1 == state.n then
        return "aa"
    end
    return 42
end

-- 裸连接（PACK_TYPE.NONE 不分帧）上发一条请求并累积到出现 term 为止。
-- 一次 RECV 未必是整条响应，只取第一段的话会把"没收全"误报成"服务端过滤掉了"
---@param fd integer
---@param skid integer
---@param req string 完整请求报文
---@param term string 收齐判据（如 "\r\n\r\n"）
---@return string txt 累积到的全部字节
local function _raw_probe(fd, skid, req, term)
    local txt = ""
    local rsp, rlen = srey.syn_send(fd, skid, req, #req, 1)
    while rsp do
        txt = txt .. srey.ud_str(rsp, rlen)
        if string.find(txt, term, 1, true) then
            break
        end
        rsp, rlen = srey.syn_recv(fd, skid)
    end
    return txt
end

srey.startup(function()
runner.run(function(t)
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        -- 只处理 accept 来的连接;客户端侧响应由 syn_send 的等待者接走。
        -- 不能按 fd 值排除客户端 socket:客户端 fd 关掉后号会被后续 accept 复用,
        -- 闭包里的旧值就把服务端自己的 socket 当成客户端放过去,于是永不回应、对端等满超时
        if 0 ~= client then
            return
        end
        if PACK_TYPE.NONE == pktype then
            -- 裸监听侧，两个用例共用：按请求目标分流
            if nil ~= string.find(srey.ud_str(data, size), INTERIM_URI, 1, true) then
                srey.send(fd, skid, INTERIM_RSP, #INTERIM_RSP, 1)
            else
                srey.send(fd, skid, HEAD_RSP, #HEAD_RSP, 1)
            end
            return
        end
        if 0 ~= slice then
            if 0 ~= (slice & 4) then-- PROT_SLICE_END:分片请求收齐才回
                http.response(fd, skid, 200, nil, CK_RSP)
            end
            return
        end
        local st = http.status(data)
        if st and "HEAD" == st[1] then
            if "/chunked" == st[2] then
                -- body 是生产者函数：长度未知省掉 CL，但 GET 会发的 TE 得照发
                http.response_head(fd, skid, 200, nil, function() return nil end)
            else
                -- info 传 nil：验 HEAD 分支照样补 Content-Length: 0
                http.response_head(fd, skid, 200, { ["X-Head-Probe"] = "v" }, nil)
            end
            return
        end
        local body = http.datastr(data)
        if PROBE == body then
            -- 六种头一次过：合法 token / 值含 CRLF / 名非 token / 顶过 HTTP_MAX_HEADLENS /
            -- 整数值浮点 / __tostring 会抛的值；且不带 body。
            -- 最后那个若照旧走 tostring，异常会越过整条校验链把这次响应整个吞掉
            http.response(fd, skid, 200, { ["X-Ok"] = "v", ["X-Inj"] = INJ, ["Bad Key"] = "x", ["X-Big"] = BIG,
                                           ["X-Num"] = 3600000 / 1000,
                                           ["X-Raiser"] = setmetatable({}, { __tostring = function() error("boom") end }) })
            return
        end
        if FRAME == body then
            -- 带 body 的响应，调用方同时塞进本函数自己会生成的两条帧长头。
            -- 大小写故意写乱：HTTP 头名大小写无关，判定不能只认标准写法。
            -- 放行的话线缆上会是 TE 叠一条自造 CL 再叠调用方那条 CL，走私形态
            http.response(fd, skid, 200,
                { ["transfer-ENCODING"] = "chunked", ["Content-Length"] = "999", ["X-Keep"] = "1" }, BODY)
            return
        end
        if N204 == body then
            -- 204 禁带报文体：故意塞一个 body，验证被 http.response 丢弃
            http.response(fd, skid, 204, nil, "dropped")
            return
        end
        if CKBODY == body then
            -- info 传函数即 chunked:每次调它取一块,返 nil 终止
            http.response(fd, skid, 200, nil, _ck_producer, { n = 0 })
            return
        end
        if BADBODY == body then
            -- body 是数字：三个 body 分支都不命中，按无 body 发走并告警
            http.response(fd, skid, 200, nil, 42)
            return
        end
        http.response(fd, skid, 200, nil, (body and #body > 0) and body or "ok")
    end)

    local lid = srey.listen(PACK_TYPE.HTTP, SSL_NAME.NONE, "0.0.0.0", PORT)
    t:check(ERR_FAILED ~= lid, "listen " .. PORT)
    if ERR_FAILED == lid then
        return
    end
    local cli_fd, cli_skid
    cli_fd, cli_skid = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", PORT)
    t:check(cli_fd and INVALID_SOCK ~= cli_fd, "connect")
    if not cli_fd or INVALID_SOCK == cli_fd then
        srey.unlisten(lid)
        return
    end

    t:eq(nil, http.post(cli_fd, cli_skid, "/", nil, nil, _bad_producer, { n = 0 }), "生产者违约返回 nil")
    -- 关键断言:同一连接的下一次请求必须拿到属于自己的响应。上一条响应未被收掉时,
    -- 它会被本次注册的等待者接走,这里拿到的是 CK_RSP 而非 BODY
    local pack = http.post(cli_fd, cli_skid, "/", nil, nil, BODY)
    t:check(nil ~= pack, "后续请求拿到响应")
    if pack then
        t:eq(BODY, pack.data, "响应属于本次请求(未错认上一条残留)")
    end

    -- url 里塞 CRLF 就是请求拆分：放行的话对端会看到两条完整请求、回两条响应，
    -- 多出来那条被下一次请求的等待者接走，连接从此错开一位。
    -- 必须在写 socket 之前就拒，故断言"下一次正常请求仍拿到属于自己的响应"
    t:eq(nil, http.get(cli_fd, cli_skid, URL_INJ), "url 含 CRLF 的 GET 被拒")
    t:eq(nil, http.post(cli_fd, cli_skid, URL_INJ, nil, nil, BODY), "url 含 CRLF 的 POST 被拒")
    t:eq(nil, http.get(cli_fd, cli_skid, "/x\0y"), "url 含 NUL 被拒")
    local after = http.post(cli_fd, cli_skid, "/", nil, nil, PROBE2)
    t:check(nil ~= after, "被拒后连接仍可用(一个字节都没写出去)")
    if after then
        t:eq(PROBE2, after.data, "拒绝的请求未在连接上留下任何残留")
    end

    -- lib.http 的 chunked 接收段:不传 ckfunc 时逐块累积再拼进 pack.data
    local ckp = http.post(cli_fd, cli_skid, "/", nil, nil, CKBODY)
    t:check(nil ~= ckp, "chunked 响应收到")
    if ckp then
        t:eq(CK1 .. CK2, ckp.data, "两块 chunk 按序拼进 pack.data")
        t:eq(#CK1 + #CK2, ckp.cksize, "cksize 累加两块的字节数")
    end
    -- 传 ckfunc 时逐块回调、不落 pack.data。只断言最后一次回调的 fin 而不数回调次数:
    -- 终止块是否单独成一次回调由 C 侧的分片边界决定,不该在这里锁死
    local got, fins = {}, {}
    local ckp2 = http.post(cli_fd, cli_skid, "/", nil, function(fin, d, sz)
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
    local raw_fd, raw_skid
    raw_fd, raw_skid = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", PORT)
    t:check(raw_fd and INVALID_SOCK ~= raw_fd, "raw connect")
    if raw_fd and INVALID_SOCK ~= raw_fd then
        local req = string.format("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n\r\n%s",
                                  #PROBE, PROBE)
        -- PACK_TYPE.NONE 不分帧,一次 RECV 未必是整条响应；累积到空行(头结束)为止再断言。
        -- 直接拿第一段就断言的话,被 TCP 切开时会把"没收全"误报成"注入头已被过滤"
        local txt = _raw_probe(raw_fd, raw_skid, req, "\r\n\r\n")
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
        local ftxt = _raw_probe(raw_fd, raw_skid, freq, "\r\n\r\n" .. BODY)
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
        local ntxt = _raw_probe(raw_fd, raw_skid, nreq, "\r\n\r\n")
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
        local btxt = _raw_probe(raw_fd, raw_skid, breq, "\r\n\r\n")
        t:check(nil ~= string.find(btxt, " 200 ", 1, true), "不支持的 body 类型仍回 200")
        t:check(nil ~= string.find(btxt, "Content-Length: 0", 1, true),
                "不支持的 body 类型按无 body 发出(Content-Length: 0)")
        t:check(nil == string.find(btxt, "42", 1, true), "那个数字没被当成 body 发出去")
        t:check(_warned("unsupported body type"), "不支持的 body 类型打出了告警")
        -- 同一条裸连接再来一次：HEAD + info 为 nil。头必须与同一资源的 GET 一致，
        -- GET 那条走"响应无 body"分支会写 Content-Length: 0，HEAD 不能因为不发体就把它省掉
        local hreq = "HEAD / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
        local htxt = _raw_probe(raw_fd, raw_skid, hreq, "\r\n\r\n")
        t:check(nil ~= string.find(htxt, " 200 ", 1, true), "HEAD 探针收到 200 响应")
        t:check(nil ~= string.find(htxt, "Content-Length: 0", 1, true),
                "HEAD 无 body 时照写 Content-Length: 0(与同资源的 GET 一致)")
        t:check(nil ~= string.find(htxt, "X-Head-Probe: v", 1, true), "HEAD 响应的普通头照常发出")
        -- 再来一次：body 是生产者函数。长度未知省掉 CL 是有意的，但 GET 那条会发
        -- Transfer-Encoding，HEAD 省掉它两个方法的头就对不上了
        local creq = "HEAD /chunked HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
        local ctxt = _raw_probe(raw_fd, raw_skid, creq, "\r\n\r\n")
        t:check(nil ~= string.find(ctxt, " 200 ", 1, true), "HEAD 生产者函数体收到 200 响应")
        t:check(nil ~= string.find(ctxt, "Transfer-Encoding: chunked", 1, true),
                "HEAD 的生产者函数体照发 Transfer-Encoding(与同资源的 GET 一致)")
        t:check(nil == string.find(ctxt, "Content-Length", 1, true),
                "长度未知时不写 Content-Length")
        srey.close(raw_fd, raw_skid)
    end

    -- token 判定与头部块上限都取自 C，不再在 Lua 重抄一份
    t:check(srey_http.is_token("X-Ok"), "is_token 认合法 token")
    t:check(not srey_http.is_token("Bad Key"), "is_token 拒含空格的头名")
    t:check(not srey_http.is_token(""), "is_token 拒空串")
    t:check(not srey_http.is_token(1), "is_token 拒非字符串(数字当不了头名)")
    t:eq(4096, srey_http.max_headlens, "max_headlens 取自 prots_pub.h 的 HTTP_MAX_HEADLENS")
    -- 组包侧不按它判(见上面 X-Big 那条)，这里只确认常量值没与 C 分叉

    -- HEAD：响应带 Content-Length 却无报文体，解包侧靠 core.http_set_method 才认得出。
    -- 漏登记的话这里会挂在等 1234 字节报文体上直到 netread 超时，拿到 nil
    local hlid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", HEAD_PORT)
    t:check(ERR_FAILED ~= hlid, "listen " .. HEAD_PORT)
    if ERR_FAILED ~= hlid then
        local head_fd, head_skid
        head_fd, head_skid = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", HEAD_PORT)
        t:check(head_fd and INVALID_SOCK ~= head_fd, "head connect")
        if head_fd and INVALID_SOCK ~= head_fd then
            local hp = http.head_req(head_fd, head_skid, "/")
            t:check(nil ~= hp, "HEAD 拿到响应(没挂在等报文体上)")
            if hp then
                t:eq("v", hp.heads and hp.heads["X-Head"], "HEAD 响应头可读")
                t:check(nil == hp.data or 0 == #hp.data, "HEAD 响应无报文体")
            end
            -- 登记只对紧随那一条生效,第二次得重新登记(http.head_req 已代劳);同一连接连发验证这点
            t:check(nil ~= http.head_req(head_fd, head_skid, "/again"), "同一连接第二次 HEAD 仍拿到响应")
            srey.close(head_fd, head_skid)
        end

        -- 1xx 中间响应：服务端先回 103 再回 200（RFC 7231 §6.2，Cloudflare/Fastly 默认开
        -- 103 Early Hints）。C 侧把 1xx 当独立完整消息投出，客户端不跳过就会返回 103、
        -- 把 200 留在连接上，此后同一 keep-alive 连接每次请求都错一格
        local itm_fd, itm_skid
        itm_fd, itm_skid = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", HEAD_PORT)
        t:check(itm_fd and INVALID_SOCK ~= itm_fd, "interim connect")
        if itm_fd and INVALID_SOCK ~= itm_fd then
            local ip = http.get(itm_fd, itm_skid, INTERIM_URI)
            t:check(nil ~= ip, "1xx 之后拿到最终响应")
            if ip then
                t:eq("200", ip.status and ip.status[2], "跳过 103，返回的是 200 而不是中间响应")
                t:eq("hello", ip.data, "最终响应的 body 完整")
            end
            srey.close(itm_fd, itm_skid)
        end
        srey.unlisten(hlid)
    end

    srey.close(cli_fd, cli_skid)
    srey.unlisten(lid)-- 释放端口给后续测试
end)
end)
