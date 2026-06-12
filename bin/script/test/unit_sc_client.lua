-- sc_client Lua 协程客户端单元测试:对齐 C 测试 task_sc_client.c,
-- 走 srey.request → SubCenter task service(C 层 startup 已自动注册到 TASK_NAME.SUBCENTER)。
-- Lua 测试聚焦 wrapper 自身逻辑:wire 解码、_handlers 路由、subscribe 自动转交 retained、
-- _topic_match 通配匹配、非法参数早返。C 层协议 / 去重 / 共享组等已由 task_sc_client.c 覆盖。

local srey      = require("lib.srey")
local sc_client = require("lib.sc_client")
local runner    = require("test.runner")
local task      = require("srey.task")

local SC = TASK_NAME.SUBCENTER

-- 循环 sleep 让 task mailbox 处理 deliver 消息;返回 predicate 是否在超时前成立
---@param predicate fun():boolean
---@param max_ms? integer 默认 2000
---@return boolean
local function _wait(predicate, max_ms)
    max_ms = max_ms or 2000
    for _ = 1, max_ms // 50 do
        if predicate() then return true end
        srey.sleep(50)
    end
    return predicate()
end

srey.startup(function()
runner.run("sc_client", function(t)
    -- ── 子段 1:subscribe + publish round-trip(验 topic/payload/publisher)──
    do
        local got = {}
        sc_client.subscribe(SC, "t1/a", function(topic, payload, publisher, meta)
            got[#got + 1] = { topic = topic, payload = payload, publisher = publisher, meta = meta }
        end)
        sc_client.publish(SC, "t1/a", "hello")
        _wait(function() return #got >= 1 end)
        t:eq(1, #got, "basic: 收到 1 次")
        t:eq("t1/a", got[1] and got[1].topic, "basic: topic 't1/a'")
        t:eq("hello", got[1] and got[1].payload, "basic: payload 'hello'")
        t:eq(task.handle(), got[1] and got[1].publisher, "basic: publisher 自己")
        sc_client.unsubscribe(SC, "t1/a")
    end

    -- ── 子段 2:通配 '+' 匹配:订 "t2/+",发 "t2/a" "t2/b" 各收 1 次 ──
    do
        local got = {}
        sc_client.subscribe(SC, "t2/+", function(topic, payload)
            got[#got + 1] = topic .. "=" .. payload
        end)
        sc_client.publish(SC, "t2/a", "1")
        sc_client.publish(SC, "t2/b", "2")
        _wait(function() return #got >= 2 end)
        t:eq(2, #got, "plus: 收到 2 次")
        local set = {}
        for _, v in ipairs(got) do set[v] = true end
        t:eq(true, set["t2/a=1"] == true, "plus: t2/a=1 命中")
        t:eq(true, set["t2/b=2"] == true, "plus: t2/b=2 命中")
        sc_client.unsubscribe(SC, "t2/+")
    end

    -- ── 子段 3:通配 '#' 匹配:订 "t3/#",发 "t3/a" "t3/a/b" 各收 1 次 ──
    do
        local got = {}
        sc_client.subscribe(SC, "t3/#", function(topic)
            got[#got + 1] = topic
        end)
        sc_client.publish(SC, "t3/a", "x")
        sc_client.publish(SC, "t3/a/b", "y")
        _wait(function() return #got >= 2 end)
        t:eq(2, #got, "hash: 收到 2 次")
        sc_client.unsubscribe(SC, "t3/#")
    end

    -- ── 子段 4:subscribe 自动转交首批 retained 给 handler(Lua wrapper 特有)──
    do
        sc_client.publish_retained(SC, "t4/r", "saved")
        srey.sleep(50)   -- 等 C 层 retained 落盘
        local got = {}
        sc_client.subscribe(SC, "t4/r", function(topic, payload)
            got[#got + 1] = topic .. "=" .. payload
        end)
        _wait(function() return #got >= 1 end)
        t:eq(1, #got, "auto-retained: subscribe 立即收到 1 次")
        t:eq("t4/r=saved", got[1], "auto-retained: payload 与 retained 一致")
        sc_client.unsubscribe(SC, "t4/r")
        sc_client.publish_retained(SC, "t4/r", nil)   -- 清理
    end

    -- ── 子段 5:query_retained 返回数组,wire 解码正确 ──
    do
        sc_client.publish_retained(SC, "t5/x", "v1")
        sc_client.publish_retained(SC, "t5/y", "v2")
        srey.sleep(50)
        local list = sc_client.query_retained(SC, "t5/+")
        t:check(list ~= nil, "query_retained: 非 nil")
        t:eq(2, list and #list or 0, "query_retained: 2 条")
        if list and #list == 2 then
            local map = {}
            for _, r in ipairs(list) do map[r.topic] = r.payload end
            t:eq("v1", map["t5/x"], "query_retained: t5/x=v1")
            t:eq("v2", map["t5/y"], "query_retained: t5/y=v2")
        end
        sc_client.publish_retained(SC, "t5/x", nil)
        sc_client.publish_retained(SC, "t5/y", nil)
    end

    -- ── 子段 6:publish_retained nil/'' 清空槽位,query_retained 拿不到 ──
    do
        sc_client.publish_retained(SC, "t6/r", "data")
        srey.sleep(50)
        sc_client.publish_retained(SC, "t6/r", nil)
        srey.sleep(50)
        local list = sc_client.query_retained(SC, "t6/r")
        t:eq(0, list and #list or -1, "retained_clear: 清空后 query 返空表")
    end

    -- ── 子段 7:set_meta + publish,handler 收到 meta ──────────────────
    do
        sc_client.set_meta(SC, "MM")
        local got_meta
        sc_client.subscribe(SC, "t7/m", function(_, _, _, meta)
            got_meta = meta
        end)
        sc_client.publish(SC, "t7/m", "p")
        _wait(function() return got_meta ~= nil end)
        t:eq("MM", got_meta, "set_meta: handler 收到 meta 'MM'")
        sc_client.unsubscribe(SC, "t7/m")
        sc_client.set_meta(SC, nil)
    end

    -- ── 子段 8:unsubscribe 后不再 deliver ────────────────────────────
    do
        local got = 0
        sc_client.subscribe(SC, "t8/u", function() got = got + 1 end)
        sc_client.unsubscribe(SC, "t8/u")
        sc_client.publish(SC, "t8/u", "x")
        srey.sleep(200)
        t:eq(0, got, "unsub: 取消订阅后不再收")
    end

    -- ── 子段 9:共享订阅:不收 retained,但收 publish ───────────────────
    do
        sc_client.publish_retained(SC, "t9/r", "before_sub")
        srey.sleep(50)
        local got = 0
        sc_client.subscribe_shared(SC, "t9/r", "g1", function() got = got + 1 end)
        srey.sleep(100)
        t:eq(0, got, "shared: 不自动收 retained")
        sc_client.publish(SC, "t9/r", "live")
        _wait(function() return got >= 1 end)
        t:eq(1, got, "shared: publish 收到 1 次")
        sc_client.unsubscribe_shared(SC, "t9/r", "g1")
        sc_client.publish_retained(SC, "t9/r", nil)
    end

    -- ── 子段 10:topics() / retained_topics() 调试接口非空 ────────────
    do
        sc_client.subscribe(SC, "t10/a", function() end)
        sc_client.subscribe(SC, "t10/b", function() end)
        sc_client.publish_retained(SC, "t10/r", "z")
        srey.sleep(50)
        local list = sc_client.topics(SC)
        t:check(list ~= nil and #list >= 2, "topics(): 非空且至少含 2 个 topic")
        local rlist = sc_client.retained_topics(SC)
        t:check(rlist ~= nil and #rlist >= 1, "retained_topics(): 非空且至少含 1 个")
        if rlist then
            local found
            for _, r in ipairs(rlist) do
                if r.topic == "t10/r" then
                    found = r
                    break
                end
            end
            t:check(found ~= nil, "retained_topics(): 含 t10/r")
            t:eq(1, found and found.size, "retained_topics(): t10/r size=1")
        end
        sc_client.unsubscribe(SC, "t10/a")
        sc_client.unsubscribe(SC, "t10/b")
        sc_client.publish_retained(SC, "t10/r", nil)
    end

    -- ── 子段 11(Lua 特有):非法参数早返,不下发到 subcenter ────────────
    do
        t:eq(ERR_FAILED, sc_client.subscribe(SC, "", function() end),
            "subscribe: empty topic 返 ERR_FAILED")
        t:eq(ERR_FAILED, sc_client.subscribe(SC, "t/x", nil),
            "subscribe: nil handler 返 ERR_FAILED")
        t:eq(ERR_FAILED, sc_client.subscribe_shared(SC, "t/x", "", function() end),
            "subscribe_shared: empty group 返 ERR_FAILED")
        t:eq(ERR_FAILED, sc_client.publish(SC, ""),
            "publish: empty topic 返 ERR_FAILED")
        t:eq(ERR_FAILED, sc_client.publish_retained(SC, ""),
            "publish_retained: empty topic 返 ERR_FAILED")
        t:eq(ERR_FAILED, sc_client.unsubscribe(SC, ""),
            "unsubscribe: empty topic 返 ERR_FAILED")
        t:eq(nil, sc_client.query_retained(SC, ""),
            "query_retained: empty pattern 返 nil")
    end

    -- ── 子段 12(Lua 特有):多 pattern 命中同 publish 各调一次 handler ──
    -- C 层 publish_dedup hashset 按 task 去重,同一订阅者只收一次 deliver。
    -- Lua wrapper _handlers 表按 pattern 索引,_on_deliver 遍历所有 pattern 对匹配的逐个调 handler。
    -- 这里测:同 task 订两个不同 pattern("t12/a" 与 "t12/+"),各注册各自 handler,
    -- 一次 publish "t12/a" 触发两个 handler 各被调一次(由 pairs(_handlers) 循环触发)。
    do
        local got = 0
        sc_client.subscribe(SC, "t12/a", function() got = got + 1 end)
        sc_client.subscribe(SC, "t12/+", function() got = got + 1 end)
        sc_client.publish(SC, "t12/a", "x")
        srey.sleep(200)
        t:eq(2, got, "multi-pattern: 同 task 订两个匹配 pattern,handler 被调 2 次")
        sc_client.unsubscribe(SC, "t12/a")
        sc_client.unsubscribe(SC, "t12/+")
    end

    -- ── 子段 13(本次修复):同 task 同 topic 普通+共享订阅,各收各的 ──────
    -- 修复前 _handlers[topic] 单槽,subscribe_shared 覆盖 subscribe 的 handler:
    -- 普通 handler 丢失,两条 deliver 都喂给幸存 handler。
    -- 修复后 deliver 带 kind:普通(0)走 _handlers、共享(1)走 _shared_handlers,各归其位。
    do
        local n_normal = 0
        local n_shared = 0
        sc_client.subscribe(SC, "t13/x", function() n_normal = n_normal + 1 end)
        sc_client.subscribe_shared(SC, "t13/x", "g13", function() n_shared = n_shared + 1 end)
        sc_client.publish(SC, "t13/x", "p")
        _wait(function() return n_normal >= 1 and n_shared >= 1 end)
        t:eq(1, n_normal, "normal+shared: 普通 handler 收到 1 次")
        t:eq(1, n_shared, "normal+shared: 共享 handler 收到 1 次")
        sc_client.unsubscribe(SC, "t13/x")
        sc_client.unsubscribe_shared(SC, "t13/x", "g13")
    end

    -- ── 子段 14(本次修复):同 task 同 topic 多 group 各自精确收 ──────────
    -- 修复前 _shared_handlers[topic] 单槽:g2 覆盖 g1、unsub 任一抹全部。
    -- 修复后 deliver wire 带 group,_shared_handlers[topic][group] 二级索引精确路由。
    do
        local n1 = 0
        local n2 = 0
        sc_client.subscribe_shared(SC, "t14/x", "g1", function() n1 = n1 + 1 end)
        sc_client.subscribe_shared(SC, "t14/x", "g2", function() n2 = n2 + 1 end)
        sc_client.publish(SC, "t14/x", "p")
        _wait(function() return n1 >= 1 and n2 >= 1 end)
        t:eq(1, n1, "multi-group: g1 handler 精确收 1 次(不被 g2 覆盖)")
        t:eq(1, n2, "multi-group: g2 handler 精确收 1 次")
        -- group 级退订:退 g1 后只剩 g2 收
        sc_client.unsubscribe_shared(SC, "t14/x", "g1")
        sc_client.publish(SC, "t14/x", "p2")
        _wait(function() return n2 >= 2 end)
        t:eq(1, n1, "multi-group: 退 g1 后 g1 handler 不再收")
        t:eq(2, n2, "multi-group: 退 g1 不影响 g2,g2 仍收")
        sc_client.unsubscribe_shared(SC, "t14/x", "g2")
    end
end)
end)
