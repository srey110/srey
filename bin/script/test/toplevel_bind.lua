-- 崩溃回归 helper：在 chunk 顶层（srey.startup 之前）调 C 绑定。
-- 绑定层依赖的东西必须在跑 chunk 之前就落定（ltask->lua、_curtask 全局），
-- 拖到 chunk 跑完才赋值的话，顶层这一句就是空指针解引用，整个进程 SIGSEGV。
-- 同时兼作 task.register 变参搬运用例的目标：不读 ...，只关心参数搬运本身不越界。

local srey = require("lib.srey")

assert(nil ~= srey.task_handle())

srey.startup(function()
end)
