-- harbor 跨节点 RPC 的 Lua 客户端(srey.net_call / srey.net_request / 内部的 _net_rpc)。
-- 本进程自己就起着 harbor(srey/startup.c 末尾无条件 harbor_start),故连回本机、dst 用自己的
-- 句柄:请求经 harbor 绕一圈回到本 task 的 on_requested,响应再原路回来。harbor 是独立 task,
-- 本协程挂在 syn_send 上不挡自己的消息派发,故不会自锁。
-- 刻意不借 multi_call_sub 当靶:它 reqtype=100 会 srey.call 回 multi_call,给那个模块塞一条
-- 额外 ack、打乱它的计数断言。
-- 连接要有界重试:harbor_start 排在 ltask_startup 之后,本 task 可能先起来(见下)。

local srey   = require("lib.srey")
local runner = require("test.runner")

-- 与 bin/configs/config.json 的 harbor.port 一致;Lua 侧读不到配置,改那边要跟着改这里
local HARBOR_PORT = 8080
local REQ = 300-- 业务 reqtype,避开 REQUEST_TYPE 里的框架保留值
local PAYLOAD = "harbor-rpc"
local ECHO = "harbor-echo"

srey.startup(function()
runner.run(function(t)
    local ncall = 0
    srey.on_requested(function(reqtype, sess, src, data, size)
        if REQ ~= reqtype then
            return
        end
        ncall = ncall + 1
        if src ~= TASK_NAME.NONE and sess ~= 0 then
            srey.response(src, reqtype, sess, 0, ECHO)
        end
    end)

    -- 有界重试而不是盲等固定毫秒:srey/startup.c 的 harbor_start 排在 ltask_startup 之后,
    -- 本 task 的 startup 完全可能先跑到这里(实测早 13ms)。成因同 project_mqtt_test_startup_flake
    -- 记的那条 —— _startup 的派发顺序不代表对端 listen 已完成
    local sk
    for _ = 1, 20 do
        sk = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", HARBOR_PORT)
        if sk and sk.valid then
            break
        end
        srey.sleep(50)
    end
    t:check(sk and sk.valid, "连上本机 harbor " .. HARBOR_PORT)
    if not sk or not sk.valid then
        return
    end
    local self_h = srey.task_handle()

    -- dst 必须是数字句柄:传本地名字直接早退,一个字节都不发
    t:eq(false, srey.net_call(sk, "framework", REQ, PAYLOAD), "net_call 的 dst 传名字被拒")
    t:eq(false, srey.net_request(sk, "framework", REQ, PAYLOAD), "net_request 的 dst 传名字被拒")

    -- 单向 call:对端 harbor 回 200,不带负载
    t:eq(true, srey.net_call(sk, self_h, REQ, PAYLOAD), "net_call 返 200")

    -- 同步 request:on_requested 里 srey.response 的负载经 harbor 原路回来
    local ok, rdata, rsize = srey.net_request(sk, self_h, REQ, PAYLOAD)
    t:eq(true, ok, "net_request 返 200")
    t:eq(ECHO, rdata and srey.ud_str(rdata, rsize) or nil, "响应负载原路回来")

    -- 框架保留的 reqtype 被对端 harbor 按 spub.h 的 subtype_reserved 拒为 404
    t:eq(false, srey.net_call(sk, self_h, REQUEST_TYPE.REQ_DEBUG, PAYLOAD),
         "框架保留的 reqtype 被拒")

    srey.sleep(100)-- 单向 call 的投递是异步的,等它落到 on_requested
    t:check(ncall >= 2, "on_requested 被 harbor 投到至少两次 (" .. ncall .. ")")
    srey.close(sk)
end)
end)
