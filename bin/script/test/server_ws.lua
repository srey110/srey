-- WebSocket 测试服务端（监听 15003），行为对齐 C 层 test/task_ws_server.c，
-- 供 bin/py_assist/test_ws.py e2e 用例直接连接。
-- 协议：
--   非分片 TEXT/BINARY -> 同 prot 回显
--   非分片 PING -> PONG
--   非分片 CLOSE -> 主动 close
--   分片消息 -> PROT_SLICE_END 后回三帧分片消息: text(fin=0,"a") + continua(fin=0,"b") + continua(fin=1,"c")
--   MQTT 子协议帧 -> 收到 CONNECT 回 CONNACK

local srey    = require("lib.srey")
local websock = require("srey.websock")
local cmqtt   = require("srey.mqtt")
local utils   = require("srey.utils")

local _PORT = 15003
-- WEBSOCK_PROT 常量（RFC 6455 §11.8 opcode）：lib/websock.lua 中通过全局赋值导出，
-- 但 lib/websock.lua 顶层会 require lib.dns，引入 DNS 查询副作用；server 端不需要客户端封装，
-- 直接在本文件 local 化常量值
local WS_TEXT, WS_BINARY, WS_PING, WS_CLOSE = 0x01, 0x02, 0x09, 0x08
local MQTT_CONNECT = 0x01 -- 同上，避免为一个常量 require lib.mqtt

srey.startup(function()
    srey.on_recved(function(pktype, sk, client, slice, data, size)
        if 0 ~= slice then
            if 0 ~= (slice & 4) then -- PROT_SLICE_END
                local frame, fsize = websock.pack_text(0, 0, "a")
                srey.send(sk, frame, fsize, 0)
                frame, fsize = websock.pack_continua(0, 0, "b")
                srey.send(sk, frame, fsize, 0)
                frame, fsize = websock.pack_continua(0, 1, "c")
                srey.send(sk, frame, fsize, 0)
            end
            return
        end
        local pack = websock.unpack(data)
        -- MQTT over WS：子协议帧的载荷在 secpack 里，收到 CONNECT 按客户端声明的版本回 CONNACK。
        -- secprot 每帧都带，控制帧与零长帧的 secpack 为 nil，按 secpack 分流才不会把 ping / close
        -- 一起吞掉；与 C 侧 test/task_ws_server.c 同一套分流
        if PACK_TYPE.MQTT == pack.secprot and pack.secpack then
            if MQTT_CONNECT == cmqtt.prot(pack.secpack) then
                local ack, alens = cmqtt.pack_connack(cmqtt.pack_version(pack.secpack), 0, 0)
                if ack then
                    local frame, fsize = websock.pack_binary(0, 1, ack, alens)
                    if frame then
                        srey.send(sk, frame, fsize, 0)
                    end
                    utils.ud_free(ack)
                end
            end
            return
        end
        if WS_TEXT == pack.prot then
            local frame, fsize = websock.pack_text(0, 1, pack.data, pack.size)
            srey.send(sk, frame, fsize, 0)
        elseif WS_BINARY == pack.prot then
            local frame, fsize = websock.pack_binary(0, 1, pack.data, pack.size)
            srey.send(sk, frame, fsize, 0)
        elseif WS_PING == pack.prot then
            local frame, fsize = websock.pack_pong(0)
            srey.send(sk, frame, fsize, 0)
        elseif WS_CLOSE == pack.prot then
            srey.close(sk)
        end
    end)
    if ERR_FAILED == srey.listen(PACK_TYPE.WEBSOCK, SSL_NAME.NONE, "0.0.0.0", _PORT) then
        WARN("server_ws listen %d error", _PORT)
        return
    end
    printd("server_ws listening on %d", _PORT)
end)
