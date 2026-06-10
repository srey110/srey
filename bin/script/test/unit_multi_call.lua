-- srey.multi_call / srey.multi_request 绑定层测试：
-- 1) multi_call: 广播给 N 个 sub,sub ack 回 publisher,验证 ack 数 == N
-- 2) multi_call 边界: 全 NONE / 空表 / valid+NONE 混合 dsts 不崩溃
-- 3) multi_request: 广播给 N 个 sub + 共用 sess,sub task_response 回 src,
--    publisher srey.on_responsed 累计响应数 == N,验证 valid 返回值
-- 4) multi_request 边界: 空表返回 0、sess=0 抛错

local srey   = require("lib.srey")
local runner = require("test.runner")

local SUBS = {
    "multi_call_sub_a",
    "multi_call_sub_b",
    "multi_call_sub_c",
}
local N = #SUBS
local MSG = "MULTI_LUA_HELLO"

srey.startup(function()
runner.run("multi_call", function(t)
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
    srey.on_responsed(function(sess, _, data, size)
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
end)
end)
