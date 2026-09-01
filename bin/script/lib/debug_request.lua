-- task 调试请求处理：解析 REQ_DEBUG seri 序列化命令并在当前 task 的 Lua 虚拟机内执行。
-- 支持命令：mem / gc / stat / coros / loglv / inject / hotfix。
-- 使用契约：首次 _dispatch 之前必须把全部 _set_* 注入器调一遍——_set_coro_sess、_set_response、
-- _set_mtype_names、_set_suspend_registries、_set_fallback，缺一个就会在 dump 时撞 nil 索引。

local seri   = require("srey.seri")
local inject = require("lib.inject")
local task   = require("srey.task")
local M = {}

local _coro_sess -- 由 _set_coro_sess 注入的 coro_sess 只读引用（不要写）
local _response -- 由 _set_response 注入的响应函数：(dst, reqtype, sess, erro, data) → void
local _mtype_names -- 由 _set_mtype_names 注入：mtype 整数 → 名字字符串（MSG_TYPE 反转表）
local _mtype_max = 0 -- 同上注入：反转表里的最大 mtype，stat 按下标遍历到此为止
local _fallback -- 由 _set_fallback 注入：未知 debug 命令时透传给业务 on_requested：(reqtype,sess,src,data,size) → void
local _fork_barriers -- 由 _set_suspend_registries 注入：fork_wait 屏障集合（只读）
local _serial_execs -- 同上：serial 执行器状态集合（只读）
local _nyield -- 同上：取 nyield 的函数，用于与 _closing_dispatch 报的数对账

