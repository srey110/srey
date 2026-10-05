-- srey.multi_call / srey.multi_request 绑定层测试：
-- 1) multi_call: 广播给 N 个 sub,sub ack 回 publisher,验证 ack 数 == N
-- 2) multi_call 边界: 全 NONE / 空表 / valid+NONE 混合 dsts 不崩溃
-- 3) multi_request: 广播给 N 个 sub + 共用 sess,sub task_response 回 src,
--    publisher srey.on_responsed 累计响应数 == N,验证 valid 返回值
-- 4) multi_request 边界: 空表返回 0、sess=0 抛错
-- 5) 目标数 / 连接数跨过绑定层栈数组上限(multi_* 32、send_multi 64)两侧都投齐，堆档非法元素抛错
-- 6) 名字缓存过期：同名 task 退出后重新注册，按名字投递要到新 task

local srey   = require("lib.srey")
local runner = require("test.runner")
local utils  = require("srey.utils")
local custz  = require("srey.custz")
local core   = require("srey.core")
local task   = require("srey.task")

local SUBS = {
    "multi_call_sub_a",
    "multi_call_sub_b",
    "multi_call_sub_c",
}
local N = #SUBS
local MSG = "MULTI_LUA_HELLO"
-- send_multi 用例的监听端口：全仓唯一，理由见 unit_framework.lua 的 UDP_PORT
local SEND_PORT = 15067
-- 名字缓存过期用例里反复关掉又重新注册的 task 名
local REGEN = "multi_call_sub_regen"

