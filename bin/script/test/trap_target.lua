-- task.trap 单元测试的 helper task：
-- 收到 "spin" 进入纯 Lua 字节码死循环（占用 worker，等待 trap 中断）；
-- 收到 "ping" 返回 "pong"。spin 协程被 trap 中断后整个 task 恢复处理消息。

local srey = require("lib.srey")

srey.startup(function()
    srey.on_requested(function(_, sess, src, data, size)
        local txt = srey.ud_str(data, size)
        if txt == "spin" then
            -- 纯字节码循环，LUA_MASKCOUNT hook 触发后下一条字节码立即 luaL_error
            while true do
                local s = 0
                for i = 1, 1000 do s = s + i end
            end
        elseif txt == "ping" then
            srey.response(src, sess, 0, "pong")
        end
    end)
end)
