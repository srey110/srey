-- srey.multi_call / srey.multi_request 测试用 subscriber：
--   reqtype=100 (multi_call BROADCAST_REQ)：通过 srey.call 单向 ack 回 publisher (reqtype=101)
--   reqtype=102 (multi_request RPC_REQ)：sess/src 非 0 时通过 srey.response 回带响应,触发 publisher on_responsed
-- 注册参数：task.register("test.multi_call_sub", "multi_call_sub_X", 0, parent_name, index)

local srey = require("lib.srey")

local _parent, _idx = ...

srey.startup(function()
    srey.on_requested(function(reqtype, sess, src, data, size)
        if not data or 0 == size then
            return
        end
        local payload = srey.ud_str(data, size)
        if 100 == reqtype then       -- BROADCAST_REQ：multi_call 路径
            srey.call(_parent, 101, tostring(_idx) .. ":" .. payload)
        elseif 102 == reqtype then   -- RPC_REQ：multi_request 路径,task_response 回 src
            if src ~= TASK_NAME.NONE and sess ~= 0 then
                srey.response(src, reqtype, sess, 0, "ack" .. tostring(_idx))
            end
        end
    end)
end)
