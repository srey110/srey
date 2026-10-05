-- Redis 客户端工具库。
-- 提供：连接（含 AUTH）、RESP 协议序列化（pack）、
-- 以及完整的 RESP3 多节点响应解包（unpack，整段在 C 绑定层做）。

local srey       = require("lib.srey")
local srey_redis = require("srey.redis")
local redis   = {}

---连接 Redis 服务器并可选地执行 AUTH 认证
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param psw string? 密码；为 nil 或空时跳过 AUTH
---@param netev NET_EV? 事件订阅掩码
---@return userdata sk 连接标识；失败时 sk.valid 为 false
function redis.connect(ip, port, sslname, psw, netev)
    local sk = srey.connect(PACK_TYPE.REDIS, sslname, ip, port, netev)
    if not sk.valid then
        return srey.sock_invalid()
    end
    if str_nullorempty(psw) then
        return sk
    end
    -- 发送 AUTH 命令验证密码
    local auth = redis.pack("AUTH", psw)
    local rtn, _ = srey.syn_send(sk, auth, #auth, 1)
    local result = rtn and redis.unpack(rtn)
    if "OK" ~= result then
        srey.close(sk)
        return srey.sock_invalid()
    end
    return sk
end

---将命令及参数序列化为 RESP 协议字符串（inline array），整条在 C 里编：参数按 tostring 转串，
---整数值的浮点按整数写（同 num_str），nil 编成空 bulk 并告警
---@type fun(...:any):string
redis.pack = srey_redis.pack

---读取当前响应节点的值
---@type fun(pk:lightuserdata?):string|integer|number|boolean|nil|RedisAggValue
redis.value = srey_redis.value

---获取响应链表中下一个节点指针
---@type fun(pk:lightuserdata?):lightuserdata|nil
redis.next = srey_redis.next

---将 C 层 RESP3 响应链表整段解包为 Lua 值（各类型怎么转见 lprot.c 的 _lprot_redis_unpack）
---@type fun(pk:lightuserdata?):any
redis.unpack = srey_redis.unpack

return redis
