-- 崩溃回归 helper：在 chunk 顶层（srey.startup 之前）调 interruptible。
-- ltask->lua 若拖到 chunk 跑完才赋值，这里就是 lua_gethook(NULL) → SIGSEGV。

local srey = require("lib.srey")

srey.interruptible()

srey.startup(function()
end)
