-- lib.http 客户端 chunked 违约路径:生产者返回非 string 时,http.post 须先把对端必回的响应收掉再返回 nil。
-- 漏收则残留响应会被同一连接上的下一次请求错认——_wait_net_recv 按 skid 匹配,不区分是哪次请求。
-- 同一 task 内起 server(PACK_HTTP 监听)与 client(连回本机):server 对分片请求回固定串,非分片请求回显 body。

local srey   = require("lib.srey")
local runner = require("test.runner")
local http   = require("lib.http")
local srey_http = require("srey.http")-- C 绑定层,直接断言 is_token / max_headlens

local PORT = 15047
local CK_RSP = "chunked-done"
local BODY = "hello"
local PROBE = "hdrprobe"
local FRAME = "frameprobe"
local INJ = "a\r\nX-Evil: 1"-- 头值里塞 CRLF：未过滤时会把一条响应劈成两条
local URL_INJ = "/x HTTP/1.1\r\nX-Evil: 1\r\n\r\nGET /y"-- 请求目标里塞 CRLF：未过滤时线缆上是两条请求
local PROBE2 = "afterinj"-- 拒绝之后的回显探针，验证连接没被污染
local N204 = "want204"-- 触发 server 回 204+body，验证 body 与 Content-Length 都被丢弃
-- 名 + ": " + 值 + CRLF 超 MAX_HEADLENS(4096)：整条头会被丢弃，不截断也不发出
local BIG = string.rep("b", 4096)

-- 第 1 块正常发出(让对端收到一条语法完整的 chunked 请求),第 2 块返回非 string 触发违约
local function _bad_producer(state)
    state.n = state.n + 1
    if 1 == state.n then
        return "aa"
    end
    return 42
end

srey.startup(function()
runner.run("http_client", function(t)
    local cli_fd, raw_fd
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        if fd == cli_fd or fd == raw_fd then
            return-- 客户端侧响应由 syn_send 的等待者接走;万一漏收落到这里也不回应,免污染断言
        end
        if 0 ~= slice then
            if 0 ~= (slice & 4) then-- PROT_SLICE_END:分片请求收齐才回
                http.response(fd, skid, 200, nil, CK_RSP)
            end
            return
        end
        local body = http.datastr(data)
        if PROBE == body then
            -- 四种头一次过：合法 token / 值含 CRLF / 名非 token / 超 MAX_HEADLENS；且不带 body
            http.response(fd, skid, 200, { ["X-Ok"] = "v", ["X-Inj"] = INJ, ["Bad Key"] = "x", ["X-Big"] = BIG })
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
        http.response(fd, skid, 200, nil, (body and #body > 0) and body or "ok")
    end)

    local lid = srey.listen(PACK_TYPE.HTTP, SSL_NAME.NONE, "0.0.0.0", PORT)
    t:check(ERR_FAILED ~= lid, "listen " .. PORT)
    if ERR_FAILED == lid then
        return
    end
    local cli_skid
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

    -- 裸连接读原始字节：srey 自己的解析器对"无 CL 无 chunked"是宽容的，
    -- 用解析后的 pack 断言区分不出 Content-Length 有没有发出去，只能看线缆上的字节
    local raw_skid
    raw_fd, raw_skid = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", PORT)
    t:check(raw_fd and INVALID_SOCK ~= raw_fd, "raw connect")
    if raw_fd and INVALID_SOCK ~= raw_fd then
        local req = string.format("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n\r\n%s",
                                  #PROBE, PROBE)
        -- PACK_TYPE.NONE 不分帧,一次 RECV 未必是整条响应；累积到空行(头结束)为止再断言。
        -- 直接拿第一段就断言的话,被 TCP 切开时会把"没收全"误报成"注入头已被过滤"
        local txt = ""
        local rsp, rlen = srey.syn_send(raw_fd, raw_skid, req, #req, 1)
        while rsp do
            txt = txt .. srey.ud_str(rsp, rlen)
            if string.find(txt, "\r\n\r\n", 1, true) then
                break
            end
            rsp, rlen = srey.syn_recv(raw_fd, raw_skid)
        end
        t:check(nil ~= string.find(txt, "\r\n\r\n", 1, true), "raw 探针收到完整响应头")
        if string.find(txt, "\r\n\r\n", 1, true) then
            t:check(nil == string.find(txt, "X-Evil", 1, true), "值含 CRLF 的头被丢弃,报文未被劈成两条")
            t:check(nil == string.find(txt, "Bad Key", 1, true), "名非 RFC7230 token 的头被丢弃")
            t:check(nil == string.find(txt, "X-Big", 1, true), "超 MAX_HEADLENS 的头整条丢弃(未截断发出)")
            t:check(nil ~= string.find(txt, "X-Ok: v", 1, true), "合法头正常发出(未误伤)")
            t:check(nil ~= string.find(txt, "Content-Length: 0", 1, true), "无 body 的响应带 Content-Length: 0")
        end
        -- 同一条裸连接再发一次：带 body 且调用方自带帧长头，验证不会造出走私形态。
        -- 这次要读到 body 结束(空行 + BODY)，不能只读到头结束
        local freq = string.format("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %d\r\n\r\n%s",
                                   #FRAME, FRAME)
        local ftxt = ""
        local frsp, frlen = srey.syn_send(raw_fd, raw_skid, freq, #freq, 1)
        while frsp do
            ftxt = ftxt .. srey.ud_str(frsp, frlen)
            if nil ~= string.find(ftxt, "\r\n\r\n" .. BODY, 1, true) then
                break
            end
            frsp, frlen = srey.syn_recv(raw_fd, raw_skid)
        end
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
        local ntxt = ""
        local nrsp, nrlen = srey.syn_send(raw_fd, raw_skid, nreq, #nreq, 1)
        while nrsp do
            ntxt = ntxt .. srey.ud_str(nrsp, nrlen)
            if string.find(ntxt, "\r\n\r\n", 1, true) then
                break
            end
            nrsp, nrlen = srey.syn_recv(raw_fd, raw_skid)
        end
        t:check(nil ~= string.find(ntxt, " 204 ", 1, true), "204 探针收到 204 响应")
        if string.find(ntxt, "\r\n\r\n", 1, true) then
            t:check(nil == string.find(string.lower(ntxt), "content%-length"),
                    "204 响应不带 Content-Length")
            t:check(nil == string.find(ntxt, "dropped", 1, true), "204 的 body 被丢弃")
        end
        srey.close(raw_fd, raw_skid)
    end

    -- token 判定与头部块上限都取自 C，不再在 Lua 重抄一份
    t:check(srey_http.is_token("X-Ok"), "is_token 认合法 token")
    t:check(not srey_http.is_token("Bad Key"), "is_token 拒含空格的头名")
    t:check(not srey_http.is_token(""), "is_token 拒空串")
    t:check(not srey_http.is_token(1), "is_token 拒非字符串(数字当不了头名)")
    t:eq(4096, srey_http.max_headlens, "max_headlens 取自 http.h 的 MAX_HEADLENS")
    -- 注：请求侧不再受 MAX_HEADLENS 约束这条没法在这里验——srey 的解析器对请求同样按
    -- MAX_HEADLENS 判，测试服务端就是 srey，超限的请求头它自己就拒收了；而"旧代码会丢弃"
    -- 与"srey 会拒收"用的是同一个 4096，不存在能区分两者的尺寸。要验得对着 nginx 之类跑

    srey.close(cli_fd, cli_skid)
    srey.unlisten(lid)-- 释放端口给后续测试
end)
end)
