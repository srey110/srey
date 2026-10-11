-- 部分组员用例的组员：请求 110 把自己设成只占所在线程的接收组(其余线程填 INVALID_TNAME)并监听，回 "ok" / "fail"；
-- 请求 111 撤掉监听回 "ok"(请求类型避开框架保留的 REQUEST_TYPE)。连进来的包原样回显，前面加上 "<所在线程号>:"
-- 注册参数：task.register("test.group_part", 名字, 0, 端口)，由调用方随后 task.bind_net 到某个 net 线程

local srey = require("lib.srey")
local task = require("srey.task")

local _port = ...
local _lid

srey.startup(function()
    srey.on_recved(function(_, sk, _, _, data, size)
        srey.send(sk, tostring(task.net_index()) .. ":" .. srey.ud_str(data, size))
    end)
    srey.on_requested(function(reqtype, sess, src)
        local ok = false
        if 110 == reqtype then
            local hs = {}
            for i = 1, task.nnet() do
                hs[i] = INVALID_TNAME
            end
            hs[task.net_index() + 1] = task.handle()
            if srey.task_accept_group(nil, hs) then
                _lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", _port)
                ok = ERR_FAILED ~= _lid
            end
        elseif 111 == reqtype and _lid then
            srey.unlisten(_lid)
            ok = true
        end
        srey.response(src, reqtype, sess, 0, ok and "ok" or "fail")
    end)
end)
