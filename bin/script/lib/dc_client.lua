-- DataCenter Lua 协程客户端:5 个协程版本 API 直接走 srey.request → DataCenter task service。
-- payload 与 lib/services/datacenter.c 协议一致(reqtype 统一 REQUEST_TYPE.REQ_DC,首字节 u8 op 标识子命令):
--   SET   = | u8 0x01 | u16 klen | key | u32 vlen | val |
--   GET   = | u8 0x02 | u16 klen | key |
--   WAIT  = | u8 0x03 | u16 klen | key |
--   DEL   = | u8 0x04 | u16 klen | key |
--   LIST  = | u8 0x05 |

local srey = require("lib.srey")
-- 子命令操作码,与 datacenter.c 内部 dc_op 枚举对齐
local OP_SET  = "\x01"
local OP_GET  = "\x02"
local OP_WAIT = "\x03"
local OP_DEL  = "\x04"
local OP_LIST = "\x05"

local dc_client = {}

---写入或覆盖 KV;唤醒所有该 key 的 waiter。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name(与 C 层 dc_start 注册一致)
---@param key string key 字符串(非空)
---@param val string? value 数据;nil 或空串等价软清空
---@return integer erro ERR_OK 成功;ERR_FAILED key 非法/datacenter 不可达/超时
function dc_client.set(dc_name, key, val)
    if not key or "" == key then
        WARN("dc set key empty.")
        return ERR_FAILED
    end
    -- string.pack(">s2", key):u16 klen 网络序 + key 字节
    -- string.pack(">s4", val or ""):u32 vlen 网络序 + val 字节(空 val 也写 0)
    local payload = OP_SET .. string.pack(">s2", key) .. string.pack(">s4", val or "")
    local _, size = srey.request(dc_name, REQUEST_TYPE.REQ_DC, payload)
    if nil == size then
        return ERR_FAILED
    end
    return ERR_OK
end

---读 KV;key 不存在返回 nil。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name
---@param key string key 字符串(非空)
---@return string? val 值字符串;key 不存在/超时返回 nil
function dc_client.get(dc_name, key)
    if not key or "" == key then
        WARN("dc get key empty.")
        return nil
    end
    local payload = OP_GET .. string.pack(">s2", key)
    local data, size = srey.request(dc_name, REQUEST_TYPE.REQ_DC, payload)
    if nil == size or nil == data then
        return nil
    end
    return srey.ud_str(data, size)
end

---读 KV;key 不存在则挂起协程直到 set 触发,或 request_timeout 超时。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name
---@param key string key 字符串(非空)
---@return string? val 值字符串;超时返回 nil
function dc_client.wait(dc_name, key)
    if not key or "" == key then
        WARN("dc wait key empty.")
        return nil
    end
    local payload = OP_WAIT .. string.pack(">s2", key)
    local data, size = srey.request(dc_name, REQUEST_TYPE.REQ_DC, payload)
    if nil == size or nil == data then
        return nil
    end
    return srey.ud_str(data, size)
end

---删除指定 key 的 KV 条目;只清 _kv,不影响 _pending(已等的 waiter 继续等)。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name
---@param key string key 字符串(非空)
---@return integer erro ERR_OK 成功(key 不存在也返 OK);ERR_FAILED key 非法/超时
function dc_client.del(dc_name, key)
    if not key or "" == key then
        WARN("dc del key empty.")
        return ERR_FAILED
    end
    local payload = OP_DEL .. string.pack(">s2", key)
    local _, size = srey.request(dc_name, REQUEST_TYPE.REQ_DC, payload)
    if nil == size then
        return ERR_FAILED
    end
    return ERR_OK
end

---列出全部 key。调试用,生产 key 量大时谨慎调。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name
---@return string[]? keys key 字符串数组;空 DC 返回空表;超时返回 nil
function dc_client.keys(dc_name)
    local data, size = srey.request(dc_name, REQUEST_TYPE.REQ_DC, OP_LIST)
    if nil == size then
        return nil
    end
    local result = {}
    if nil == data or 0 == size then
        return result
    end
    local buf = srey.ud_str(data, size)
    local total = #buf
    local pos = 1
    local k
    while pos <= total do
        k, pos = string.unpack(">s2", buf, pos)
        result[#result + 1] = k
    end
    return result
end

return dc_client
