-- WebSocket 测试服务端（监听 15003），行为对齐 C 层 test/task_ws_server.c，
-- 供 bin/py_assist/test_ws.py e2e 用例直接连接。
-- 协议：
--   非分片 TEXT/BINARY -> 同 prot 回显
--   非分片 PING -> PONG
--   非分片 CLOSE -> 主动 close
--   分片消息 -> PROT_SLICE_END 后回三帧分片消息: text(fin=0,"a") + continua(fin=0,"b") + continua(fin=1,"c")

local srey    = require("lib.srey")
local websock = require("srey.websock")

local _PORT = 15003
-- WEBSOCK_PROT 常量（RFC 6455 §11.8 opcode）：lib/websock.lua 中通过全局赋值导出，
-- 但 lib/websock.lua 顶层会 require lib.dns，引入 DNS 查询副作用；server 端不需要客户端封装，
-- 直接在本文件 local 化常量值
local WS_TEXT, WS_BINARY, WS_PING, WS_CLOSE = 0x01, 0x02, 0x09, 0x08

srey.startup(function()
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        if 0 ~= slice then
            if 0 ~= (slice & 4) then  -- PROT_SLICE_END
                local frame, fsize = websock.pack_text(0, 0, "a")
                srey.send(fd, skid, frame, fsize, 0)
                frame, fsize = websock.pack_continua(0, 0, "b")
                srey.send(fd, skid, frame, fsize, 0)
                frame, fsize = websock.pack_continua(0, 1, "c")
                srey.send(fd, skid, frame, fsize, 0)
            end
            return
        end
        local pack = websock.unpack(data)
        if WS_TEXT == pack.prot then
            local frame, fsize = websock.pack_text(0, 1, pack.data, pack.size)
            srey.send(fd, skid, frame, fsize, 0)
        elseif WS_BINARY == pack.prot then
            local frame, fsize = websock.pack_binary(0, 1, pack.data, pack.size)
            srey.send(fd, skid, frame, fsize, 0)
        elseif WS_PING == pack.prot then
            local frame, fsize = websock.pack_pong(0)
            srey.send(fd, skid, frame, fsize, 0)
        elseif WS_CLOSE == pack.prot then
            srey.close(fd, skid)
        end
    end)
    if ERR_FAILED == srey.listen(PACK_TYPE.WEBSOCK, SSL_NAME.NONE, "0.0.0.0", _PORT) then
        WARN("server_ws listen %d error", _PORT)
        return
    end
    printd("server_ws listening on %d", _PORT)
end)
