-- Redis 客户端工具库。
-- 提供：连接（含 AUTH）、RESP 协议序列化（pack）、
-- 以及完整的 RESP3 多节点响应解包（unpack）。
-- unpack 支持 array/set/map/attr 等聚合类型的嵌套递归解析，
-- 使用显式栈（mark）替代递归，避免深层嵌套时栈溢出。

local srey       = require("lib.srey")
local srey_redis = require("srey.redis")
local table   = table
local redis   = {}

---连接 Redis 服务器并可选地执行 AUTH 认证
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param psw string? 密码；为 nil 或空时跳过 AUTH
---@param netev NET_EV? 事件订阅掩码
---@return integer fd socket fd；失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；仅在 fd 有效时返回
function redis.connect(ip, port, sslname, psw, netev)
    local fd, skid = srey.connect(PACK_TYPE.REDIS, sslname, ip, port, netev)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    if str_nullorempty(psw) then
        return fd, skid
    end
    -- 发送 AUTH 命令验证密码
    local auth = redis.pack("AUTH", psw)
    local rtn, _ = srey.syn_send(fd, skid, auth, #auth, 1)
    local result = rtn and redis.unpack(rtn) or nil
    if "OK" ~= result then
        srey.close(fd, skid)
        return INVALID_SOCK
    end
    return fd, skid
end

---将命令及参数序列化为 RESP 协议字符串（inline array）；所有参数先 tostring 转换
---@param ... any 命令名及参数，如 redis.pack("SET", "key", "value")
---@return string req RESP 编码后的请求字符串
function redis.pack(...)
    -- select('#', ...) 计数保留以支持含 nil 的精确长度；{...} 一次性收集为表后
    -- 循环按下标 O(1) 访问，避免逐参 select(i, ...) 重复扫描 vararg 起点导致 O(n²)
    local n = select('#', ...)
    local args = {...}
    local req = { '*', n, '\r\n' }
    local idx = 4
    for i = 1, n do
        local value = tostring(args[i])
        req[idx] = '$'
        req[idx + 1] = #value
        req[idx + 2] = '\r\n'
        req[idx + 3] = value
        req[idx + 4] = '\r\n'
        idx = idx + 5
    end
    return table.concat(req)
end

---读取当前响应节点的值
---@type fun(pk:lightuserdata?):string|integer|number|boolean|nil|RedisAggValue
redis.value = srey_redis.value

---获取响应链表中下一个节点指针
---@type fun(pk:lightuserdata):lightuserdata|nil
redis.next = srey_redis.next

-- ── 聚合类型辅助判断 ──────────────────────────────────────────────────────

---是否为 map 或 attr（键值对聚合）
---@param val any 节点值
---@return boolean ok
local function _is_map(val)
    if "table" ~= type(val) then
        return false
    end
    return "map" == val.resp_type or "attr" == val.resp_type
end

---是否为 attr（属性前置聚合，RESP3 特有）
---@param val any 节点值
---@return boolean ok
local function _is_attr(val)
    if "table" ~= type(val) then
        return false
    end
    return "attr" == val.resp_type
end

---是否为任意聚合类型（array / set / push / map / attr）
---@param val any 节点值
---@return boolean ok
local function _is_agg(val)
    if "table" ~= type(val) then
        return false
    end
    return "array" == val.resp_type or "set" == val.resp_type or "push" == val.resp_type or
           "map" == val.resp_type or "attr" == val.resp_type
end

-- ── 解析栈管理 ────────────────────────────────────────────────────────────

---更新栈顶计数器；计数归零时弹出并归还对象池；attr 完成后立即 break，
---因为 attr 之后跟随被修饰的真实数据，需由上层继续处理，不能连续弹出
---@param mark table[] 解析栈
local function _update_mark(mark)
    local mk
    local attr
    while true do
        mk = mark[#mark]
        if not mk then
            break
        end
        mk.nelem = mk.nelem - 1
        if mk.nelem > 0 then
            break
        end
        mark[#mark] = nil          -- 弹出栈顶
        attr = _is_attr(mk.agg)
        mk.agg.resp_nelem = nil
        mk.agg.resp_type  = nil
        if attr then
            break
        end
    end
end

---将聚合节点压入解析栈；map/attr 的元素个数需乘以 2（每元素占 key+val 两节点）
---@param mark table[] 解析栈
---@param val RedisAggValue 聚合节点
local function _add_mark(mark, val)
    local nelem = _is_map(val) and val.resp_nelem * 2 or val.resp_nelem
    mark[#mark + 1] = {
        status = 0,    -- 0=期望 key，1=期望 val（仅 map/attr 使用）
        nelem  = nelem,
        agg    = val,
    }
end

-- ── 单/首节点处理 ─────────────────────────────────────────────────────────

---处理单一节点：标量直接返回；聚合 nelem=0 返回 {}，nelem=-1 返回 nil
---@param pk lightuserdata redis_pack_ctx 节点指针
---@return any value 解包后的值
local function _single_node(pk)
    local val = redis.value(pk)
    if _is_agg(val) then
        if -1 == val.resp_nelem then
            return nil
        elseif 0 == val.resp_nelem then
            return {}
        else
            WARN("resp message error.")
           return nil
        end
    end
    return val
end

---处理多节点响应的第一个节点（必须为聚合类型）；attr 类型用 {val} 包装以区分属性与数据节点
---@param mark table[] 解析栈
---@param pk lightuserdata redis_pack_ctx 首节点指针
---@return RedisAggValue|nil rtn 容器表；首节点非聚合或为 nil 聚合返回 nil
local function _first_nodes(mark, pk)
    local val = redis.value(pk)
    if not _is_agg(val) then
        WARN("resp message error.")
        return nil
    end
    if val.resp_nelem > 0  then
        _add_mark(mark, val)
        if _is_attr(val) then
            return {val}
        else
            return val
        end
    elseif 0 == val.resp_nelem then
        if _is_attr(val) then
            return {{}}
        end
        return {}
    else
        if _is_attr(val) then
            return {}
        end
        return nil
    end
end

-- ── 主解包函数 ────────────────────────────────────────────────────────────

---将 C 层 RESP3 响应链表解包为 Lua 值；单节点直接返回，多节点用显式栈组装嵌套聚合（array/set/push/map/attr）
---@param pk lightuserdata redis_pack_ctx 首节点指针
---@return any value 解包后的 Lua 值
function redis.unpack(pk)
    -- 单一节点：无嵌套，直接返回
    if not redis.next(pk) then
        return _single_node(pk)
    end
    -- 多节点：第一个节点必须是 aggregate data
    local mark = {}
    local rtn = _first_nodes(mark, pk)
    if not rtn then
        return nil
    end
    local val, parent
    pk = redis.next(pk)
    while pk do
        parent = mark[#mark]
        val = redis.value(pk)
        if not parent then
            -- 无父节点（顶层多值响应，如 pipeline）
            if _is_agg(val) then
                if val.resp_nelem > 0 then
                    table.insert(rtn, val)
                    _add_mark(mark, val)
                elseif 0 == val.resp_nelem then
                    table.insert(rtn, {})
                else
                    table.insert(rtn, false)
                end
            else
                table.insert(rtn, val ~= nil and val or false)
            end
        else
            -- 有父节点：根据父节点类型（map/attr vs 其他）及当前 key/val 状态填充
            if _is_agg(val) then
                if _is_map(parent.agg) and not _is_attr(val) then
                    -- 父为 map/attr，当前为非 attr 聚合节点
                    if 0 == parent.status then
                        -- 作为 key 暂存
                        parent.status = 1
                        if val.resp_nelem > 0 then
                            parent.key = val
                        elseif 0 == val.resp_nelem then
                            parent.key = {}
                        else
                            parent.key = nil
                        end
                    else
                        -- 作为 val 写入父 map
                        parent.status = 0
                        if nil ~= parent.key then
                            if val.resp_nelem > 0 then
                                parent.agg[parent.key] = val
                            elseif 0 == val.resp_nelem then
                                parent.agg[parent.key] = {}
                            else
                                parent.agg[parent.key] = false
                            end
                        end
                    end
                else
                    -- 父为 array/set/push 或当前节点为 attr：顺序追加
                    if val.resp_nelem > 0 then
                        table.insert(parent.agg, val)
                    elseif 0 == val.resp_nelem then
                        table.insert(parent.agg, {})
                    else
                        table.insert(parent.agg, false)
                    end
                end
                if val.resp_nelem > 0 then
                    _add_mark(mark, val)
                else
                    if not _is_attr(val) then
                        _update_mark(mark)
                    end
                end
            else
                -- 当前为标量节点
                if _is_map(parent.agg) then
                    -- 父为 map/attr：交替填充 key/val
                    if 0 == parent.status then
                        parent.status = 1
                        parent.key = val
                    else
                        parent.status = 0
                        if nil ~= parent.key then
                            parent.agg[parent.key] = val ~= nil and val or false
                        end
                    end
                else
                    -- 父为 array/set/push：顺序追加
                    table.insert(parent.agg, val ~= nil and val or false)
                end
                _update_mark(mark)
            end
        end
        pk = redis.next(pk)
    end
    return rtn
end

return redis
