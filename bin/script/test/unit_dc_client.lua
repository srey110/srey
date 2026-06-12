-- dc_client Lua 协程客户端单元测试:mirror C 测试 task_dc_client.c,
-- 走 srey.request → DataCenter task service(C 层 startup 已自动注册到 TASK_NAME.DATACENTER)。
-- 接口契约(与 dc_client.lua 一致):set/del 返 boolean;get/wait 返 (rdata, rsize) 裸指针,
-- 由调用方 srey.ud_str 解码;keys 返字符串数组。

local srey      = require("lib.srey")
local dc_client = require("lib.dc_client")
local runner    = require("test.runner")

local DC = TASK_NAME.DATACENTER

-- get/wait 返回 (rdata, rsize):成功且非空 → 解码字符串;空值(rsize=0)/超时(rsize=nil)→ nil
local function _val(rdata, rsize)
    if not rdata or not rsize or rsize <= 0 then
        return nil
    end
    return srey.ud_str(rdata, rsize)
end

srey.startup(function()
runner.run("dc_client", function(t)
    -- ── 子段 1:set + get 同步 round-trip ────────────────────────────
    do
        t:eq(true, dc_client.set(DC, "k1", "v1"), "set k1=v1 ok")
        t:eq("v1", _val(dc_client.get(DC, "k1")), "get k1 == 'v1'")
        t:eq(nil, _val(dc_client.get(DC, "no_such_key")), "get 不存在的 key 返回 nil")
    end

    -- ── 子段 2:wait 命中(set 在前)─────────────────────────────────
    do
        dc_client.set(DC, "k_hit", "ready")
        t:eq("ready", _val(dc_client.wait(DC, "k_hit")), "wait 命中路径返回 'ready'")
    end

    -- ── 子段 3:wait 未命中 → fork 协程稍后 set → waiter 唤醒 ────────
    do
        srey.fork(function()
            srey.sleep(100)
            dc_client.set(DC, "k_delay", "delayed")
        end)
        t:eq("delayed", _val(dc_client.wait(DC, "k_delay")), "wait miss-then-set 返回 'delayed'")
    end

    -- ── 子段 4:multi waiter — 3 个协程 wait 同 key,1 次 set 全唤醒 ──
    do
        local N = 3
        local received = 0
        for _ = 1, N do
            srey.fork(function()
                if "hello" == _val(dc_client.wait(DC, "k_multi")) then
                    received = received + 1
                end
            end)
        end
        srey.sleep(50)  -- 等所有 fork 进入 wait
        dc_client.set(DC, "k_multi", "hello")
        -- 等所有 waiter 被唤醒
        for _ = 1, 40 do
            srey.sleep(50)
            if received >= N then break end
        end
        t:eq(N, received, "multi waiter: " .. received .. "/" .. N .. " 唤醒")
    end

    -- ── 子段 5:wait 超时 — 临时降 request_timeout,等不存在的 key ────
    do
        local old_to = srey.get_request_timeout()
        srey.set_request_timeout(200)
        local d, s = dc_client.wait(DC, "k_never")
        srey.set_request_timeout(old_to)
        t:eq(nil, s, "wait 超时返回 size nil")
        t:eq(nil, _val(d, s), "wait 超时解码为 nil")
    end

    -- ── 子段 6:delete — set 后 delete,get 返回 nil ─────────────────
    do
        dc_client.set(DC, "k_del", "x")
        t:eq("x", _val(dc_client.get(DC, "k_del")), "delete pre-check")
        t:eq(true, dc_client.del(DC, "k_del"), "del k_del ok")
        t:eq(nil, _val(dc_client.get(DC, "k_del")), "del 后 get 返回 nil")
    end

    -- ── 子段 7:list_keys — set 3 key + list → 含 3 个 key ──────────
    do
        dc_client.set(DC, "lk_a", "1")
        dc_client.set(DC, "lk_b", "2")
        dc_client.set(DC, "lk_c", "3")
        local keys = dc_client.keys(DC)
        t:check(keys ~= nil, "keys() 非 nil")
        -- 验 3 个 key 都在 keys 列表内(其它子测试残留 key 也可能在,只查包含)
        local got = {}
        for _, k in ipairs(keys or {}) do
            got[k] = true
        end
        t:eq(true, got["lk_a"] == true, "keys 含 lk_a")
        t:eq(true, got["lk_b"] == true, "keys 含 lk_b")
        t:eq(true, got["lk_c"] == true, "keys 含 lk_c")
        -- 清理
        dc_client.del(DC, "lk_a")
        dc_client.del(DC, "lk_b")
        dc_client.del(DC, "lk_c")
    end

    -- ── 子段 8:set value=nil/空串 软清空 → get 返回 nil ─────────────
    do
        dc_client.set(DC, "k_clr", "data")
        t:eq("data", _val(dc_client.get(DC, "k_clr")), "set_null pre-check")
        dc_client.set(DC, "k_clr", nil)
        t:eq(nil, _val(dc_client.get(DC, "k_clr")), "set(nil) 后 get 返回 nil")
        -- 空串也是软清空
        dc_client.set(DC, "k_clr", "again")
        dc_client.set(DC, "k_clr", "")
        t:eq(nil, _val(dc_client.get(DC, "k_clr")), "set('') 后 get 返回 nil")
    end

    -- ── 子段 9(Lua 特有):非法 key(空/nil/超长)早返,不下发到 datacenter ─
    do
        t:eq(false, dc_client.set(DC, "", "v"), "set 空 key 返 false")
        t:eq(false, dc_client.set(DC, nil, "v"), "set nil key 返 false")
        t:eq(nil, _val(dc_client.get(DC, "")), "get 空 key 返 nil")
        t:eq(false, dc_client.del(DC, ""), "del 空 key 返 false")
        local long_key = string.rep("k", 512)  -- >= DC_KEY_MAX(512)
        t:eq(false, dc_client.set(DC, long_key, "v"), "set 超长 key 返 false")
        t:eq(nil, _val(dc_client.get(DC, long_key)), "get 超长 key 返 nil")
        t:eq(false, dc_client.del(DC, long_key), "del 超长 key 返 false")
    end

    -- ── 子段 10(空值语义):set(k,"") 后 wait 立即命中(size 0,区别于超时的 nil)──
    do
        local old = srey.get_request_timeout()
        srey.set_request_timeout(3000)
        dc_client.set(DC, "k_empty_b", "")   -- 空值:命中判定 if(e) 视为 present
        local t0 = srey.timer_ms()
        local d, s = dc_client.wait(DC, "k_empty_b")
        local dt = srey.timer_ms() - t0
        srey.set_request_timeout(old)
        t:eq(0, s, "空值 wait 命中返回 size 0(成功;区别于超时的 nil)")
        t:eq(nil, _val(d, s), "空值解码为 nil")
        t:check(dt < 1000, "空值 wait 立即命中(dt=" .. dt .. "ms,非挂起到 3s 超时)")
        dc_client.del(DC, "k_empty_b")
    end
end)
end)
