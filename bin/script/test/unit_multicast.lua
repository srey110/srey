-- srey.send_multi (core.send_multi) 多播绑定层测试：
-- 1) 边界: 空数组 / 长度不匹配
-- 2) 集成: server task 自起 N=4 个 client 协程,集齐后 srey.send_multi 广播 → 验证全收齐

local srey   = require("lib.srey")
local runner = require("test.runner")
local yyjson = require("yyjson")-- yyjson.null 是 NULL lightuserdata，配 size 0 可以走到 copy 校验而不碰缓冲
local core   = require("srey.core")-- 绕开 srey.lua 的 wrapper，直打 C 层早退分支
local utils  = require("srey.utils")

-- 端口全仓唯一：15013 是 C 套件 v6only_test 的，那条用例断言"IPv4 侧无人监听"，
-- 两个二进制同跑时会被这里的 0.0.0.0 监听打成假失败
local PORT = 15018
local N = 4
local MSG = "BROADCAST_LUA"

srey.startup(function()
runner.run(function(t)
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

    -- ── 边界: copy=0 的早退分支，验所有权按 C 取载荷的位置一分为二 ──
    -- 校验全排在取载荷之前，抛出时所有权没转过去，得由这里释放；空数组返 false 那条排在
    -- 取载荷之后，由 C 释放。任一侧记错账，进程退出时 not free 就不为 0
    do
        local seri = require("srey.seri")
        local buf, sz = seri.pack("multicast-c-branch")
        t:eq(false, pcall(core.send_multi, {1, 2}, {1}, buf, sz, 0), "C 层长度不匹配抛错")
        utils.ud_free(buf)
        buf, sz = seri.pack("multicast-c-branch")
        t:eq(false, pcall(core.send_multi, {1, "x"}, {1, 2}, buf, sz, 0), "C 层元素非整数抛错")
        utils.ud_free(buf)
        buf, sz = seri.pack("multicast-c-branch")
        t:eq(false, pcall(core.send_multi, nil, {1}, buf, sz, 0), "C 层 fds 非 table 抛错")
        utils.ud_free(buf)
        buf, sz = seri.pack("multicast-c-branch")
        t:eq(false, core.send_multi({}, {}, buf, sz, 0), "C 层空数组返 false")
    end

    -- ── 边界: 含非数字元素应抛错(元素校验在填充前完成,不被 lua_tointeger 静默转 0 掩盖) ──
    do
        local ok = pcall(function()
            srey.send_multi({1, "x", 3}, {1, 2, 3}, "")
        end)
        t:eq(false, ok, "非数字元素抛 error")
    end

    -- ── 边界: 非整数浮点与数字字符串同样要抛错 ────────────────────
    -- 校验若用 lua_isnumber 会放行这两类,而取值的 lua_tointeger 对非整数浮点返回 0,
    -- 于是"校验通过"之后朝 fd 0 / skid 0 发了出去
    do
        t:eq(false, pcall(function() srey.send_multi({1.5}, {1}, "") end),  "fds 非整数浮点抛 error")
        t:eq(false, pcall(function() srey.send_multi({1}, {2.5}, "") end),  "skids 非整数浮点抛 error")
        t:eq(false, pcall(function() srey.send_multi({"1"}, {1}, "") end),  "fds 数字字符串抛 error")
    end

    -- ── 边界: copy 标志只收 0/1，且不能先收窄后判定 ────────────────
    -- 早先用 lua_isinteger 当"有没有传"的判据再裸 (int32_t) 收窄:2^32 截成 0 会让框架
    -- 接管一个借用指针,而浮点被当成"没传"强制成 1,调用方以为交了所有权于是漏释放
    do
        t:eq(false, pcall(function() srey.send(0, 0, yyjson.null, 0, 2) end), "copy=2 被拒")
        t:eq(false, pcall(function() srey.send(0, 0, yyjson.null, 0, -1) end), "copy=-1 被拒")
        t:eq(false, pcall(function() srey.send(0, 0, yyjson.null, 0, 0x100000000) end), "copy=2^32 被拒")
        t:eq(false, pcall(function() srey.send(0, 0, yyjson.null, 0, 0.5) end), "copy 非整数浮点被拒")
        -- 正向对照：合法的 0/1 必须过得去。少了这条，上面四条只要在更早的参数检查里被拒
        -- （比如 lpub_check_buf 以后收紧空 lightuserdata），就全退化成恒真断言。
        -- 返 false 是因为 fd 0 非法，ev_send 的 EMPTYPTR 早退，与 copy 校验无关
        t:eq(false, srey.send(0, 0, yyjson.null, 0, 1), "copy=1 放行，走到 ev_send")
        t:eq(false, srey.send(0, 0, yyjson.null, 0, 0), "copy=0 放行，走到 ev_send")
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

    -- 起 N 个 client coro。连接结果记进 cli_fds 由主体统一判：断言写在 fork 里的话，
    -- 连接失败要等满 5s 的 timeout_connect 才执行，那时主体早已 t:done() 上报完，
    -- 这条失败会被静默丢掉
    local cli_fds, cli_skids = {}, {}
    for _ = 1, N do
        srey.fork(function()
            local fd, skid = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", PORT)
            if fd and fd ~= INVALID_SOCK then
                cli_fds[#cli_fds + 1] = fd
                cli_skids[#cli_skids + 1] = skid
            end
            -- 不主动 recv,等 server 广播触发 _net_recv 回调。
            -- 主协程收齐就往下走了，这里睡多久都不影响结果，给个够用的兜底即可
            srey.sleep(1000)
        end)
    end

    srey.sleep(200)
    t:eq(N, #cli_fds, "全部 N 个 client coro_connect 成功")
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

    -- 库级 CLOSE 观察者：on_closed 是单槽、库抢不到，watch_closed 与业务回调并存。
    -- router 的流式上下文回收就靠它，这里借关连接验一次真的会被调到。
    -- 两条转发路径都要把 erro 交出来：watch_closed 就地同步调，on_closed 走协程
    local seen = {}
    local seen_cb = {}
    srey.watch_closed(function(_, fd, _, _, erro)
        seen[fd] = erro
    end)
    srey.on_closed(function(_, fd, _, _, erro)
        seen_cb[fd] = erro
    end)

    -- 收尾: 关掉两侧连接再 unlisten 释放端口。srey.close 要 (fd, skid) 两个值，
    -- 只接 fd 的话这 2N 条连接一直挂在事件循环里到进程退出
    for i = 1, #cli_fds do
        srey.close(cli_fds[i], cli_skids[i])
    end
    for i = 1, #server_fds do
        srey.close(server_fds[i], server_skids[i])
    end
    srey.unlisten(lid)

    srey.sleep(200)
    local nseen, nlocal, ncb = 0, 0, 0
    local fd
    for i = 1, #cli_fds do
        fd = cli_fds[i]
        if nil ~= seen[fd] then
            nseen = nseen + 1
        end
        -- client 侧是上面那轮 srey.close 主动关的(且先于 server 侧那轮)，erro 必是 LOCAL。
        -- 判 LOCAL 而不是"非 nil"：ORDERLY 恰好等于 0，只判非 nil 的话"转发点漏传 erro
        -- 拿到 nil"与"传了 ORDERLY"分不开，形参加了却传常量 0 也照样过
        if CLOSE_TYPE.LOCAL == seen[fd] then
            nlocal = nlocal + 1
        end
        if CLOSE_TYPE.LOCAL == seen_cb[fd] then
            ncb = ncb + 1
        end
    end
    t:eq(N, nseen, "watch_closed 观察到全部 client 连接的关闭 (" .. nseen .. "/" .. N .. ")")
    t:eq(N, nlocal, "watch_closed 拿到的 erro 是 LOCAL (" .. nlocal .. "/" .. N .. ")")
    t:eq(N, ncb, "on_closed 拿到的 erro 是 LOCAL (" .. ncb .. "/" .. N .. ")")
end)
end)
