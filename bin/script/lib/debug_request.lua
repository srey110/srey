-- task 调试请求处理：解析 REQ_DEBUG JSON 命令并在当前 task 的 Lua 虚拟机内执行。
-- 支持命令：mem / gc / stat / coros / loglv / inject。
-- 使用契约：首次 _dispatch 之前必须调用 _set_coro_sess、_set_response、_set_mtype_names 注入依赖。

local seri   = require("srey.seri")
local inject = require("lib.inject")
local task   = require("srey.task")
local M = {}

local _coro_sess     -- 由 _set_coro_sess 注入的 coro_sess 只读引用（不要写）
local _response      -- 由 _set_response 注入的响应函数：(dst, reqtype, sess, erro, data) → void
local _mtype_names   -- 由 _set_mtype_names 注入：mtype 整数 → 名字字符串（MSG_TYPE 反转表）
local _fallback      -- 由 _set_fallback 注入：未知 debug 命令时透传给业务 on_requested：(reqtype,sess,src,data,size) → void

-- 遍历 coro_sess，按 coroutine stack traceback 聚类去重；返回可读字符串
-- 每个聚类记录最长挂起时长 maxage（毫秒），并按 maxage 降序输出，便于定位卡死协程
local function _dump_coros()
    local now = task.timer_ms()
    local total = 0
    local clusters = {}    -- traceback → { count, samples = {sess,...}, mtype, maxage }
    for sess, corosess in pairs(_coro_sess) do
        local list = corosess.disposable and { corosess.coroinfo } or corosess.coroinfo
        for _, info in ipairs(list) do
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
    if 0 == total then
        return "(no suspended coros)"
    end
    -- 转数组并按最长挂起时长降序（卡死协程排最前）
    local list = {}
    for trace, c in pairs(clusters) do
        list[#list + 1] = { trace = trace, count = c.count, samples = c.samples, mtype = c.mtype, maxage = c.maxage }
    end
    table.sort(list, function(a, b) return a.maxage > b.maxage end)
    -- 格式化输出
    local lines = { string.format("=== %d suspended coros in %d stacks ===", total, #list) }
    for _, c in ipairs(list) do
        lines[#lines + 1] = ""
        local samples = table.concat(c.samples, ",")
        if c.count > #c.samples then
            samples = samples .. ",..."
        end
        lines[#lines + 1] = string.format("[%dx] mtype=%d maxage=%dms sess=%s", c.count, c.mtype, c.maxage, samples)
        lines[#lines + 1] = c.trace
    end
    return table.concat(lines, "\n")
end

-- 执行调试命令，在当前 task 的 Lua 虚拟机中运行，返回 erro, result
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
        for mt, name in ipairs(_mtype_names) do
            local s = st.by_type[mt]
            if s then
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
    for name, val in pairs(msgtype) do
        _mtype_names[val] = name
    end
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
    local result = _debug_handle(cmd, a1, a2)
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
