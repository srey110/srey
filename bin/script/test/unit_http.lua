-- lib.http 客户端 chunked 违约路径:生产者返回非 string 时,http.post 须先把对端必回的响应收掉再返回 nil。
-- 漏收则残留响应会被同一连接上的下一次请求错认——_wait_net_recv 按 skid 匹配,不区分是哪次请求。
-- 同一 task 内起 server(PACK_HTTP 监听)与 client(连回本机):server 对分片请求回固定串,非分片请求回显 body。

local srey   = require("lib.srey")
local runner = require("test.runner")
local http   = require("lib.http")

local PORT = 15047
local CK_RSP = "chunked-done"
local BODY = "hello"

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
    local cli_fd
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        if fd == cli_fd then
            return-- 客户端侧响应由 syn_send 的等待者接走;万一漏收落到这里也不回应,免污染断言
        end
        if 0 ~= slice then
            if 0 ~= (slice & 4) then-- PROT_SLICE_END:分片请求收齐才回
                http.response(fd, skid, 200, nil, CK_RSP)
            end
            return
        end
        local body = http.datastr(data)
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

    srey.close(cli_fd, cli_skid)
    srey.unlisten(lid)-- 释放端口给后续测试
end)
end)
