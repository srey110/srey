-- Redis 客户端工具库。
-- 提供：连接（含 AUTH）、RESP 协议序列化（pack）、
-- 以及完整的 RESP3 多节点响应解包（unpack）。
-- unpack 支持 array/set/map/attr 等聚合类型的嵌套递归解析，
-- 使用显式栈（mark）替代递归，避免深层嵌套时栈溢出。

local srey       = require("lib.srey")
local srey_redis = require("srey.redis")
local table   = table
local math    = math
local redis   = {}

---连接 Redis 服务器并可选地执行 AUTH 认证
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param psw string? 密码；为 nil 或空时跳过 AUTH
---@param netev NET_EV? 事件订阅掩码
---@return integer fd socket fd；失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；失败时为 nil，返回值个数恒为 2
function redis.connect(ip, port, sslname, psw, netev)
    local fd, skid = srey.connect(PACK_TYPE.REDIS, sslname, ip, port, netev)
    if INVALID_SOCK == fd then
        return INVALID_SOCK, nil
    end
    if str_nullorempty(psw) then
        return fd, skid
    end
    -- 发送 AUTH 命令验证密码
    local auth = redis.pack("AUTH", psw)
    local rtn, _ = srey.syn_send(fd, skid, auth, #auth, 1)
    local result = rtn and redis.unpack(rtn)
    if "OK" ~= result then
        srey.close(fd, skid)
        return INVALID_SOCK, nil
    end
    return fd, skid
end

-- 单个参数转上线的字节。浮点走 num_str，其余原样 tostring
local function _arg_str(v)
    if "float" == math.type(v) then
        return num_str(v)
    end
    return tostring(v)
end
---将命令及参数序列化为 RESP 协议字符串（inline array）；所有参数先转成字符串，见 _arg_str
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
        if nil ~= args[i] then
            local value = _arg_str(args[i])
            req[idx] = '$'
            req[idx + 1] = #value
            req[idx + 2] = '\r\n'
            req[idx + 3] = value
            req[idx + 4] = '\r\n'
            idx = idx + 5
        else
            WARN("redis.pack: nil argument #%d, encoded as empty bulk string", i)
            req[idx] = "$0\r\n\r\n"
            idx = idx + 1
        end
    end
    return table.concat(req)
end

---读取当前响应节点的值
---@type fun(pk:lightuserdata?):string|integer|number|boolean|nil|RedisAggValue
redis.value = srey_redis.value

---获取响应链表中下一个节点指针
---@type fun(pk:lightuserdata?):lightuserdata|nil
redis.next = srey_redis.next

-- unpack 的解析循环每个 RESP 节点都要调这两个，提成 local 免得逐节点重查模块表
local rvalue, rnext = redis.value, redis.next

-- ── 节点读取与聚合类型判断 ────────────────────────────────────────────────
-- 所有判断都基于 _node 摘出来的 kind，不读容器表里的字段：resp_type / resp_nelem 是 C 层的哨兵，
-- 而 Redis 字段名是任意二进制串，HGETALL 拿到同名字段就会把哨兵盖掉。故一读到就摘走

---读一个节点：标量原样回带；聚合把 resp_type / resp_nelem 摘出来单独回带，表本身只留载荷。
---非聚合节点 C 侧恒推标量或 nil（lprot.c 的 default），故 table 即聚合
---@param pk lightuserdata redis_pack_ctx 节点指针
---@return any val 载荷；聚合为已摘净哨兵的容器表
---@return string? kind 聚合类型名；非聚合为 nil
---@return integer? nelem 元素计数；非聚合为 nil
local function _node(pk)
    local val = rvalue(pk)
    if "table" ~= type(val) then
        return val, nil, nil
    end
    local kind, nelem = val.resp_type, val.resp_nelem
    val.resp_type = nil
    val.resp_nelem = nil
    return val, kind, nelem
end

-- 能不能当表键。nil 与 NaN 都不行（t[NaN] 抛错），而 RESP3 的 ,nan 解出来就是 NaN
local function _key_ok(key)
    return nil ~= key and key == key
end
---是否为 map 或 attr（键值对聚合）
---@param kind string? _node 回带的聚合类型名
---@return boolean ok
local function _is_map(kind)
    return "map" == kind or "attr" == kind
end

---是否为 attr（属性前置聚合，RESP3 特有）
---@param kind string? _node 回带的聚合类型名
---@return boolean ok
local function _is_attr(kind)
    return "attr" == kind
end

---是否为任意聚合类型（array / set / push / map / attr）
---@param kind string? _node 回带的聚合类型名
---@return boolean ok
local function _is_agg(kind)
    return "array" == kind or "set" == kind or "push" == kind or
           "map" == kind or "attr" == kind
end

-- ── 解析栈管理 ────────────────────────────────────────────────────────────

---@class RedisParseMark
---@field status 0|1           0=期望 key，1=期望 val（仅 map/attr 使用）
---@field nelem  integer       剩余待处理元素个数（map/attr 已 ×2）
---@field agg    RedisAggPayload 所属聚合节点（哨兵已摘，只装载荷）
---@field ismap  boolean       压栈时按 kind 算好的"是否键值对聚合"，替代读表判定
---@field isattr boolean       压栈时按 kind 算好的"是否 attr"
---@field key    any?          map/attr 解析到 key 时暂存，读到 val 时配对写进 agg

---更新栈顶计数器；计数归零时弹出并归还对象池；attr 完成后立即 break，
---因为 attr 之后跟随被修饰的真实数据，需由上层继续处理，不能连续弹出
---@param mark RedisParseMark[] 解析栈
local function _update_mark(mark)
    local mk
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
        if mk.isattr then
            break
        end
    end
end

---聚合节点按 nelem 三态取值:>0 取 val,==0 取空表,<0(RESP3 的 nil 聚合)取 neg。
---neg 由调用点显式给:父为 map 暂存 key 时取 nil(让 _key_ok 拦掉整对),其余三处取 false。
---@param val any 聚合值
---@param nelem integer 元素计数
---@param neg any nelem<0 时的取值
---@return any
local function _aggval(val, nelem, neg)
    if nelem > 0 then
        return val
    end
    if 0 == nelem then
        return {}
    end
    return neg
end

---将聚合节点压入解析栈；map/attr 的元素个数需乘以 2（每元素占 key+val 两节点）
---@param mark RedisParseMark[] 解析栈
---@param val RedisAggPayload 聚合容器表（哨兵已摘）
---@param kind string 聚合类型名
---@param nelem integer 元素计数
local function _add_mark(mark, val, kind, nelem)
    local ismap = _is_map(kind)
    mark[#mark + 1] = {
        status = 0,    -- 0=期望 key，1=期望 val（仅 map/attr 使用）
        nelem  = ismap and nelem * 2 or nelem,
        agg    = val,
        ismap  = ismap,
        isattr = _is_attr(kind),
    }
end

-- ── 单/首节点处理 ─────────────────────────────────────────────────────────

---处理单一节点：标量直接返回；聚合 nelem=0 返回 {}，nelem=-1 返回 nil
---@param pk lightuserdata redis_pack_ctx 节点指针
---@return any value 解包后的值
local function _single_node(pk)
    local val, kind, nelem = _node(pk)
    if _is_agg(kind) then
        if -1 == nelem then
            return nil
        elseif 0 == nelem then
            return {}
        else
            WARN("resp message error.")
            return nil
        end
    end
    return val
end

---处理多节点响应的第一个节点（必须为聚合类型）；attr 类型用 {val} 包装以区分属性与数据节点
---@param mark RedisParseMark[] 解析栈
---@param pk lightuserdata redis_pack_ctx 首节点指针
---@return RedisAggPayload|nil rtn 容器表；首节点非聚合或为 nil 聚合返回 nil
local function _first_nodes(mark, pk)
    local val, kind, nelem = _node(pk)
    if not _is_agg(kind) then
        WARN("resp message error.")
        return nil
    end
    if nelem > 0  then
        _add_mark(mark, val, kind, nelem)
        if _is_attr(kind) then
            return {val}
        else
            return val
        end
    elseif 0 == nelem then
        if _is_attr(kind) then
            return {{}}
        end
        return {}
    else
        if _is_attr(kind) then
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
    if not rnext(pk) then
        return _single_node(pk)
    end
    -- 多节点：第一个节点必须是 aggregate data
    local mark = {}
    local rtn = _first_nodes(mark, pk)
    if not rtn then
        return nil
    end
    local val, kind, nelem, parent
    pk = rnext(pk)
    while pk do
        parent = mark[#mark]
        val, kind, nelem = _node(pk)
        if not parent then
            -- 无父节点（顶层多值响应，如 pipeline）
            if _is_agg(kind) then
                table.insert(rtn, _aggval(val, nelem, false))
                if nelem > 0 then
                    _add_mark(mark, val, kind, nelem)
                end
            else
                table.insert(rtn, val ~= nil and val or false)
            end
        else
            -- 有父节点：根据父节点类型（map/attr vs 其他）及当前 key/val 状态填充
            if _is_agg(kind) then
                if parent.ismap and not _is_attr(kind) then
                    -- 父为 map/attr，当前为非 attr 聚合节点
                    if 0 == parent.status then
                        -- 作为 key 暂存
                        parent.status = 1
                        parent.key = _aggval(val, nelem, nil)
                    else
                        -- 作为 val 写入父 map
                        parent.status = 0
                        if _key_ok(parent.key) then
                            parent.agg[parent.key] = _aggval(val, nelem, false)
                        end
                    end
                else
                    -- 父为 array/set/push 或当前节点为 attr：顺序追加
                    table.insert(parent.agg, _aggval(val, nelem, false))
                end
                if nelem > 0 then
                    _add_mark(mark, val, kind, nelem)
                else
                    if not _is_attr(kind) then
                        _update_mark(mark)
                    end
                end
            else
                -- 当前为标量节点
                if parent.ismap then
                    -- 父为 map/attr：交替填充 key/val
                    if 0 == parent.status then
                        parent.status = 1
                        parent.key = val
                    else
                        parent.status = 0
                        if _key_ok(parent.key) then
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
        pk = rnext(pk)
    end
    return rtn
end

return redis
