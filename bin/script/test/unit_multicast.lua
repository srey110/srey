-- srey.send_multi (core.send_multi) 多播绑定层测试：
-- 1) 边界: 空数组 / 长度不匹配
-- 2) 集成: server task 自起 N=4 个 client 协程,集齐后 srey.send_multi 广播 → 验证全收齐

local srey   = require("lib.srey")
local runner = require("test.runner")

local PORT = 15013
local N = 4
local MSG = "BROADCAST_LUA"

srey.startup(function()
runner.run("multicast", function(t)
    -- ── 边界: 空数组返回 false ────────────────────────────────────
    do
        local ok = srey.send_multi({}, {}, "")
        t:eq(false, ok, "空 fds/skids 返回 false")
    end

    -- ── 边界: fds 与 skids 长度不匹配应抛错 ───────────────────────
    do
        local ok = pcall(function()
            srey.send_multi({1, 2, 3}, {1, 2}, "")
        end)
        t:eq(false, ok, "长度不匹配抛 error")
    end

    -- ── 集成: 自启 server + N 个 client + 广播验证 ────────────────
    -- accept 端累积 server-side fd/skid;集齐后 send_multi;每个 client 收到 +1
    local server_fds = {}
    local server_skids = {}
    local accepted = 0
    local received = 0

    srey.on_accepted(function(pktype, fd, skid)
        accepted = accepted + 1
        server_fds[accepted] = fd
        server_skids[accepted] = skid
    end)
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        -- client 字段含 STATUS_CLIENT (0x08) 标志位,非 0 即 outgoing 连接
        if 0 ~= client and size == #MSG then
            local s = srey.ud_str(data, size)
            if s == MSG then
                received = received + 1
            end
        end
    end)

    -- listen 必须显式传 NET_EV.ACCEPT,否则 task 不订阅 ACCEPT 消息回调无法触发
    local lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "0.0.0.0", PORT, NET_EV.ACCEPT)
    t:check(lid and lid >= 0, "task_listen 成功")

    -- 起 N 个 client coro
    for _ = 1, N do
        srey.fork(function()
            local fd = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", PORT)
            t:check(fd and fd ~= INVALID_SOCK, "client coro_connect 成功")
            -- 不主动 recv,等 server 广播触发 _net_recv 回调
            srey.sleep(3000)
        end)
    end

    srey.sleep(200)
    t:eq(N, accepted, "全部 N 个 client 已 accept")

    -- 调 srey.send_multi 一次广播给所有 server-side fd
    local ok = srey.send_multi(server_fds, server_skids, MSG)
    t:eq(true, ok, "srey.send_multi 返回 true")

    -- polling 等所有 client 收到,每 50ms 检查一次最多 2s
    for _ = 1, 40 do
        srey.sleep(50)
        if received >= N then break end
    end
    t:eq(N, received, "全部 N 个 client 收到广播 (" .. received .. "/" .. N .. ")")

    -- 收尾: unlisten 释放端口给后续测试
    srey.unlisten(lid)
end)
end)
