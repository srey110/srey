-- srey.serial 协程串行化执行器单元测试：基础进入、嵌套、FIFO 串行、抛错锁释放、cs 内 yield

local srey   = require("lib.srey")
local runner = require("test.runner")

srey.startup(function()
runner.run("serial", function(t)
    -- ── 基础：单协程进入，返回 (ok, ret) ─────────────────────────────
    do
        local cs = srey.serial()
        local entered = false
        local ok, ret = cs(function() entered = true; return 42 end)
        t:eq(true, ok, "单协程 ok=true")
        t:eq(42, ret, "单协程返回值")
        t:eq(true, entered, "f 已被调用")
    end

    -- ── 同协程嵌套 cs 不死锁 ─────────────────────────────────────────
    do
        local cs = srey.serial()
        local outer, inner = false, false
        local ok = cs(function()
            outer = true
            local iok, _ = cs(function() inner = true end)
            t:eq(true, iok, "嵌套内层 ok=true")
        end)
        t:eq(true, ok, "嵌套外层 ok=true")
        t:eq(true, outer, "外层 f 执行")
        t:eq(true, inner, "内层 f 执行")
    end

    -- ── 跨协程串行化 + FIFO 顺序：A 持锁 sleep，B/C 排队等待 ──────────
    do
        local cs = srey.serial()
        local order = {}
        srey.fork(function()
            cs(function()
                order[#order + 1] = "A_enter"
                srey.sleep(20)    -- A 持锁 yield，B/C 必须等
                order[#order + 1] = "A_leave"
            end)
        end)
        srey.fork(function()
            cs(function() order[#order + 1] = "B" end)
        end)
        srey.fork(function()
            cs(function() order[#order + 1] = "C" end)
        end)
        srey.sleep(60)            -- 等三个协程都完成
        t:eq(4, #order, "三协程串行完成 (A_enter/A_leave/B/C)")
        t:eq("A_enter", order[1], "A 先进入")
        t:eq("A_leave", order[2], "A 必须完全退出后 B 才进入（串行）")
        t:eq("B",       order[3], "FIFO: B 在 A 后")
        t:eq("C",       order[4], "FIFO: C 在 B 后")
    end

    -- ── f 抛错时锁正常释放，下个等待者继续 ────────────────────────────
    do
        local cs = srey.serial()
        local ok = cs(function() error("boom") end)
        t:eq(false, ok, "抛错时 ok=false")
        -- 锁已释放，第二次正常进入
        local ok2, ret = cs(function() return 99 end)
        t:eq(true, ok2, "抛错后下次仍可进入")
        t:eq(99, ret, "下次返回值正确")
    end

    -- ── 持锁协程抛错也唤醒等待者（B 不会死等）────────────────────────
    do
        local cs = srey.serial()
        local b_done = false
        srey.fork(function()
            cs(function() srey.sleep(10); error("A_crash") end)
        end)
        srey.fork(function()
            cs(function() b_done = true end)
        end)
        srey.sleep(40)
        t:eq(true, b_done, "A 抛错后 B 仍被唤醒执行")
    end

    -- ── 多次 cs 独立实例互不影响 ──────────────────────────────────────
    do
        local cs1 = srey.serial()
        local cs2 = srey.serial()
        local hit = 0
        srey.fork(function()
            cs1(function() srey.sleep(15); hit = hit + 1 end)
        end)
        srey.fork(function()
            cs2(function() hit = hit + 10 end)   -- 不同 cs，无需等 cs1
        end)
        srey.sleep(5)
        t:eq(10, hit, "cs2 不被 cs1 阻塞（独立锁）")
        srey.sleep(30)
        t:eq(11, hit, "cs1 也完成")
    end

    -- ── cs 内调用 srey.request 等 yield 操作时锁仍保持 ──────────────
    -- 用 srey.sleep 模拟 yield 操作（无需依赖网络）
    do
        local cs = srey.serial()
        local in_cs = 0          -- 同时在 cs 内的协程数
        local peak  = 0
        srey.fork(function()
            cs(function()
                in_cs = in_cs + 1; if in_cs > peak then peak = in_cs end
                srey.sleep(20)
                in_cs = in_cs - 1
            end)
        end)
        srey.fork(function()
            cs(function()
                in_cs = in_cs + 1; if in_cs > peak then peak = in_cs end
                srey.sleep(20)
                in_cs = in_cs - 1
            end)
        end)
        srey.sleep(60)
        t:eq(1, peak, "互斥：任意时刻最多 1 个协程在 cs 内")
        t:eq(0, in_cs, "两协程均已退出 cs")
    end

    -- ── cs 出口 coro_running 还原（与 C 层 coro_serial_call B05 镜像）─
    -- 触发条件：A 持锁 sleep 期间 B 排队入 cs；A 完成 _release 唤醒 B,B 在
    -- cs 内 yield 后 _release 返回,A 的 cs 闭包返回；A 继续调 srey.sleep
    -- —— 修复前 coro_running stale=B → _set_coro_sess 登记错协程 →
    -- 后续消息按 B resume 触发 "attempt to call a table value" → A 永远完不成
    do
        local cs = srey.serial()
        local a_done = false
        local b_done = false
        srey.fork(function()
            cs(function() srey.sleep(20) end)    -- A 持锁 yield
            srey.sleep(5)                        -- cs 出口后再 yield —— B05 触发点
            a_done = true
        end)
        srey.fork(function()
            cs(function() srey.sleep(20) end)    -- B 排队 → A.release 唤醒 → B cs 内 yield
            b_done = true
        end)
        srey.sleep(80)
        t:eq(true, a_done, "A 在 cs 出口后的 srey.sleep 正常完成（coro_running 已还原）")
        t:eq(true, b_done, "B 正常完成")
    end
end)
end)
