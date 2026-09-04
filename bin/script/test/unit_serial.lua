-- srey.serial 协程串行化执行器单元测试：基础进入、嵌套、FIFO 串行、抛错锁释放、cs 内 yield，
-- 以及 coros 调试命令能否看见 serial / fork_wait 这两类不进 coro_sess 的等待者

local srey   = require("lib.srey")
local runner = require("test.runner")
local seri   = require("srey.seri")
local task   = require("srey.task")-- 起一个安静的 helper task，测"三张登记表全空"那条路径

srey.startup(function()
runner.run(function(t)
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

    -- ── cs 出口后 A 仍能正常 yield（历史上 coro_running 被写坏过的场景）──
    -- 触发条件：A 持锁 sleep 期间 B 排队入 cs；A 退出 cs 完成交接后继续调 srey.sleep。
    -- 唤醒摊平到 dispatch 末尾之后，_release 不再改写 coro_running，本用例作为回归护栏保留
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

    -- ── 长等待链：N 个连续同步完成的等待者不得触顶 C 调用深度 ────────────
    -- 修复前 _release 在持锁协程自己的栈上直接 resume 下一个，而 Lua 的 resume 是在同一条
    -- C 栈上嵌帧、nCcalls 从 resume 方继承，一串不 yield 的等待者链式唤醒累加到
    -- LUAI_MAXCCALLS(200) 就触顶，resume 返回 "C stack overflow"；此时 current/ref 已写死，
    -- 那个等待者永远不被唤醒也永远不 _release → 整个 serial 永久死锁，done 会停在 60~90 附近。
    -- 关键是等待者的 f 必须"不 yield"，一旦 yield 控制权就沿嵌套链回溯、链不会累积
    do
        local cs = srey.serial()
        local N = 200
        local done = 0
        srey.fork(function()
            cs(function() srey.sleep(20) end)-- 持锁 yield，逼后面 N 个全部排队
        end)
        for _ = 1, N do
            srey.fork(function()
                cs(function() done = done + 1 end) -- 同步完成，不 yield
            end)
        end
        srey.sleep(120)
        t:eq(N, done, "200 个连续同步完成的等待者全部执行（唤醒链已摊平）")
    end

    -- ── coros 要能看见 serial 队列与 fork_wait 屏障上的等待者 ────────
    -- 这两类走裸 coroutine_yield()，不进 coro_sess。漏登记的话就会出现
    -- "关闭时 _closing_dispatch 报还有 N 个协程没退，coros 却回 (no suspended coros)"
    do
        local cs = srey.serial()
        local release = false
        local function _hold()
            while not release do
                srey.sleep(5)
            end
        end
        srey.fork(function() cs(_hold) end) -- 占住执行器不放
        srey.sleep(20)
        srey.fork(function() cs(function() end) end) -- 排进 serial waiters
        srey.fork(function() srey.fork_wait({ _hold }) end) -- 停在 fork_wait 屏障上
        srey.sleep(40)

        local ptr, sz = seri.pack("coros")
        local rdata, rsize = srey.request(srey.task_handle(), REQUEST_TYPE.REQ_DEBUG, ptr, sz, 0)
        t:check(rdata ~= nil, "coros 调试命令有响应")
        local text = rdata and srey.ud_str(rdata, rsize) or ""
        t:check(text:find("serial=", 1, true) ~= nil, "coros 列出 serial 等待者")
        t:check(text:find("fork_wait pending=", 1, true) ~= nil, "coros 列出 fork_wait 屏障")
        -- 探针要带前导空格：协程聚类行的 maxage= 里也含 age=，裸 "age=" 会被它抢先满足，
        -- 那时把 fork_wait 与 serial 两行的 age 字段全删掉这条也照过
        t:check(text:find(" age=", 1, true) ~= nil, "fork_wait / serial 行带挂起时长，与 C 侧 coro_dump 同格式")
        t:check(text:find("hold=", 1, true) ~= nil, "serial 行带持锁时长")
        t:check(text:find("co=", 1, true) ~= nil, "serial 行带持锁协程，可与挂起段对上号")
        t:check(text:find("1 fork_wait", 1, true) ~= nil, "汇总行统计 fork_wait")
        t:check(text:find("1 serial", 1, true) ~= nil, "汇总行统计 serial")
        -- sessions 是 coro_sess 条目数，与 suspended 分开报：keep 的条目摘空 waiters 后仍留着，
        -- 两个数长期背离就是 keep 条目泄漏。C 侧 coro_dump 同格式
        t:check(text:find("sessions,", 1, true) ~= nil, "汇总行报 coro_sess 条目数")

        release = true
        srey.sleep(60)
    end

    -- ── 一个都没挂起时，汇总行仍须打印 ────────────────────────────────
    -- 自请求测不到这条：请求方自己就挂在 coro_sess 里，total 恒 >= 1。得向另一个
    -- 安静的 task 要 coros——它三张登记表全空，正是原来那句提前 return 吞掉汇总行的路径。
    -- 而三表全空却 nyield 非零，恰恰是最需要这行来对账的时刻
    do
        local quiet = task.register("test.toplevel_bind", "coros_quiet", 0)
        t:check(quiet ~= nil, "起一个不挂协程的 helper task")
        srey.sleep(30)

        local ptr, sz = seri.pack("coros")
        local rdata, rsize = srey.request("coros_quiet", REQUEST_TYPE.REQ_DEBUG, ptr, sz, 0)
        t:check(rdata ~= nil, "安静 task 的 coros 有响应")
        local text = rdata and srey.ud_str(rdata, rsize) or ""
        t:check(text:find("(no suspended coros)", 1, true) ~= nil, "空表时仍报 (no suspended coros)")
        t:check(text:find("yield total.", 1, true) ~= nil, "空表时汇总行照样打印")

        local h = task.grab("coros_quiet")
        if h then
            task.close(h)
            task.ungrab(h)
        end
    end
end)
end)
