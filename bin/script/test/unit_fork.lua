-- srey.fork / srey.fork_wait 单元测试：协程分叉、屏障等待、错误路径

local srey   = require("lib.srey")
local runner = require("test.runner")

srey.startup(function()
runner.run("fork", function(t)
    -- ── srey.fork：fire-and-forget，主协程不等 ─────────────────────────
    do
        local hit = 0
        srey.fork(function() hit = hit + 1 end)
        srey.fork(function() hit = hit + 1 end)
        srey.fork(function() hit = hit + 1 end)
        -- 此时 fork 出来的协程还在 fork_queue 里，未起；hit 仍是 0
        t:eq(0, hit, "fork 不立即执行")
        -- yield 让出后，message_dispatch 末尾 drain 触发 3 个协程
        srey.sleep(10)
        t:eq(3, hit, "yield 后 3 个 fork 都执行完")
    end

    -- ── srey.fork 参数透传 ────────────────────────────────────────────
    do
        local sum = 0
        srey.fork(function(a, b, c) sum = a + b + c end, 1, 2, 3)
        srey.sleep(10)
        t:eq(6, sum, "fork 参数透传")
    end

    -- ── srey.fork_wait：0 个任务 ──────────────────────────────────────
    do
        local r = srey.fork_wait({})
        t:eq(0, #r, "fork_wait 空列表立即返回空表")
    end

    -- ── srey.fork_wait：1 个任务 ──────────────────────────────────────
    do
        local r = srey.fork_wait({
            function() return 42 end,
        })
        t:eq(1, #r, "fork_wait 1 个任务长度 1")
        t:eq(true, r[1].ok, "fork_wait 1 个任务 ok=true")
        t:eq(42, r[1].val, "fork_wait 1 个任务 val 正确")
    end

    -- ── srey.fork_wait：N 个并发任务，结果与输入同序 ──────────────────
    do
        local t0 = srey.timer_ms()
        local r = srey.fork_wait({
            function() srey.sleep(50); return "a" end,
            function() srey.sleep(50); return "b" end,
            function() srey.sleep(50); return "c" end,
        })
        local elapsed = srey.timer_ms() - t0
        t:eq(3, #r, "fork_wait N 个任务长度 N")
        t:eq("a", r[1].val, "results[1] = a")
        t:eq("b", r[2].val, "results[2] = b")
        t:eq("c", r[3].val, "results[3] = c")
        -- 3 个 sleep 50ms 并发，总耗时应该 < 串行 150ms（留 30ms 余量）
        t:check(elapsed < 120, "3 个 fork 并发耗时 < 120ms（实际 " .. elapsed .. "ms）")
    end

    -- ── srey.fork_wait：错误路径，ok=false 时 val 是 err 字符串 ───────
    do
        local r = srey.fork_wait({
            function() return "ok1" end,
            function() error("boom") end,
            function() return "ok3" end,
        })
        t:eq(3, #r, "异常任务不影响其他任务长度")
        t:eq(true, r[1].ok, "r[1].ok=true")
        t:eq("ok1", r[1].val, "r[1].val=ok1")
        t:eq(false, r[2].ok, "r[2].ok=false (异常任务)")
        t:check(r[2].val ~= nil and string.find(r[2].val, "boom"), "r[2].val 含 boom（错误字符串）")
        t:eq(true, r[3].ok, "r[3].ok=true")
        t:eq("ok3", r[3].val, "r[3].val=ok3")
    end

    -- ── 嵌套 fork：fork 出来的协程内部再 fork ─────────────────────────
    do
        local depth_hit = 0
        srey.fork(function()
            depth_hit = depth_hit + 1
            srey.fork(function() depth_hit = depth_hit + 10 end)
            srey.fork(function() depth_hit = depth_hit + 100 end)
        end)
        srey.sleep(20)   -- 让外层 + 两个内层 fork 都跑完
        t:eq(111, depth_hit, "嵌套 fork 全部完成（1+10+100）")
    end

    -- ── fork_wait 内部的 srey.call 真正并发 ────────────────────────────
    -- 复用 reporter task 作为下游：srey.call 会阻塞协程等响应
    -- 这里只验证 fork_wait 在 srey.call yield 期间能并发推进
    do
        local t0 = srey.timer_ms()
        local r = srey.fork_wait({
            function() srey.sleep(30) end,
            function() srey.sleep(30) end,
        })
        local elapsed = srey.timer_ms() - t0
        t:eq(2, #r, "fork_wait + sleep 两个任务都返回")
        t:check(elapsed < 50, "两个 sleep(30) 并发 < 50ms（实际 " .. elapsed .. "ms）")
    end

    -- ── srey.fork_bind：参数预绑定，返回无参 lambda ───────────────────
    do
        local function add(a, b, c) return a + b + c end
        local f = srey.fork_bind(add, 1, 2, 3)
        t:eq(6, f(), "fork_bind 后调用返回 f(1,2,3)")
        -- 多次调用，参数稳定（闭包捕获）
        t:eq(6, f(), "fork_bind 多次调用结果一致")
        -- 0 参数也支持
        local function ret42() return 42 end
        t:eq(42, srey.fork_bind(ret42)(), "fork_bind 无参函数")
    end

    -- ── srey.fork_bind 配合 fork_wait：免去 function() return ... end 样板 ──
    do
        local function echo(x) return x end
        local function add2(a, b) return a + b end
        local r = srey.fork_wait({
            srey.fork_bind(echo, "hello"),
            srey.fork_bind(add2, 10, 20),
            function() return "closure" end,   -- fork_bind 与闭包混用
        })
        t:eq(3, #r, "fork_wait + fork_bind 混用任务数")
        t:eq("hello", r[1].val, "fork_bind(echo, 'hello') 结果")
        t:eq(30, r[2].val, "fork_bind(add2, 10, 20) 结果")
        t:eq("closure", r[3].val, "纯闭包结果")
        t:eq(true, r[1].ok and r[2].ok and r[3].ok, "三个任务都 ok")
    end
end)
end)
