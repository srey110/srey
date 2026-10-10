-- 接收组用例的回显组员：收到什么回什么，前面加上 "<组员号>:"
-- 注册参数：task.register("test.group_echo", "group_echo_<i>", 0, i)，由调用方随后 task.bind_net 到第 i 个 net 线程

local srey = require("lib.srey")

local _idx = ...

srey.startup(function()
    srey.on_recved(function(_, sk, _, _, data, size)
        srey.send(sk, tostring(_idx) .. ":" .. srey.ud_str(data, size))
    end)
end)