srey.startup(function()
runner.run(function(t)
    -- ── 集成: 广播给 N 个 sub,等他们 ack 回来 ────────────────────────
    local ack_count = 0
    local ack_seen = {}
    local ack_body = {}-- ack 载荷去掉 "<idx>:" 前缀后 → 收到次数
    srey.on_requested(function(reqtype, _, _, data, size)
        if 101 == reqtype and data and size > 0 then -- ACK_REQ
            -- sub 把自己的编号编进了载荷（multi_call_sub.lua 回的是 "<idx>:<payload>"）。
            -- 只计数的话，"3 个 sub 各 ack 一次"与"同一个 sub ack 三次"在断言上完全等价，
            -- 投递循环把同一个 dst 投三次也照样通过
            local idx, body = srey.ud_str(data, size):match("^(%d+):(.*)$")
            if idx then
                ack_seen[tonumber(idx)] = true
                ack_body[body] = (ack_body[body] or 0) + 1
            end
            ack_count = ack_count + 1
        end
    end)
    -- 广播：copy=1 默认,内部 MALLOC + memcpy 共享 pack
    srey.multi_call(SUBS, 100, MSG) -- BROADCAST_REQ
    for _ = 1, 40 do
        srey.sleep(50)
        if ack_count >= N then break end
    end
    t:eq(N, ack_count, "全部 N 个 sub ack 回来 (" .. ack_count .. "/" .. N .. ")")
    for i = 1, N do
        t:check(ack_seen[i], "sub" .. i .. " 自己的 ack 到齐（不是同一个 sub 重复 ack）")
    end

    -- ── 边界: dsts 全为 TASK_NAME.NONE 不崩溃 ────────────────────────
    do
        local ok = pcall(function()
            srey.multi_call({TASK_NAME.NONE, TASK_NAME.NONE}, 100, "noop")
        end)
        t:eq(true, ok, "全 NONE dsts 不抛错")
    end

    -- ── 边界: 空 dsts 不崩溃 ────────────────────────────────────────
    do
        local ok = pcall(function()
            srey.multi_call({}, 100, "noop")
        end)
        t:eq(true, ok, "空 dsts 不抛错")
    end

    -- ── 边界: 无处可投 + copy=0,载荷得由 C 侧释放 ───────────────────
    -- 上面两个边界用的都是字符串(copy=1,C 侧不接管),走不到这条路。copy=0 时所有权已经
    -- 转过去了,空表/全 NONE 两条早退分支各自都得释放,漏一条就每调一次泄漏一次
    do
        local ud1, usz1 = custz.pack(PACK_TYPE.CUSTZ_FIXED, "orphan_empty")
        srey.multi_call({}, 100, ud1, usz1, 0)
        local ud2, usz2 = custz.pack(PACK_TYPE.CUSTZ_FIXED, "orphan_none")
        srey.multi_call({TASK_NAME.NONE, TASK_NAME.NONE}, 100, ud2, usz2, 0)
        local ud3, usz3 = custz.pack(PACK_TYPE.CUSTZ_FIXED, "orphan_req")
        t:eq(0, srey.multi_request({}, 100, srey.id(), ud3, usz3, 0),
             "空 dsts + copy=0 返回 0")
        local ud4, usz4 = custz.pack(PACK_TYPE.CUSTZ_FIXED, "orphan_req_none")
        t:eq(0, srey.multi_request({TASK_NAME.NONE}, 100, srey.id(), ud4, usz4, 0),
             "全 NONE dsts + copy=0 返回 0")
    end

    -- ── 边界: dsts 的 __len 抛错时,copy=0 的载荷不能被吞掉 ───────────
    -- luaL_len 会走 __len 元方法、也会对非整数结果自行抛错。校验必须排在
    -- _lcore_opt_buf 之前,否则抛出时 C 侧已经接管了 copy=0 那块内存却来不及释放,
    -- 每调一次泄漏一次(泄漏本身由退出时的内存检查报出)
    do
        local bad = setmetatable({}, { __len = function() return 1.5 end })
        local ud, usize = custz.pack(PACK_TYPE.CUSTZ_FIXED, "leakcheck")
        t:check(ud ~= nil and usize > 0, "custz.pack 拿到一块 C 堆缓冲")
        t:eq(false, pcall(function() srey.multi_call(bad, 100, ud, usize, 0) end),
             "__len 返回非整数时 multi_call 抛错")
        t:eq(false, pcall(function() srey.multi_request(bad, 100, srey.id(), ud, usize, 0) end),
             "__len 返回非整数时 multi_request 抛错")
        -- 抛错发生在接管之前,所有权仍在调用方手上,由这里释放
        utils.ud_free(ud)

        local raiser = setmetatable({}, { __len = function() error("boom") end })
        t:eq(false, pcall(function() srey.multi_call(raiser, 100, "x") end),
             "__len 自身抛错时 multi_call 抛错")
    end

    -- ── 消息表的共享元表受保护 ──────────────────────────────────────
    -- 该元表全 task 共用且只挂 __gc，业务若能经 getmetatable 拿到真表并清掉 __gc，
    -- 此后每条带载荷的消息都不再释放 C 侧 payload
    do
        local sess = srey.id()
        -- 返回值必须断言：挂在裸 if 上的话，core.request 返 false 时下面 4 条一条都不跑，
        -- 模块仍报全绿
        t:check(core.request(SUBS[1], 102, sess, MSG), "core.request 投递成功")
        local msg = srey._coro_wait(sess, srey.MSG_TYPE.RESPONSE, 3000)
        t:eq(srey.MSG_TYPE.RESPONSE, msg.mtype, "拿到 RESPONSE 消息表")
        t:eq("msg", getmetatable(msg), "消息元表被 __metatable 挡住")
        t:eq(false, pcall(function() setmetatable(msg, {}) end), "消息表不可被换元表")
        t:check(msg.data ~= nil and msg.size > 0, "RESPONSE 载荷字段齐全")
        -- sub 把自己的编号编进了应答（multi_call_sub.lua 回的是 "ack"..idx），
        -- 只判"有载荷"的话，服务端把响应投到别的协程上不会被发现
        t:eq("ack1", srey.ud_str(msg.data, msg.size), "RESPONSE 来自 SUBS[1] 而非别的 sub")
    end

    -- ── 边界: dsts 混 valid + NONE 占位,仅 valid 收到 ────────────────
    do
        ack_count = 0
        srey.multi_call({SUBS[1], TASK_NAME.NONE, SUBS[2]}, 100, "mix")
        for _ = 1, 40 do
            srey.sleep(50)
            if ack_count >= 2 then break end
        end
        t:eq(2, ack_count, "valid + NONE 混合: 仅 2 个 sub 收到")
    end

    -- ── 集成: multi_request,sub 各自 task_response 回 src,触发 on_responsed ──
    local rpc_sess = srey.id()
    local resp_count = 0
    local resp_by_sess = {}
    srey.on_responsed(function(reqtype, sess, _, data, size)
        if data and size > 0 then
            resp_by_sess[sess] = (resp_by_sess[sess] or 0) + 1
            if sess == rpc_sess then
                resp_count = resp_count + 1
            end
        end
    end)
    local valid = srey.multi_request(SUBS, 102, rpc_sess, MSG) -- RPC_REQ
    t:eq(N, valid, "multi_request 返回 valid = N")
    for _ = 1, 40 do
        srey.sleep(50)
        if resp_count >= N then break end
    end
    t:eq(N, resp_count, "全部 N 个 sub response 回来 (" .. resp_count .. "/" .. N .. ")")

    -- ── 边界: multi_request 空 dsts 返回 0 ──────────────────────────
    do
        local v = srey.multi_request({}, 102, srey.id(), "noop")
        t:eq(0, v, "空 dsts multi_request 返回 0")
    end

    -- ── 边界: multi_request sess=0 抛错 ─────────────────────────────
    do
        local ok = pcall(function()
            srey.multi_request(SUBS, 102, 0, "noop")
        end)
        t:eq(false, ok, "sess=0 抛错")
    end

    -- ── 边界: sess=0 抛错时,copy=0 的载荷所有权仍在调用方 ─────────────
    -- sess 校验(lpub_check_sess)排在 _lcore_opt_buf 取载荷之前,抛出时 C 侧一次都没接管过这块内存,
    -- 与同函数其余抛出路径同口径。上面那条传的是字符串(copy 缺省 1)走不到这里;
    -- 这里若由被调方代为释放,调用方照下面这样收尾就是二次 free(退出时的内存检查报出)
    do
        local ud, usize = custz.pack(PACK_TYPE.CUSTZ_FIXED, "sess0_owner")
        t:check(ud ~= nil and usize > 0, "custz.pack 拿到一块 C 堆缓冲")
        t:eq(false, pcall(function() srey.multi_request(SUBS, 102, 0, ud, usize, 0) end),
             "sess=0 + copy=0 抛错")
        utils.ud_free(ud)
    end

    -- ── 回归: core 侧 longjmp(非法 reqtype)时错误须透传,且不泄漏 task 引用 ──
    -- 全部 grab-in-C:reqtype/data 校验在 grab 前,longjmp 时未 grab → 错误透传(下面断言)+ 无引用泄漏(退出无死锁,ASan 验证)
    do
        local ok = pcall(function()
            srey.multi_request(SUBS, {}, srey.id(), "x") -- reqtype 非整数 → C 侧 longjmp
        end)
        t:eq(false, ok, "multi_request: core longjmp 错误仍透传")

        ok = pcall(function()
            srey.multi_call(SUBS, {}, "x") -- reqtype 非整数 → C 侧 longjmp
        end)
        t:eq(false, ok, "multi_call: core longjmp 错误仍透传")

        -- 单目标 request/call/response(grab-in-C):非法 reqtype 在 task_grab 前 longjmp,错误透传且未 grab 无泄漏
        ok = pcall(function()
            srey.request(SUBS[1], {}, "x") -- 非法 reqtype → core.request longjmp
        end)
        t:eq(false, ok, "request: core longjmp 错误仍透传")
        ok = pcall(function()
            srey.call(SUBS[1], {}, "x")
        end)
        t:eq(false, ok, "call: core longjmp 错误仍透传")
        ok = pcall(function()
            srey.response(SUBS[1], {}, srey.id(), 0, "x")
        end)
        t:eq(false, ok, "response: core longjmp 错误仍透传")
    end

    -- 有界轮询：cond 成立即返回 true，最多等约 3 秒
    local function _wait(cond)
        for _ = 1, 60 do
            if cond() then
                return true
            end
            srey.sleep(50)
        end
        return cond()
    end

    -- ── multi_call / multi_request 目标数跨过栈档上限(32) ─────────────
    -- 不超过上限时 dsts 用定长数组，超过改堆上分配，两侧都要投齐；按 SUBS 轮转填满 n 格。
    -- 两种广播分开等：回包一起涌回来会顶过本 task 队列的积压告警线
    for _, n in ipairs({ 31, 32, 33, 64 }) do
        local dsts = {}
        for i = 1, n do
            dsts[i] = SUBS[(i - 1) % N + 1]
        end
        local body = "tier" .. n
        local sess = srey.id()
        srey.multi_call(dsts, 100, body)
        _wait(function() return (ack_body[body] or 0) >= n end)
        t:eq(n, ack_body[body], "multi_call n=" .. n .. " 全部 ack")
        t:eq(n, srey.multi_request(dsts, 102, sess, body), "multi_request n=" .. n .. " 投递数")
        _wait(function() return (resp_by_sess[sess] or 0) >= n end)
        t:eq(n, resp_by_sess[sess], "multi_request n=" .. n .. " 全部响应")
    end
    -- 非法元素在取载荷之前就抛，栈档与堆档同口径，copy=0 的载荷仍归调用方
    for _, n in ipairs({ 10, 40 }) do
        local bad = {}
        for i = 1, n do
            bad[i] = SUBS[1]
        end
        bad[n - 4] = true
        local ud, usz = custz.pack(PACK_TYPE.CUSTZ_FIXED, "bad_dst")
        t:eq(false, pcall(srey.multi_call, bad, 100, ud, usz, 0), "n=" .. n .. " 含非法元素 multi_call 抛错")
        t:eq(false, pcall(srey.multi_request, bad, 102, srey.id(), ud, usz, 0),
             "n=" .. n .. " 含非法元素 multi_request 抛错")
        utils.ud_free(ud)
    end

    -- ── send_multi 连接数跨过栈档上限(64) ─────────────────────────────
    -- 不超过上限时校验那趟顺手拷进栈数组，超过则校验完再拷一趟到堆上；同一连接重复 n 次，
    -- 客户端收到的字节数就是各档 n 之和
    do
        local server_sk
        local got = 0
        srey.on_accepted(function(_, sk)
            server_sk = sk
        end)
        srey.on_recved(function(_, _, client, _, _, size)
            if 0 ~= client then
                got = got + size
            end
        end)
        local lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", SEND_PORT, NET_EV.ACCEPT)
        local csk = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", SEND_PORT)
        _wait(function() return nil ~= server_sk end)
        if t:check(csk.valid and nil ~= server_sk, "send_multi 用例连接建立") then
            local expect = 0
            for _, n in ipairs({ 1, 63, 64, 65, 300 }) do
                local sks = {}
                for i = 1, n do
                    sks[i] = server_sk
                end
                t:eq(true, srey.send_multi(sks, "x"), "send_multi n=" .. n)
                expect = expect + n
            end
            _wait(function() return got >= expect end)
            t:eq(expect, got, "客户端收齐各档 send_multi 的字节")
            -- 堆档第 70 个元素非法：校验整趟走完才取载荷，抛错时 copy=0 的载荷仍归调用方
            local bad = {}
            for i = 1, 100 do
                bad[i] = server_sk
            end
            bad[70] = 1
            local ud, usz = custz.pack(PACK_TYPE.CUSTZ_FIXED, "bad_sk")
            t:eq(false, pcall(srey.send_multi, bad, ud, usz, 0), "堆档含非法元素 send_multi 抛错")
            utils.ud_free(ud)
            srey.close(server_sk)
        end
        if csk.valid then
            srey.close(csk)
        end
        srey.unlisten(lid)
    end

    -- ── 名字缓存过期：同名 task 退出后重新注册 ───────────────────────
    -- 按名字投递会把 名字→句柄 记进本 task 的缓存；旧 task 退出后缓存的句柄 grab 不到，
    -- 必须按名字重查并改写缓存，否则同名 task 重启后按名字再也投不到。
    -- 每一代 sub 的编号不同（ack 载荷是 "<idx>:..."，response 是 "ack<idx>"），据此认出投到的是哪一代
    do
        -- 关掉当前这一代并等它从 task 表消失，再注册编号为 idx 的新一代。
        -- 等消失只看 task_list：它不经过名字缓存，不会提前把过期条目修掉
        local function _regen(idx)
            local old = task.grab(REGEN)
            if old then
                task.close(old)
                task.ungrab(old)
            end
            _wait(function()
                for _, item in ipairs(core.task_list()) do
                    if REGEN == item.name then
                        return false
                    end
                end
                return true
            end)
            t:check(nil ~= task.register("test.multi_call_sub", REGEN, 0, task.name(), idx),
                    "第 " .. idx .. " 代 sub 注册成功")
            srey.sleep(100)-- 等它的 startup 挂上 on_requested
        end
        _regen(7)
        srey.call(REGEN, 100, "regen7")
        t:check(_wait(function() return 1 == ack_body["regen7"] end), "首次按名字 call 到第 7 代(写入缓存)")
        t:check(ack_seen[7], "ack 来自第 7 代")

        _regen(8)
        t:eq(true, core.call(REGEN, 100, "regen8"), "缓存过期后按名字 call 仍投递成功")
        t:check(_wait(function() return 1 == ack_body["regen8"] end) and ack_seen[8], "call 投到了第 8 代")

        _regen(9)
        local rd, rs = srey.request(REGEN, 102, "regen9")
        t:eq("ack9", rd and srey.ud_str(rd, rs), "缓存过期后按名字 request 投到第 9 代")

        _regen(10)
        srey.multi_call({ REGEN }, 100, "regen10")
        t:check(_wait(function() return 1 == ack_body["regen10"] end) and ack_seen[10],
                "缓存过期后按名字 multi_call 投到第 10 代")

        local last = task.grab(REGEN)
        if last then
            task.close(last)
            task.ungrab(last)
        end
    end
end)
end)
