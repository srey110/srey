-- srey 框架绑定层单元测试：srey.core (SSL/cert) + srey.task (timer/timeout)

local srey   = require("lib.srey")
local runner = require("test.runner")
local core   = require("srey.core")
local task   = require("srey.task")

srey.startup(function()
runner.run("framework", function(t)
    -- ── srey.core: SSL 证书注册与查询 ─────────────────────────────────
    -- 使用一组独立 name（unit_pem/unit_p12）避免污染其他 task 用的 SSL_NAME.SERVER/CLIENT
    local NAME_PEM = "unit_pem"
    local NAME_P12 = "unit_p12"
    do
        -- 注册 PEM 服务端证书；返回 ssl ctx lightuserdata，未注册时返回 nil
        local ssl = core.cert_register(NAME_PEM, "ca.crt", "server.crt", "server.key")
        t:check(ssl ~= nil, "cert_register PEM 返回 ssl ctx")
        -- ssl_qury 用同 name 查回同一指针
        local q = core.ssl_qury(NAME_PEM)
        t:check(q ~= nil, "ssl_qury PEM 返回 ssl ctx")
        t:eq(ssl, q, "ssl_qury 返回与 register 同一指针")
        -- 未注册的 name 返回 nil
        t:eq(nil, core.ssl_qury("unregistered"), "ssl_qury 未注册 name 返回 nil")
        -- 重复 register 同一 name 返回 nil（evssl_register 已占用）
        local dup = core.cert_register(NAME_PEM, "ca.crt", "server.crt", "server.key")
        t:eq(nil, dup, "重复 register 同 name 拒绝")

        -- ssl ctx 操作（不会崩溃即通过；这些函数无返回值）
        core.ssl_seclevel(ssl, 1)
        core.ssl_min_proto(ssl, TLS_VERSION.TLS1_2)
        core.ssl_verify(ssl, 0)
    end
    do
        -- p12_register
        local ssl = core.p12_register(NAME_P12, "client.p12", "srey")
        t:check(ssl ~= nil, "p12_register 返回 ssl ctx")
        t:eq(ssl, core.ssl_qury(NAME_P12), "ssl_qury 查回 p12 ssl ctx")
        -- 错误密码注册失败
        local bad = core.p12_register("unit_p12_bad", "client.p12", "wrong-password")
        t:eq(nil, bad, "p12_register 错误密码失败")
    end

    -- ── srey.task: timer_ms 单调递增 ──────────────────────────────────
    do
        local a = task.timer_ms()
        t:check(type(a) == "number" and a > 0, "timer_ms returns positive number")
        srey.sleep(2)  -- 让出协程让时间走过
        local b = task.timer_ms()
        t:check(b >= a, "timer_ms monotonic non-decreasing")
    end

    -- ── srey.task: set/get *_timeout round-trip ───────────────────────
    do
        -- 保存原值后还原（避免污染本 task 后续协程）
        local saved_req = task.get_request_timeout()
        local saved_con = task.get_connect_timeout()
        local saved_net = task.get_netread_timeout()

        task.set_request_timeout(7777)
        t:eq(7777, task.get_request_timeout(), "set/get_request_timeout")

        task.set_connect_timeout(8888)
        t:eq(8888, task.get_connect_timeout(), "set/get_connect_timeout")

        task.set_netread_timeout(9999)
        t:eq(9999, task.get_netread_timeout(), "set/get_netread_timeout")

        task.set_request_timeout(saved_req)
        task.set_connect_timeout(saved_con)
        task.set_netread_timeout(saved_net)
        t:eq(saved_req, task.get_request_timeout(), "request_timeout restored")
        t:eq(saved_con, task.get_connect_timeout(), "connect_timeout restored")
        t:eq(saved_net, task.get_netread_timeout(), "netread_timeout restored")
    end

    -- ── srey.task: isclosing / name (当前 task) ───────────────────────
    do
        -- 当前 task 未关闭
        t:eq(false, task.isclosing(), "current task not closing")
        -- 当前 task name = unit_framework
        t:eq("unit_framework", task.name(), "current task name")
    end

    -- ── srey.task: grab/ungrab 引用计数 ───────────────────────────────
    do
        -- 抓取 reporter task（必然存在）
        local rep = task.grab("reporter")
        t:check(rep ~= nil, "task.grab existing task")
        task.incref(rep)
        task.ungrab(rep)  -- 平衡 incref
        task.ungrab(rep)  -- 平衡 grab

        -- 不存在的 task name 返回 nil
        t:eq(nil, task.grab("__no_such_task__"), "task.grab missing returns nil")
    end

    -- ── srey.task: trap (跨 task 中断卡死协程) ────────────────────────
    -- 起一个 helper task 接收 "spin" 进入死循环，验证 task.trap 能从外部
    -- 安装 hook 中断协程；中断后 task 应能恢复处理新请求（"ping" → "pong"）。
    do
        -- 选用一个不在 TASK_NAME 表中的字符串名，与其他单测错开
        local TRAP_TARGET = "trap_target"
        task.register("test.trap_target", TRAP_TARGET, 0)
        srey.sleep(20)  -- 等 helper startup 完成

        -- 让 helper 进入 spin 死循环
        srey.call(TRAP_TARGET, 0, "spin")
        srey.sleep(50)  -- 等 spin 协程在 helper worker 上跑起来

        -- 跨 task 触发 trap
        t:eq(true, task.trap(TRAP_TARGET), "task.trap 有效目标返回 true")
        srey.sleep(100)  -- 等 hook 触发, 协程退出, helper 恢复消息循环

        -- 验证 helper 已恢复 —— ping 应能拿到 pong
        local rdata, rsize = srey.request(TRAP_TARGET, 0, "ping")
        t:check(rdata ~= nil, "trap 后 helper 恢复处理消息（收到 ping 响应）")
        if rdata then
            t:eq("pong", srey.ud_str(rdata, rsize), "ping 响应内容正确")
        end

        -- 无效 name 返回 false
        t:eq(false, task.trap("__no_such_task__"), "task.trap 无效 name 返回 false")

        -- 收尾：关闭 helper
        local helper = task.grab(TRAP_TARGET)
        if helper then
            task.close(helper)
            task.ungrab(helper)
        end
    end

    -- ── srey.task: mem / memlimit (per-task lua_State 内存监控) ──
    -- memlimit 为软告警阈值：超阈值仅 LOG_WARN，不拒绝分配、不触发 LUA_ERRMEM
    do
        -- task.mem() 返回 lua_State 当前累计字节数；新建 state 后内部已 alloc 若干，必非 0
        local m0 = task.mem()
        t:check(m0 > 0, "task.mem() returns positive (got " .. tostring(m0) .. ")")
        t:check(m0 < 1024 * 1024 * 1024, "task.mem() 合理范围 < 1GB (got " .. tostring(m0) .. ")")

        -- memlimit(0) 禁用告警：大分配应成功
        task.memlimit(0)
        local ok_big = pcall(function()
            local _ = string.rep("y", 64 * 1024)  -- 64KB
        end)
        t:eq(true, ok_big, "memlimit=0 时大分配应成功")

        -- memlimit = 当前 mem（告警阈值）：超阈值只 LOG_WARN、不拒绝分配 → 分配应成功
        collectgarbage("collect")
        local before = task.mem()
        task.memlimit(before)
        local ok_over = pcall(function()
            local _ = string.rep("z", 32 * 1024)
        end)
        task.memlimit(0)  -- 解除告警阈值
        t:eq(true, ok_over, "memlimit=current mem 超阈值时分配应成功(软告警不拒绝)")
    end
end)
end)
