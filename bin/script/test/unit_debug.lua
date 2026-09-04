-- debug_request 单元测试：7 条内置命令的输出格式 + 未知命令透传 + 处理内抛错转文本。
-- 不走网络：直接注入 _set_* 注入器再调 _dispatch，_response 换成捕获函数，
-- 于是 coros 的输入（coro_sess / fork 屏障 / serial 执行器 / nyield）全部可控，
-- 聚类条数与汇总行的五个数字都能钉死。注入器的"仅初始化阶段调一次"契约在这里
-- 是故意越过的：本 task 不对外提供 debug 服务，模块又是每 VM 一份，越不出这个 task

local srey   = require("lib.srey")
local runner = require("test.runner")
local seri   = require("srey.seri")
local task   = require("srey.task")
local utils  = require("srey.utils")
local dbg    = require("lib.debug_request")

-- 造两个挂在同一行 yield 的协程（traceback 相同 → 聚成一类）和一个挂在别处的
local function _park_a()
    coroutine.yield()
end
local function _park_b()
    coroutine.yield()
end
local function _spawn(fn)
    local co = coroutine.create(fn)
    coroutine.resume(co)
    return co
end

srey.startup(function()
runner.run(function(t)
    local captured
    local nresp = 0
    dbg._set_response(function(dst, reqtype, sess, erro, data)
        nresp = nresp + 1
        captured = data
    end)
    dbg._set_mtype_names(srey.MSG_TYPE)

    -- ── coros：输入全可控，逐项对账 ────────────────────────────────
    local now = task.timer_ms()
    local co1, co2, co3 = _spawn(_park_a), _spawn(_park_a), _spawn(_park_b)
    dbg._set_coro_sess({
        [11] = { waiters = { { coro = co1, mtype = 3, since = now - 100 } } },
        [12] = { waiters = { { coro = co2, mtype = 3, since = now - 200 } } },
        -- 同一条目里再挂一个 func 模式的等待者：它没有 coro，不该计进 suspended
        [13] = { waiters = { { coro = co3, mtype = 5, since = now - 9000 },
                             { func = function() end, mtype = 5 } } },
    })
    local barrier = { pending = 2, since = now - 300 }
    local sexec = { current = co1, since = now - 400,
                    waiters = { { since = now - 500 }, { since = now - 50 } } }
    dbg._set_suspend_registries({ [barrier] = true }, { [sexec] = true },
                                function() return 7 end)

    -- buf 得自己释放：真实路径上载荷由框架持有，_dispatch 只读不接管
    local function send(...)
        local buf, size = seri.pack(...)
        captured = nil
        dbg._dispatch(REQUEST_TYPE.REQ_DEBUG, 1, 0, buf, size)
        utils.ud_free(buf)
        return captured
    end

    local txt = send("coros")
    t:check(txt and txt:find("=== 3 suspended coros in 2 stacks ===", 1, true) ~= nil,
            "coros 头行：3 个挂起(func 模式不计) 聚成 2 类")
    t:check(txt and txt:find("[2x] mtype=3", 1, true) ~= nil, "coros 同栈两个聚成 [2x]")
    t:check(txt and txt:find("[1x] mtype=5", 1, true) ~= nil, "coros 另一栈 [1x]")
    -- maxage 降序：9000ms 那个（[1x]）必须排在 [2x] 前面
    local p1 = txt and txt:find("[1x]", 1, true)
    local p2 = txt and txt:find("[2x]", 1, true)
    t:check(p1 and p2 and p1 < p2, "coros 按 maxage 降序，最久的排最前")
    t:check(txt and txt:match("fork_wait pending=2 age=%d+ms") ~= nil, "coros fork_wait 行")
    t:check(txt and txt:match("held=1 hold=%d+ms waiters=2 age=%d+ms") ~= nil,
            "coros serial 行：持锁 + 2 个排队")
    -- 汇总行五个数：suspended=3 sessions=3 fork_wait=1 serial=2(排队人数) yield=注入的 7
    t:check(txt and txt:find("3 suspended, 3 sessions, 1 fork_wait, 2 serial, 7 yield total.",
            1, true) ~= nil, "coros 汇总行五个数")

    -- 三张表全空时汇总行仍须打印（nyield 非零正是要靠它对账）
    dbg._set_coro_sess({})
    dbg._set_suspend_registries({}, {}, function() return 4 end)
    txt = send("coros")
    t:check(txt and txt:find("(no suspended coros)", 1, true) ~= nil, "coros 空表提示")
    t:check(txt and txt:find("0 suspended, 0 sessions, 0 fork_wait, 0 serial, 4 yield total.",
            1, true) ~= nil, "coros 空表也打印汇总行")

    -- ── mem / gc ───────────────────────────────────────────────────
    txt = send("mem")
    t:check(txt and txt:match("^%d+%.%d%d KB$") ~= nil, "mem 格式 '<n>.<nn> KB' (" .. tostring(txt) .. ")")
    txt = send("gc")
    t:check(txt and txt:match("^before=[%d%.]+KB  after=[%d%.]+KB  freed=[%-%d%.]+KB$") ~= nil,
            "gc 三段格式 (" .. tostring(txt) .. ")")

    -- ── stat ───────────────────────────────────────────────────────
    -- MTYPE / TOTAL 两串都出自格式串常量：把 task.stat() 的数全归零，逐 mtype 的
    -- for 一行不出，body 只剩表头 + TOTAL，两串照样命中。明细行数才能证伪 ——
    -- 数换行不依赖列宽（表头一个，每条明细一个，TOTAL 不带）。
    -- 计数是 C 侧编译期开关(ENABLE_DISPATCH_STAT)，关闭时恒为 0、明细一行都不该有，
    -- 故期望取自同一份 task.stat()：有数就必须出明细，没数就必须没有
    local want_detail = task.stat().total.nmsg > 0
    txt = send("stat")
    local nline = txt and select(2, txt:gsub("\n", "")) or 0
    t:check(txt and txt:find("MTYPE", 1, true) ~= nil and txt:find("TOTAL", 1, true) ~= nil
            and (nline >= 2) == want_detail,
            "stat 表头 + TOTAL + 明细行数与计数开关一致 (nline=" .. nline
            .. " want_detail=" .. tostring(want_detail) .. ")")

    -- ── loglv：合法设回当前级别，非法不改状态 ──────────────────────
    local lv0 = utils.log_getlv()
    t:eq("log level => " .. lv0, send("loglv", lv0), "loglv 合法回显")
    -- 上界是 DEBUG=4；log.lua 的 LOG_LV 是模块局部, 这里直接写字面量
    txt = send("loglv", 5)
    t:check(txt and txt:find("invalid log level", 1, true) ~= nil, "loglv 越界拒绝")
    txt = send("loglv", "3")
    t:check(txt and txt:find("invalid log level", 1, true) ~= nil, "loglv 非整数拒绝")
    t:eq(lv0, utils.log_getlv(), "loglv 非法时级别不变")

    -- ── inject / hotfix ────────────────────────────────────────────
    t:eq("[OK]\nfrom-inject", send("inject", "print('from-inject')"), "inject 捕获输出")
    txt = send("inject", "syntax ((")
    t:check(txt and txt:sub(1, 5) == "[ERR]", "inject 编译错回 [ERR]")
    txt = send("hotfix", "no_such_module_xyz", "return 1")
    t:check(txt and txt:sub(1, 5) == "[ERR]", "hotfix 不存在的模块回 [ERR]")

    -- 处理内抛错不得逃出 _dispatch（逃了请求方就挂到 request_timeout）
    local before = nresp
    local ok = pcall(send, "inject", true)
    t:eq(true, ok, "inject 收非字符串源码：_dispatch 不抛出")
    t:eq(before + 1, nresp, "抛错路径同样回了一次响应")

    -- ── 未知命令：无 fallback 时回提示，有 fallback 时透传 ──────────
    dbg._set_fallback(nil)
    t:eq("unknown command: nosuchcmd", send("nosuchcmd"), "未知命令无 fallback 回提示")
    local fbcmd
    dbg._set_fallback(function(reqtype, sess, src, data, size)
        fbcmd = seri.unpack(data, size)
    end)
    before = nresp
    t:eq(nil, send("nosuchcmd2"), "未知命令有 fallback 时不自己回响应")
    t:eq("nosuchcmd2", fbcmd, "fallback 收到原始载荷")
    t:eq(before, nresp, "透传路径没有额外响应")

    coroutine.resume(co1)
    coroutine.resume(co2)
    coroutine.resume(co3)
end)
end)
