-- db_pool 单元测试：懒建连、并发上界、FIFO 交接、断连重连、冷却（排队 / 翻倍）、等连接超时、
-- 关闭唤醒等待者、空闲探活、寿命抖动、reset、维护协程、with 的自动归还。全部用假连接，不碰网络。

local srey      = require("lib.srey")
local runner    = require("test.runner")
local db_pool = require("lib.db_pool")

-- 假连接：只记计数与状态，不碰网络
local function new_factory()
    local made = {}
    local function factory()
        local c = {
            id = #made + 1,
            established = false,
            connects = 0,
            pings = 0,
            quits = 0,
            fail_connect = 0,
            fail_ping = 0,
            connect_ms = 0,
        }
        function c:connect()
            self.connects = self.connects + 1
            if self.connect_ms > 0 then
                srey.sleep(self.connect_ms)
            end
            if self.fail_connect > 0 then
                self.fail_connect = self.fail_connect - 1
                return false
            end
            self.established = true
            return true
        end
        function c:ping()
            self.pings = self.pings + 1
            if self.fail_ping > 0 then
                self.fail_ping = self.fail_ping - 1
                self.established = false
                return false
            end
            return true
        end
        function c:quit()
            self.quits = self.quits + 1
            self.established = false
        end
        c.dirty = false
        c.resets = 0
        c.fail_reset = 0
        -- 带着没结束的会话状态时清掉；干净时不算一次
        function c:reset_session()
            if not self.dirty then
                return true
            end
            self.resets = self.resets + 1
            if self.fail_reset > 0 then
                self.fail_reset = self.fail_reset - 1
                return false
            end
            self.dirty = false
            return true
        end
        made[#made + 1] = c
        return c
    end
    return factory, made
end

srey.startup(function()
runner.run(function(t)
    -- ── 懒建连：首次借出才 connect，归还后再借不重连 ──────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local c = pool:get()
        t:check(c ~= nil, "首次 get 成功")
        t:eq(1, made[1].connects, "首次借出才建连")
        pool:put(c)
        local c2 = pool:get()
        t:check(c2 ~= nil, "归还后再借成功")
        t:eq(1, made[1].connects, "空闲连接复用，不重连")
        pool:put(c2)
        t:eq(1, pool:stat_get().idle, "归还后空闲 1 条")
        pool:free()
        t:eq(1, made[1].quits, "关闭断开空闲连接")
    end

    -- ── 并发上界：max=2、4 协程同时借，峰值只能是 2 ──────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 2, backoff = 20, factory = factory }
        local in_use, peak = 0, 0
        for i = 1, 4 do
            srey.fork(function()
                local c = pool:get(1000)
                if not c then
                    return
                end
                in_use = in_use + 1
                if in_use > peak then
                    peak = in_use
                end
                srey.sleep(20)
                in_use = in_use - 1
                pool:put(c)
            end)
        end
        srey.sleep(100)
        t:eq(2, peak, "并发峰值等于 max")
        local st = pool:stat_get()
        t:eq(0, st.busy, "全部归还后 busy=0")
        t:eq(2, st.idle, "全部归还后 idle=2")
        t:eq(2, st.connects, "只建两条连接")
        pool:free()
        t:eq(2, made[1].quits + made[2].quits, "关闭断开两条")
    end

    -- ── FIFO：max=1，先借到的持有 30ms，后两个按入队顺序拿 ──────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local order = {}
        for i = 1, 3 do
            local id = i
            srey.fork(function()
                local c = pool:get(1000)
                if not c then
                    return
                end
                order[#order + 1] = id
                if 1 == id then
                    srey.sleep(30)
                end
                pool:put(c)
            end)
        end
        srey.sleep(120)
        t:eq(3, #order, "三个协程都拿到连接")
        t:eq(1, order[1], "FIFO 第一")
        t:eq(2, order[2], "FIFO 第二")
        t:eq(3, order[3], "FIFO 第三")
        pool:free()
        t:eq(1, made[1].quits, "单连接关闭断开一次")
    end

    -- ── 借出期间断连：归还后下一次借出自动重连 ──────────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local c = pool:get()
        c.established = false
        pool:put(c)
        local c2 = pool:get()
        t:check(c2 ~= nil, "断连后重新借到")
        t:eq(2, made[1].connects, "归还断连连接后重连一次")
        pool:put(c2)
        pool:free()
    end

    -- ── 建连失败冷却：冷却期内不再重试，过了再重连 ──────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 30, factory = factory }
        local c = pool:get()
        pool:put(c)
        made[1].established = false
        made[1].fail_connect = 1
        local c1 = pool:get()
        t:eq(nil, c1, "建连失败时 get 返回 nil")
        t:eq(2, made[1].connects, "失败那次确实调了 connect")
        local c2 = pool:get()
        t:eq(nil, c2, "冷却期内不再尝试")
        t:eq(2, made[1].connects, "冷却期内 connects 不增")
        srey.sleep(40)
        local c3 = pool:get()
        t:check(c3 ~= nil, "冷却结束后可重连")
        t:eq(3, made[1].connects, "冷却后重连一次")
        pool:put(c3)
        pool:free()
    end

    -- ── 等连接超时：max=1 被占住，get(100) 返回 timeout ──────────
    do
        local factory = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local holder = pool:get()
        local c, err = pool:get(100)
        t:eq(nil, c, "等超时拿不到连接")
        t:eq("get timeout", err, "超时原因")
        t:eq(1, pool:stat_get().timeouts, "超时计数")
        pool:put(holder)
        local c2 = pool:get(1000)
        t:check(c2 ~= nil, "归还后可继续借")
        pool:put(c2)
        pool:free()
    end

    -- ── 关闭：唤醒等待者，借出连接在 put 时断开 ─────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local holder = pool:get()
        local got
        srey.fork(function()
            local c, err = pool:get()
            got = c and "conn" or err
        end)
        srey.sleep(10)
        pool:free()
        srey.sleep(10)
        t:eq("pool closed", got, "等待者被关闭唤醒")
        pool:put(holder)
        t:eq(1, made[1].quits, "借出连接在 put 时断开")
    end

    -- ── 空闲探活：超过 ping_idle 先 ping ─────────────────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, ping_idle = 10, backoff = 20, factory = factory }
        local c = pool:get()
        pool:put(c)
        srey.sleep(25)
        local c2 = pool:get()
        t:check(c2 ~= nil, "空闲后再借成功")
        t:eq(1, made[1].pings, "空闲超时先 ping")
        pool:put(c2)
        pool:free()
    end

    -- ── warm：直接建 n 条不同连接，不受空闲栈复用影响 ───────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 2, backoff = 20, factory = factory }
        t:eq(true, pool:warm(2), "warm(2) 成功")
        t:eq(2, #made, "warm 创建两条不同连接")
        t:eq(2, pool:stat_get().idle, "warm 后空闲 2")
        pool:free()
        t:eq(2, made[1].quits + made[2].quits, "关闭断开两条")
    end

    -- ── 冷却中的空闲连接不挡新连接：max=2 时仍能新建 ─────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 2, backoff = 30, factory = factory }
        local c = pool:get()
        pool:put(c)
        made[1].established = false
        made[1].fail_connect = 1
        local c2 = pool:get()
        t:check(c2 ~= nil, "冷却期间仍能新建连接")
        t:eq(2, #made, "冷却的空闲连接不占满 max")
        t:eq(2, pool:stat_get().connects, "新连接建连成功")
        if c2 then
            pool:put(c2)
        end
        pool:free()
    end

    -- ── max_lifetime 到期归还：丢弃并唤醒等待者新建 ─────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, max_lifetime = 10, backoff = 20, factory = factory }
        local holder = pool:get()
        local got
        srey.fork(function()
            local c = pool:get(1000)
            got = c
            if c then
                pool:put(c)
            end
        end)
        srey.sleep(20)
        pool:put(holder)
        srey.sleep(30)
        t:check(got ~= nil, "max_lifetime 丢弃后等待者拿到新连接")
        t:eq(2, #made, "丢弃后新建了一条")
        pool:free()
    end

    -- ── 还有别的借出时，断连归还 / 寿命丢弃空出的位置也要叫醒等待者 ──
    do
        local factory = new_factory()
        local pool = db_pool.new{ max = 2, backoff = 20, factory = factory }
        local a = pool:get()
        local b = pool:get()
        local got
        srey.fork(function()
            got = pool:get(2000)
            if got then
                pool:put(got)
            end
        end)
        srey.sleep(10)
        b.established = false
        pool:put(b)
        srey.sleep(30)
        t:check(got ~= nil, "断连归还后等待者不必等另一条归还")
        pool:put(a)
        pool:free()
    end
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 2, max_lifetime = 10, backoff = 20, factory = factory }
        local a = pool:get()
        local b = pool:get()
        local got
        srey.fork(function()
            got = pool:get(2000)
            if got then
                pool:put(got)
            end
        end)
        srey.sleep(20)
        pool:put(b)
        srey.sleep(30)
        t:check(got ~= nil, "寿命丢弃后等待者不必等另一条归还")
        t:eq(3, #made, "丢弃后新建了第三条")
        pool:put(a)
        pool:free()
    end

    -- 以下几段与 C 侧 test/task_pool.c 的同名用例逐条对应：冷却排队、冷却翻倍、寿命抖动、reset、维护协程
    -- ── 冷却排队：有连接借出、空闲的在冷却时不快速失败，等归还 ─────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 2, backoff = 5000, factory = factory }
        local a = pool:get()
        local b = pool:get()
        pool:put(b)
        made[2].established = false
        made[2].fail_connect = 1
        local got, gerr
        srey.fork(function()
            got, gerr = pool:get(2000)
            if got then
                pool:put(got)
            end
        end)
        srey.sleep(50)
        pool:put(a)
        srey.sleep(20)
        t:check(got ~= nil, "冷却排队: 借到归还的那条 " .. tostring(gerr))
        t:eq(2, #made, "冷却排队: 不新建")
        pool:free()
    end

    -- ── 冷却翻倍：连续失败按 backoff 翻倍 ────────────────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 100, factory = factory }
        local c = pool:get()
        pool:put(c)
        made[1].established = false
        made[1].fail_connect = 3
        pool:get()-- t0 第 1 次失败：冷却 100ms
        srey.sleep(150)
        pool:get()-- t150 第 2 次失败：冷却 200ms，到 t350
        srey.sleep(100)
        t:eq(nil, pool:get(), "冷却翻倍: t250 仍在冷却")
        t:eq(3, made[1].connects, "冷却翻倍: 第二次冷却翻倍，没再试")
        srey.sleep(150)
        pool:get()-- t400 第 3 次失败：冷却 400ms，到 t800
        srey.sleep(450)
        local c2 = pool:get()
        t:check(c2 ~= nil, "冷却翻倍: 冷却过后连上")
        t:eq(5, made[1].connects, "冷却翻倍: 首次建连 + 3 次失败 + 这次")
        if c2 then
            pool:put(c2)
        end
        pool:free()
    end

    -- ── 寿命抖动：到 7/8 之前一定没到期，过了整个寿命一定到期 ─────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, max_lifetime = 160, backoff = 20, factory = factory }
        for i = 1, 3 do
            if i > 1 then
                srey.sleep(40)
            end
            local c = pool:get()
            pool:put(c)
        end
        t:eq(1, #made, "寿命抖动: 7/8 寿命之前没到期")
        srey.sleep(100)
        local c = pool:get()
        t:eq(2, #made, "寿命抖动: 过了寿命丢弃新建")
        pool:put(c)
        pool:free()
    end

    -- ── reset：归还时清掉会话状态再给下一个人；清不掉就断开 ─────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local c = pool:get()
        pool:put(c)
        t:eq(0, made[1].resets, "reset: 干净的不算清理")
        c = pool:get()
        c.dirty = true
        pool:put(c)
        t:eq(1, made[1].resets, "reset: 脏的清掉")
        t:eq(0, made[1].quits, "reset: 清掉后不断开")
        c = pool:get()
        c.dirty = true
        c.fail_reset = 1
        pool:put(c)
        t:eq(1, made[1].quits, "reset: 清不掉就断开")
        t:eq(0, pool:stat_get().total, "reset: 清不掉的整条丢掉")
        c = pool:get()
        t:eq(made[2], c, "reset: 下次借出新建一条")
        t:eq(1, made[2].connects, "reset: 新建的只连一次")
        pool:put(c)
        pool:free()
    end

    -- ── 维护协程：没人借还也收缩；借出过久告警；补足 min。协程超时是秒级粒度，等 2.3 秒 ──
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 3, min = 1, idle_timeout = 10, leak_warn = 500, backoff = 20, factory = factory }
        local held = pool:get()
        local a = pool:get()
        local b = pool:get()
        pool:put(a)
        pool:put(b)
        srey.sleep(2300)
        t:eq(1, a.quits + b.quits, "维护协程: 没人借还也收缩，留 min 条")
        t:eq(1, pool:stat_get().leaks, "维护协程: 借出过久告警一次")
        pool:put(held)
        pool:free()
        local factory2, made2 = new_factory()
        local pool2 = db_pool.new{ max = 2, min = 2, backoff = 20, factory = factory2 }
        local c = pool2:get()
        pool2:put(c)
        srey.sleep(2300)
        t:eq(2, #made2, "维护协程: 补足 min 条")
        t:eq(2, pool2:stat_get().idle, "维护协程: 补上的在空闲栈")
        pool2:free()
    end

    -- ── idle_timeout：维护协程收缩空闲超时的连接；协程超时是秒级粒度，等 2.3 秒 ──
    do
        local factory = new_factory()
        local pool = db_pool.new{ max = 2, min = 0, idle_timeout = 10, factory = factory }
        local a = pool:get()
        local b = pool:get()
        pool:put(a)
        srey.sleep(2300)-- 维护协程丢掉空闲的 a；b 还借着，不受影响
        pool:put(b)
        t:eq(1, pool:stat_get().idle, "收缩后只留刚归还的空闲")
        t:eq(1, a.quits, "超时空闲连接被断开")
        pool:free()
    end

    -- ── 连接实现抛错：计数回滚，异常不漏给调用方 ─────────────────────
    do
        local bad = {
            established = false,
            connect = function()
                error("connect boom")
            end,
            quit = function()
            end,
        }
        local pool = db_pool.new{ max = 1, factory = function()
            return bad
        end }
        local c = pool:get()
        t:eq(nil, c, "connect 抛错时 get 返回 nil")
        t:eq(1, pool:stat_get().total, "connect 抛错的连接留在空闲栈冷却(同 C 侧的槽)")
        t:eq(0, pool:stat_get().busy, "connect 抛错后 busy 回滚")
        t:eq(1, pool:stat_get().connfails, "connect 抛错计入 connfails")
        pool:free()
    end

    -- ── 参数校验：负数 timeout 直接抛错，不当作"一直等" ─────────────
    do
        local factory = new_factory()
        local pool = db_pool.new{ max = 1, factory = factory }
        local ok = pcall(pool.get, pool, -1)
        t:eq(false, ok, "负数 timeout 抛错")
        t:eq(false, pool:warm(-1), "负数 warm 返回 false")
        pool:free()
    end

    -- ── with：自动归还，并原样带回全部返回值 ─────────────────────────
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local a, b, c = pool:with(function()
            return 1, nil, 3
        end)
        t:eq(1, a, "with 返回值 1")
        t:eq(nil, b, "with 返回值 2")
        t:eq(3, c, "with 返回值 3")
        t:eq(0, pool:stat_get().busy, "with 结束自动归还")
        pool:free()
        t:eq(1, made[1].quits, "with 的连接随关闭断开")
    end

    -- ── with：fn 抛错也归还，错误原样带回 ───────────────────────────
    do
        local factory = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local r, err = pool:with(function()
            error("boom")
        end)
        t:eq(nil, r, "with 抛错返回 nil")
        t:check("string" == type(err) and nil ~= err:find("boom", 1, true), "with 抛错带回错误")
        t:eq(0, pool:stat_get().busy, "with 抛错也归还")
        pool:free()
    end

    -- ── 没有借出、空闲的全在冷却：第二次 get 不等，报 all cooling ──────
    do
        local factory = new_factory()
        local function failing()
            local c = factory()
            c.fail_connect = 1
            return c
        end
        local pool = db_pool.new{ max = 1, backoff = 1000, factory = failing }
        local c, err = pool:get()
        t:check(nil == c and "connect failed" == err, "建连失败直接返回")
        c, err = pool:get()
        t:check(nil == c and "all cooling" == err, "全在冷却快速失败")
        pool:free()
    end

    -- ── 新建失败、已无借出时叫醒排队者，别让它一直等 ─────────────────
    do
        local factory = new_factory()
        local function slow_failing()
            local c = factory()
            c.connect_ms = 30
            c.fail_connect = 1
            return c
        end
        local pool = db_pool.new{ max = 1, backoff = 1000, factory = slow_failing }
        local first, second
        srey.fork(function()
            local _, err = pool:get()
            first = err
        end)
        srey.sleep(5)
        srey.fork(function()
            local _, err = pool:get()-- 一直等：前一个建连失败后要被叫醒
            second = err
        end)
        srey.sleep(100)
        t:eq("connect failed", first, "建连者拿到真实原因")
        t:eq("all cooling", second, "排队者被叫醒后快速失败")
        pool:free()
    end

    -- ── 已交接给等待者、它还没被 resume 时池被 free：等待者丢掉连接、报池关闭 ──
    do
        local factory, made = new_factory()
        local pool = db_pool.new{ max = 1, backoff = 20, factory = factory }
        local c = pool:get()
        local werr
        srey.fork(function()
            local got, err = pool:get()
            werr = got and "got" or err
        end)
        srey.sleep(10)
        pool:put(c)-- 交接给等待者，它要等本次分发末尾才跑
        pool:free()
        srey.sleep(10)
        t:eq("pool closed", werr, "交接途中关闭：等待者报池关闭")
        t:eq(0, pool:stat_get().total, "交接的连接被丢掉")
        t:eq(1, made[1].quits, "交接的连接被断开")
    end

    -- ── 超时醒来后同一 sess 上又挂起的等待，不能被那次超时扫描顺手删掉 ──
    -- 协程超时是秒级粒度，时序见注释；修复前 W1 第二次 get 永远等不到超时
    do
        local factory = new_factory()
        local pool = db_pool.new{ max = 2, backoff = 20, factory = factory }
        local h = pool:get()
        local res
        srey.fork(function()-- W1：借着 a，再等第二条
            local a = pool:get()
            srey.sleep(20)-- 让 W0 先排上队头
            if pool:get(500) then
                res = "unexpected"
                return
            end
            pool:put(a)-- 超时扫描 resume 期间归还：交给 W0，本 sess 的旧等待表被摘空删掉
            local got, err = pool:get(500)-- 新建等待表，不能被那次扫描末尾删掉
            res = got and "got" or err
        end)
        srey.sleep(5)
        srey.fork(function()-- W0：一直等，拿到 a 后多拿一会，确保 W1 第二次只能超时
            local a = pool:get()
            if a then
                srey.sleep(2900)
                pool:put(a)
            end
        end)
        srey.sleep(3400)
        t:eq("get timeout", res, "第二次等待照常超时")
        pool:put(h)
        srey.sleep(1300)
        pool:free()
    end

    -- ── srey._coro_wake / _coro_wake_all：只摘队头同标记的，延迟到本次分发末尾才 resume ──
    do
        local sess = srey.id()
        local TAG = srey.TAG_DB_POOL
        local got = {}
        for i = 1, 3 do
            srey.fork(function()
                -- 先挂起再算下标：写成一行的话 #got + 1 在挂起前就算好了，三个协程都写到同一格
                local msg = srey._coro_wait(sess, TAG, 0)
                got[#got + 1] = { i, msg }
            end)
        end
        srey.sleep(10)
        local payload = { x = 1 }
        t:eq(false, srey._coro_wake(sess, srey.TAG_DB_POOL_TICK, payload), "队头标记不符不摘")
        t:eq(true, srey._coro_wake(sess, TAG, payload), "摘到队头")
        t:eq(0, #got, "唤醒延迟到本次分发末尾")
        srey.sleep(10)
        t:eq(1, #got, "只醒了一个")
        t:eq(1, got[1] and got[1][1], "先等的先醒")
        t:eq(payload, got[1] and got[1][2], "载荷原样带回")
        t:eq(true, srey._coro_wake_all(sess, TAG, payload), "wake_all 摘到其余")
        srey.sleep(10)
        t:eq(3, #got, "其余都醒了")
        t:eq(false, srey._coro_wake(sess, TAG, payload), "没人等返回 false")
    end
end)
end)