-- 遍历 coro_sess，按 coroutine stack traceback 聚类去重；返回可读字符串
-- 每个聚类记录最长挂起时长 maxage（毫秒），并按 maxage 降序输出，便于定位卡死协程。
-- serial 行给执行器地址 / 持锁协程 / 持锁多久 / 排队人数与最久那个排了多久：空闲 serial 不出行，
-- 排队时长只在真有人排队时才给。格式与计数口径同 C 侧 coro_dump
local function _dump_coros()
    local now = task.timer_ms()
    local total = 0
    local nsess = 0 -- coro_sess 的条目数，与 total(挂起协程数)不是一回事，理由见汇总行
    local clusters = {} -- traceback → { count, samples = {sess,...}, mtype, maxage }
    for sess, corosess in pairs(_coro_sess) do
        nsess = nsess + 1
        for _, info in ipairs(corosess.waiters) do
            if info.coro then    -- 跳过 func 模式（无挂起协程）
                total = total + 1
                local age = info.since and (now - info.since) or 0
                local trace = debug.traceback(info.coro, nil, 0)
                local c = clusters[trace]
                if c then
                    c.count = c.count + 1
                    if age > c.maxage then
                        c.maxage = age
                    end
                    if #c.samples < 5 then
                        c.samples[#c.samples + 1] = sess
                    end
                else
                    clusters[trace] = { count = 1, samples = { sess }, mtype = info.mtype, maxage = age }
                end
            end
        end
    end
    -- fork_wait 与 serial 的等待者走裸 yield，不在 coro_sess 里，只能靠登记表枚举；
    -- 它们同样计进 nyield，漏掉就会出现"关闭时报还有 N 个协程、coros 却说一个都没有"
    local nfork, nserial = 0, 0
    local extra = {}
    for b in pairs(_fork_barriers) do
        nfork = nfork + 1
        extra[#extra + 1] = string.format("fork_wait pending=%d age=%dms", b.pending, now - b.since)
    end
    for st in pairs(_serial_execs) do
        local nwait, oldest = 0, now
        for _, w in ipairs(st.waiters) do
            if w.since < oldest then
                oldest = w.since
            end
            nwait = nwait + 1
        end
        local hold = st.current and (now - st.since) or 0
        if nwait > 0 then
            extra[#extra + 1] = string.format("serial=%s co=%s held=%d hold=%dms waiters=%d age=%dms",
                tostring(st), tostring(st.current), st.current and 1 or 0, hold, nwait, now - oldest)
        elseif st.current then
            extra[#extra + 1] = string.format("serial=%s co=%s held=1 hold=%dms waiters=0",
                tostring(st), tostring(st.current), hold)
        end
        nserial = nserial + nwait
    end
    local lines = {}
    if total > 0 then
        -- 转数组并按最长挂起时长降序（卡死协程排最前）
        local list = {}
        for trace, c in pairs(clusters) do
            list[#list + 1] = { trace = trace, count = c.count, samples = c.samples, mtype = c.mtype, maxage = c.maxage }
        end
        table.sort(list, function(a, b) return a.maxage > b.maxage end)
        lines[#lines + 1] = string.format("=== %d suspended coros in %d stacks ===", total, #list)
        for _, c in ipairs(list) do
            lines[#lines + 1] = ""
            local samples = table.concat(c.samples, ",")
            if c.count > #c.samples then
                samples = samples .. ",..."
            end
            lines[#lines + 1] = string.format("[%dx] mtype=%d maxage=%dms sess=%s", c.count, c.mtype, c.maxage, samples)
            lines[#lines + 1] = c.trace
        end
    end
    if #extra > 0 then
        if #lines > 0 then
            lines[#lines + 1] = ""
        end
        for _, e in ipairs(extra) do
            lines[#lines + 1] = e
        end
    end
    if 0 == #lines then
        lines[#lines + 1] = "(no suspended coros)"
    end
    -- 汇总行无条件打印，不能跟着"一个都没有"一起早退：三张表都空而 nyield 非零，
    -- 正是最需要这行来对账的时候。C 侧 coro_dump 也是无条件 emit 的
    lines[#lines + 1] = ""
    -- sessions 是 coro_sess 的条目数：keep 的条目摘空 waiters 后仍留着复用，
    -- 所以 sessions 只增不减、suspended 长期为 0，就是 keep 条目泄漏。口径同 C 侧 coro_dump
    lines[#lines + 1] = string.format("%d suspended, %d sessions, %d fork_wait, %d serial, %d yield total.",
        total, nsess, nfork, nserial, _nyield())
    return table.concat(lines, "\n")
end

-- 执行调试命令，在当前 task 的 Lua 虚拟机中运行
-- 命令以位置化 seri 解出：cmd 为首参，后续 a1/a2 为该命令参数
--   loglv → a1=lv;  inject → a1=code;  hotfix → a1=module, a2=source
-- 返回结果文本字符串（含成功/失败说明，由 [OK]/[ERR] 前缀区分）；未知命令返回 nil 供 _dispatch 透传
local function _debug_handle(cmd, a1, a2)
    if "mem" == cmd then
        return string.format("%.2f KB", collectgarbage("count"))
    elseif "gc" == cmd then
        local before = collectgarbage("count")
        collectgarbage("collect")
        local after = collectgarbage("count")
        return string.format("before=%.2fKB  after=%.2fKB  freed=%.2fKB",
            before, after, before - after)
    elseif "stat" == cmd then
        local st = task.stat()
        local lines = { string.format("%-14s %12s %18s %14s",
            "MTYPE", "NMSG", "DISPATCH_CPU_NS", "AVG_NS") }
        -- 按下标走到最大 mtype 而不用 ipairs：枚举值一旦出现空洞 ipairs 会在洞前停下，
        -- 而 C 侧 debug_request.c 是按下标全量遍历，两边输出会静默分叉
        for mt = 1, _mtype_max do
            local name = _mtype_names[mt]
            local s = st.by_type[mt]
            if name and s then
                lines[#lines + 1] = string.format("%-14s %12d %18d %14.0f",
                    name, s.nmsg, s.dispatch_cpu_ns, s.dispatch_cpu_ns / s.nmsg)
            end
        end
        local t = st.total
        local avg = t.nmsg > 0 and (t.dispatch_cpu_ns / t.nmsg) or 0
        lines[#lines + 1] = string.format("%-14s %12d %18d %14.0f",
            "TOTAL", t.nmsg, t.dispatch_cpu_ns, avg)
        return table.concat(lines, "\n")
    elseif "coros" == cmd then
        return _dump_coros()
    elseif "loglv" == cmd then
        if not log_setlv(a1) then
            return string.format("invalid log level: %s", tostring(a1))
        end
        return string.format("log level => %d", a1)
    elseif "inject" == cmd then
        local ok, out = inject(a1)
        local lines = (out and #out > 0) and table.concat(out, "\n") or "(no output)"
        return (ok and "[OK]" or "[ERR]") .. "\n" .. lines
    elseif "hotfix" == cmd then
        local hotfix = require("lib.hotfix")
        local ok, msg = hotfix.apply(a1, a2)
        return (ok and "[OK] " or "[ERR] ") .. tostring(msg)
    else
        return nil  -- 未知命令：返回 nil 作标志，由 _dispatch 透传给业务 on_requested
    end
end

---注入 srey 挂起会话表 coro_sess 的只读引用；仅初始化阶段调用一次
---@param corosess table<integer, CoroSession> 由 lib/srey 持有的 coro_sess 表
function M._set_coro_sess(corosess)
    _coro_sess = corosess
end

---注入响应函数（通常是 srey.response）；仅初始化阶段调用一次
---@param respfunc fun(dst:integer, reqtype:integer, sess:integer, erro:integer, data:string) 响应回调
function M._set_response(respfunc)
    _response = respfunc
end

---注入 srey.lua 的 MSG_TYPE 枚举（name → int），内部反转为 int → name 表供 stat 渲染使用；
---仅初始化阶段调用一次
---@param msgtype MSG_TYPE MSG_TYPE 枚举表
function M._set_mtype_names(msgtype)
    _mtype_names = {}
    _mtype_max = 0
    for name, val in pairs(msgtype) do
        _mtype_names[val] = name
        if val > _mtype_max then
            _mtype_max = val
        end
    end
end

---注入 fork_wait 屏障与 serial 执行器的活跃登记表，以及取 nyield 的函数；仅初始化阶段调用一次。
---这两类等待者走裸 yield、不进 coro_sess，不注入的话 coros 只能看见 _coro_wait 那一类，
---而 task 关闭时 _closing_dispatch 报的 nyield 恰恰把它们算在内，两个数对不上
---@param barriers table<ForkBarrier,boolean> fork_wait 屏障集合（srey.lua 的 fork_barriers，只读）
---@param execs table<SerialState,boolean> serial 执行器状态集合（srey.lua 的 serial_execs，弱键，只读）
---@param nyieldfn fun():integer 取当前挂起协程总数
function M._set_suspend_registries(barriers, execs, nyieldfn)
    _fork_barriers = barriers
    _serial_execs = execs
    _nyield = nyieldfn
end

---注入未知命令透传函数；收到非内置 debug 命令时调用，交业务 on_requested 处理；仅初始化阶段调用一次
---@param fallback fun(reqtype:integer, sess:integer, src:integer, data:lightuserdata, size:integer) 透传回调
function M._set_fallback(fallback)
    _fallback = fallback
end

---REQUEST_TYPE.REQ_DEBUG 请求入口：解析 JSON 命令，执行后回复结果
---@param reqtype integer 请求类型（REQ_DEBUG）
---@param sess integer 会话 id
---@param src integer 请求方 task name
---@param data lightuserdata 请求数据指针
---@param size integer 请求数据长度
function M._dispatch(reqtype, sess, src, data, size)
    local ok, cmd, a1, a2 = pcall(seri.unpack, data, size)
    if not ok then
        -- debug 响应一律 ERR_OK，结果(含错误说明)由文本承载；否则 srey.request 对 erro!=OK 吞 data
        _response(src, reqtype, sess, ERR_OK, "invalid seri: " .. tostring(cmd))
        return
    end
    local hok, result = pcall(_debug_handle, cmd, a1, a2)
    if not hok then
        -- 命令处理内部抛错(如 inject 收非 string 源码致 load 抛错):转文本回错,避免请求方挂到 request_timeout
        _response(src, reqtype, sess, ERR_OK, "[ERR] " .. tostring(result))
        return
    end
    if nil == result then
        -- 未知命令：透传给业务 on_requested（对齐 C 端 _debug_request 返回 ERR_FAILED 的语义）
        if _fallback then
            _fallback(reqtype, sess, src, data, size)
        else
            _response(src, reqtype, sess, ERR_OK, "unknown command: " .. tostring(cmd))
        end
        return
    end
    _response(src, reqtype, sess, ERR_OK, result)
end

return M
