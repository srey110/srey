-- task.trap 单元测试的 helper task：
-- 收到 "spin" 进入纯 Lua 字节码死循环（占用 worker，等待 trap 中断）；
-- 收到 "ping" 返回 "pong"。spin 协程被 trap 中断后整个 task 恢复处理消息。

local srey = require("lib.srey")

srey.startup(function()
    -- 自行声明可被中断：hook 只能由属主线程挂，所以必须 task 自己在这里调；
    -- 不调的话 task.trap 会直接返回 false
    srey.interruptible()
    srey.on_requested(function(reqtype, sess, src, data, size)
        local txt = srey.ud_str(data, size)
        if txt == "spin" then
            -- 纯字节码循环，hook 每 TRAP_HOOK_COUNT 条字节码检查一次 trap 标志
            while true do
                local s = 0
                for i = 1, 1000 do s = s + i end
            end
        elseif txt == "ping" then
            srey.response(src, reqtype, sess, 0, "pong")
        end
    end)
end)
