-- HTTP 测试服务端（监听 15002），行为对齐 C 层 test/task_http_server.c，
-- 供 bin/py_assist/test_http.py e2e 用例直接连接，不上报 reporter（常驻监听）。
-- 协议：
--   非分片 POST 请求 -> 回显 body
--   非分片 GET 或无 body -> 200 "ok"
--   chunked 请求 -> PROT_SLICE_END 到达后回三帧 chunked: "a", "b", 终止块

local srey = require("lib.srey")
local http = require("lib.http")

local _PORT = 15002

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
        -- 非分片请求：有 body 回显，否则回 "ok"
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
end)
