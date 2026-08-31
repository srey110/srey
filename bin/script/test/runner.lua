-- 简易测试运行器：每个测试 task 创建一个 ctx，调用 check/eq 累计 npass/nfail，
-- 结束后向 reporter task 上报；失败用 WARN 高亮，成功打印 "<module> tested."
-- 模块名取自 srey.task_name()，脚本不自己声明：名字只写在 test.lua 的 TESTS 里一处，
-- 那张表同时用于注册 task 和喂给 reporter 的期望列表，三者对不上在结构上就不可能。
-- 使用方式：
--   local runner = require("test.runner")
--   srey.startup(function() runner.run(function(t)
--       t:check(...)
--       t:eq(...)
--   end) end)

local srey = require("lib.srey")
local json = require("yyjson")

---@class M
---@field name string 模块名（即 task 名）
---@field npass integer 通过数
---@field nfail integer 失败数
local M = {}
M.__index = M

---新建测试上下文
---@param name string 模块名
---@return M ctx
function M.new(name)
    return setmetatable({ name = name, npass = 0, nfail = 0 }, M)
end

---条件断言：cond 为真累加 npass，否则累加 nfail 并 WARN
---@param cond boolean
---@param msg string
function M:check(cond, msg)
    if cond then
        self.npass = self.npass + 1
    else
        self.nfail = self.nfail + 1
        WARN("[%s] FAIL: %s", self.name, tostring(msg))
    end
end

---等值断言
---@param expected any 期望值
---@param actual any 实际值
---@param msg string 断言描述
function M:eq(expected, actual, msg)
    if expected == actual then
        self.npass = self.npass + 1
    else
        self.nfail = self.nfail + 1
        WARN("[%s] FAIL %s: expected=%s, got=%s",
             self.name, tostring(msg), tostring(expected), tostring(actual))
    end
end

---直接记一次失败
---@param msg string
function M:fail(msg)
    self.nfail = self.nfail + 1
    WARN("[%s] FAIL: %s", self.name, tostring(msg))
end

---测试结束：打印结果，向 reporter 上报
function M:done()
    if self.nfail == 0 then
        printd("%s tested. (%d ok)", self.name, self.npass)
    else
        WARN("%s tested with FAILURES (%d ok, %d fail)", self.name, self.npass, self.nfail)
    end
    local payload = json.encode({
        module = self.name,
        npass  = self.npass,
        nfail  = self.nfail,
    })
    srey.call("reporter", 0, payload)
end

---运行测试体；body(t) 抛异常时记一次失败并继续上报。
---模块名取当前 task 名，不由调用方给——两处各写一份就会漂移，而漂移只表现为
---reporter 的一条 [MISS] 加一条来路不明的 [OK]，没有任何报错点名
---@param body fun(t:M) 测试体，参数为 ctx
function M.run(body)
    local t = M.new(srey.task_name())
    local ok, err = xpcall(body, debug.traceback, t)
    if not ok then
        t.nfail = t.nfail + 1
        WARN("[%s] test crashed: %s", name, tostring(err))
    end
    t:done()
end

return M
