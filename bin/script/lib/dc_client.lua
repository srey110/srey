-- DataCenter Lua 协程客户端:5 个 API 经 srey.datacenter C 绑定投递(wire 打包在 C 侧 dc_* 完成),
-- 再由 srey 协程层挂起等响应。set/del 返 boolean;get/wait 返 (rdata, rsize) 裸指针;keys 返字符串数组。
local srey       = require("lib.srey")
local datacenter = require("srey.datacenter")
local MSG_TYPE   = srey.MSG_TYPE
local set = datacenter.set
local get = datacenter.get
local wait = datacenter.wait
local del = datacenter.del
local keys = datacenter.keys
local parse_keys = datacenter.parse_keys

local dc_client = {}

-- 投递成功(post_ok)后挂起等响应;返回 msg(含空值命中)或 nil(未投递/超时/erro 非 OK)
local function _wait_resp(post_ok, sess, op)
    if not post_ok then
        WARN("dc %s failed: datacenter unreachable or invalid args.", op)
        return nil
    end
    local msg = srey._coro_wait(true, sess, MSG_TYPE.RESPONSE, srey.get_request_timeout())
    if MSG_TYPE.TIMEOUT == msg.mtype then
        WARN("dc %s timeout, session %s.", op, tostring(sess))
        return nil
    end
    if ERR_OK ~= msg.erro then
        return nil
    end
    return msg
end

---写入或覆盖 KV;唤醒所有该 key 的 waiter。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name(与 C 层 dc_start 注册一致)
---@param key string key 字符串(非空,< DC_KEY_MAX;非法由 C 侧拒绝)
---@param val string? value 数据;nil 或空串等价软清空
---@return boolean ok 成功 true;key 非法或超时/不可达 false
function dc_client.set(dc_name, key, val)
    local sess = srey.id()
    return nil ~= _wait_resp(set(dc_name, sess, key, val), sess, "set")
end

---读 KV;key 不存在返回 nil。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name
---@param key string key 字符串(非空)
---@return lightuserdata|nil rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝；失败/超时返回 nil
---@return integer? rsize 响应数据长度
function dc_client.get(dc_name, key)
    local sess = srey.id()
    local msg = _wait_resp(get(dc_name, sess, key), sess, "get")
    if not msg then
        return nil
    end
    return msg.data, msg.size
end

---读 KV;key 不存在则挂起协程直到 set 触发,或 request_timeout 超时。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name
---@param key string key 字符串(非空)
---@return lightuserdata|nil rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝；失败/超时返回 nil
---@return integer? rsize 响应数据长度
function dc_client.wait(dc_name, key)
    local sess = srey.id()
    local msg = _wait_resp(wait(dc_name, sess, key), sess, "wait")
    if not msg then
        return nil
    end
    return msg.data, msg.size
end

---删除指定 key 的 KV 条目;只清 KV,不影响 pending(已等的 waiter 继续等)。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name
---@param key string key 字符串(非空)
---@return boolean ok 成功 true(key 不存在也返 true);key 非法或超时 false
function dc_client.del(dc_name, key)
    local sess = srey.id()
    return nil ~= _wait_resp(del(dc_name, sess, key), sess, "del")
end

---列出全部 key。调试用,生产 key 量大时谨慎调。必须在协程中调用。
---@param dc_name TASK_NAME DataCenter task name
---@return string[]? keys key 字符串数组;空 DC 返回空表;超时返回 nil
function dc_client.keys(dc_name)
    local sess = srey.id()
    local msg = _wait_resp(keys(dc_name, sess), sess, "keys")
    if not msg then
        return nil
    end
    if not msg.data or 0 == msg.size then
        return {}
    end
    return parse_keys(msg.data, msg.size)
end

return dc_client
