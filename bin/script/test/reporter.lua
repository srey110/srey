-- 测试结果汇总 task：等收齐 N 个模块上报后输出总报告
-- 注册参数：task.register("test.reporter", "reporter", 0, N)，N 为期望模块数

local srey = require("lib.srey")
local json = require("cjson")

local _expected = ...
_expected = _expected or 1

local _results = {}
local _total_pass = 0
local _total_fail = 0
local _fail_modules = 0

local function _print_summary()
    printd("================ test summary ================")
    for _, r in ipairs(_results) do
        if r.nfail == 0 then
            printd("  [OK]   %-14s %4d ok", r.module, r.npass)
        else
            WARN("  [FAIL] %-14s %4d ok / %d FAIL", r.module, r.npass, r.nfail)
        end
    end
    if _total_fail == 0 then
        printd("[runner] all %d modules passed, %d assertions ok.",
               _expected, _total_pass)
    else
        WARN("[runner] %d/%d modules FAILED. total %d ok, %d FAIL.",
             _fail_modules, _expected, _total_pass, _total_fail)
    end
    printd("===============================================")
end

srey.startup(function()
    srey.on_requested(function(_, _, _, data, size)
        if not data or 0 == size then
            return
        end
        local txt = srey.ud_str(data, size)
        local ok, r = srey.xpcall(json.decode, txt)
        if not ok or type(r) ~= "table" then
            WARN("[reporter] decode error: %s", tostring(r))
            return
        end
        _results[#_results + 1] = r
        _total_pass = _total_pass + (r.npass or 0)
        _total_fail = _total_fail + (r.nfail or 0)
        if (r.nfail or 0) > 0 then
            _fail_modules = _fail_modules + 1
        end
        if #_results >= _expected then
            _print_summary()
        end
    end)
end)
