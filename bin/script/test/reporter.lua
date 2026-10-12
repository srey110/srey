-- 测试结果汇总 task：等收齐全部模块上报后输出总报告，然后请求进程退出(同 ./bin/test 跑完自己收尾)
-- 注册参数：task.register("test.reporter", "reporter", 0, name1, name2, ...)，
-- 变参为期望上报的模块名（由 test.lua 的 TESTS 表推出）

local srey = require("lib.srey")
local json = require("yyjson")

-- 收不齐就一直等的话进程永远不退，一个崩在 done 之前的模块也看不出来。到点出一份残缺报告，
-- 没上报的计失败，照样收尾退出。
-- 这个值必须大于最慢单模块的自有预算，否则一次只是偏慢的运行会被判成 FAILED：
-- e2e_runner 串行跑 5 个 python 脚本、每个自带 60s 上限（见该文件的 _SCRIPTS 与 _PY_TIMEOUT_MS），
-- 单它最坏就要 300s 出头。改那边的脚本数或超时的话，这里要跟着抬
local REPORT_TIMEOUT = 600 * 1000

local _pending = {} -- 尚未上报的模块名 → true
local _expected = select("#", ...)
local _npending = _expected
for i = 1, _expected do
    _pending[(select(i, ...))] = true
end

local _results = {}
local _total_pass = 0
local _total_fail = 0
local _fail_modules = 0
local _exiting = false -- 已请求退出：超时收尾之后到的上报不再出汇总

---@return boolean ok 全部模块都上报且没有失败
local function _print_summary()
    printd("================ test summary ================")
    for _, r in ipairs(_results) do
        if r.nfail == 0 then
            printd("  [OK]   %-14s %4d ok", r.module, r.npass)
        else
            WARN("  [FAIL] %-14s %4d ok / %d FAIL", r.module, r.npass, r.nfail)
        end
    end
    local missing = {}
    for name in pairs(_pending) do
        missing[#missing + 1] = name
    end
    if #missing > 0 then
        table.sort(missing)
        WARN("  [MISS] %d modules never reported: %s", #missing, table.concat(missing, ", "))
    end
    local ok = 0 == _total_fail and 0 == #missing
    if ok then
        printd("[runner] all %d modules passed, %d assertions ok.",
               _expected, _total_pass)
    else
        WARN("[runner] %d/%d modules FAILED, %d never reported. total %d ok, %d FAIL.",
             _fail_modules, _expected, #missing, _total_pass, _total_fail)
    end
    printd("===============================================")
    return ok
end

-- 出汇总并请求退出，全过退出码 0，否则 1。收齐与超时两条路都走这里，只做一次
local function _finish()
    if _exiting then
        return
    end
    _exiting = true
    local ok = _print_summary()
    printd("[runner] shutting down.")
    srey.exit(ok and 0 or 1)
end

srey.startup(function()
    srey.on_requested(function(_, _, _, data, size)
        -- 三条丢弃分支都要留痕：静默丢的话，症状会退化成"全模块 MISS、汇总等超时、日志零线索"
        if not data or 0 == size then
            WARN("[reporter] empty payload, dropped.")
            return
        end
        local txt = srey.ud_str(data, size)
        local ok, r = srey.xpcall(json.decode, txt)
        if not ok or type(r) ~= "table" then
            WARN("[reporter] decode error: %s", tostring(r))
            return
        end
        -- 模块名先卡死类型再往下走：缺这个字段的话 _pending[nil] 抛 table index is nil，
        -- 就算躲过去，汇总里的 string.format("%-14s", nil) 也一样炸
        local name = r.module
        if "string" ~= type(name) then
            WARN("[reporter] report without module name, dropped.")
            return
        end
        -- 计数字段在入口补齐，别在每个读点各写一遍 or 0：汇总里的 %d 拿到 nil 一样会炸
        r.npass = r.npass or 0
        r.nfail = r.nfail or 0
        _results[#_results + 1] = r
        -- 只对"确实还欠着的名字"销账：重复上报或不在 TESTS 里的名字都不能让它减，
        -- 否则收齐判定会提前一格
        if _pending[name] then
            _pending[name] = nil
            _npending = _npending - 1
        end
        _total_pass = _total_pass + r.npass
        _total_fail = _total_fail + r.nfail
        if r.nfail > 0 then
            _fail_modules = _fail_modules + 1
        end
        -- 判"欠账清零"而不是"收到够多条"：后者被一条重复上报就能骗过去
        if _expected > 0 and 0 == _npending then
            _finish()
        end
    end)
    srey.timeout(REPORT_TIMEOUT, function()
        -- 已收齐的话 _finish 早做过了。没传期望名单时（_expected 为 0）无从判断收没收齐，由这里收尾
        if _exiting then
            return
        end
        if _expected > 0 then
            WARN("[reporter] timeout after %dms, only %d/%d modules reported.",
                 REPORT_TIMEOUT, #_results, _expected)
        end
        _finish()
    end)
end)
