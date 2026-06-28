-- HTTP 测试服务端（明文 15002 + SSL 15443），行为对齐 C 层 test/task_http_server.c，
-- 供 bin/py_assist/test_http.py / test_ssl_reneg.py e2e 用例直接连接，不上报 reporter（常驻监听）。
-- 协议：
--   非分片 POST 请求 -> 回显 body
--   非分片 GET 或无 body -> 200 "ok"
--   chunked 请求 -> PROT_SLICE_END 到达后回三帧 chunked: "a", "b", 终止块

local srey = require("lib.srey")
local http = require("lib.http")

local _PORT = 15002
local _SSL_PORT = 15443
-- 大下行端点 body：固定 760000 字节(<1MB 避免 wb tda 告警)，制造长发送窗口，
-- 供 test_ssl_reneg 在服务端发送中穿插 KeyUpdate 命中 IOCP 发侧接力(overlap.c)
local _DOWN_BODY = string.rep("D", 760000)

local function _gen_chunked(state)
    state.n = state.n + 1
    if 1 == state.n then return "a" end
    if 2 == state.n then return "b" end
    return nil    -- 触发 0\r\n\r\n 终止块
end

srey.startup(function()
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        if 0 ~= slice then
            -- 分片请求：到 PROT_SLICE_END 才回复
            -- SLICE 标志位：START=1 SLICE=2 END=4，按位 AND 判断
            if 0 ~= (slice & 4) then
                http.response(fd, skid, 200, nil, _gen_chunked, { n = 0 })
            end
            return
        end
        -- 非分片请求
        local st = http.status(data)
        if st and st[2] and st[2]:match("^/down") then
            -- 大下行：服务端主动回大 body，供客户端在读流期间穿插 KeyUpdate
            http.response(fd, skid, 200, nil, _DOWN_BODY)
            return
        end
        -- 有 body 回显，否则回 "ok"
        local body = http.datastr(data)
        if body and #body > 0 then
            http.response(fd, skid, 200, nil, body)
        else
            http.response(fd, skid, 200, nil, "ok")
        end
    end)
    if ERR_FAILED == srey.listen(PACK_TYPE.HTTP, SSL_NAME.NONE, "0.0.0.0", _PORT) then
        WARN("server_http listen %d error", _PORT)
        return
    end
    printd("server_http listening on %d", _PORT)
    if ERR_FAILED == srey.listen(PACK_TYPE.HTTP, SSL_NAME.SERVER, "0.0.0.0", _SSL_PORT) then
        WARN("server_http SSL listen %d error", _SSL_PORT)
    else
        printd("server_http SSL listening on %d", _SSL_PORT)
    end
end)
