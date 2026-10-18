-- 注册前(chunk 顶层)调 task.close 的回归 helper：注册前关只记标志，注册时在 STARTUP 之后补发 CLOSING。
-- startup 与 closing 各 srey.call 回 parent(reqtype 202)，parent 据此断言两者各跑一次且先后不乱。
-- 注册参数:task.register("test.close_before_reg", NAME, 0, parent_name)

local srey = require("lib.srey")
local task = require("srey.task")

local _parent = ...

task.close()

srey.startup(function()
    srey.call(_parent, 202, "startup")
end)
srey.closing(function()
    srey.call(_parent, 202, "closing")
end)
