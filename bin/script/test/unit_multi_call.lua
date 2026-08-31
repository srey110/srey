-- srey.multi_call / srey.multi_request 绑定层测试：
-- 1) multi_call: 广播给 N 个 sub,sub ack 回 publisher,验证 ack 数 == N
-- 2) multi_call 边界: 全 NONE / 空表 / valid+NONE 混合 dsts 不崩溃
-- 3) multi_request: 广播给 N 个 sub + 共用 sess,sub task_response 回 src,
--    publisher srey.on_responsed 累计响应数 == N,验证 valid 返回值
-- 4) multi_request 边界: 空表返回 0、sess=0 抛错

local srey   = require("lib.srey")
local runner = require("test.runner")
local utils  = require("srey.utils")
local custz  = require("srey.custz")
local core   = require("srey.core")

local SUBS = {
    "multi_call_sub_a",
    "multi_call_sub_b",
    "multi_call_sub_c",
}
local N = #SUBS
local MSG = "MULTI_LUA_HELLO"

srey.startup(function()
runner.run(function(t)
    -- ── 集成: 广播给 N 个 sub,等他们 ack 回来 ────────────────────────
    local ack_count = 0
    srey.on_requested(function(reqtype, _, _, data, size)
        if 101 == reqtype and data and size > 0 then  -- ACK_REQ
            ack_count = ack_count + 1
        end
    end)
    -- 广播：copy=1 默认,内部 MALLOC + memcpy 共享 pack
    srey.multi_call(SUBS, 100, MSG)  -- BROADCAST_REQ
    for _ = 1, 40 do
        srey.sleep(50)
        if ack_count >= N then break end
    end
    t:eq(N, ack_count, "全部 N 个 sub ack 回来 (" .. ack_count .. "/" .. N .. ")")

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
        if core.request(SUBS[1], 102, sess, MSG) then
            local msg = srey._coro_wait(sess, srey.MSG_TYPE.RESPONSE, 3000)
            t:eq(srey.MSG_TYPE.RESPONSE, msg.mtype, "拿到 RESPONSE 消息表")
            t:eq("msg", getmetatable(msg), "消息元表被 __metatable 挡住")
            t:eq(false, pcall(function() setmetatable(msg, {}) end), "消息表不可被换元表")
            t:check(msg.data ~= nil and msg.size > 0, "RESPONSE 载荷字段齐全")
        end
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
    srey.on_responsed(function(reqtype, sess, _, data, size)
        if sess == rpc_sess and data and size > 0 then
            resp_count = resp_count + 1
        end
    end)
    local valid = srey.multi_request(SUBS, 102, rpc_sess, MSG)  -- RPC_REQ
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

    -- ── 回归: core 侧 longjmp(非法 reqtype)时错误须透传,且不泄漏 task 引用 ──
    -- 全部 grab-in-C:reqtype/data 校验在 grab 前,longjmp 时未 grab → 错误透传(下面断言)+ 无引用泄漏(退出无死锁,ASan 验证)
    do
        local ok = pcall(function()
            srey.multi_request(SUBS, {}, srey.id(), "x")  -- reqtype 非整数 → C 侧 longjmp
        end)
        t:eq(false, ok, "multi_request: core longjmp 错误仍透传")

        ok = pcall(function()
            srey.multi_call(SUBS, {}, "x")  -- reqtype 非整数 → C 侧 longjmp
        end)
        t:eq(false, ok, "multi_call: core longjmp 错误仍透传")

        -- 单目标 request/call/response(grab-in-C):非法 reqtype 在 task_grab 前 longjmp,错误透传且未 grab 无泄漏
        ok = pcall(function()
            srey.request(SUBS[1], {}, "x")  -- 非法 reqtype → core.request longjmp
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
end)
end)
