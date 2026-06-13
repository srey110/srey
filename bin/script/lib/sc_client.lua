-- subcenter Lua 协程客户端:11 个协程版 API 走 srey.request → subcenter task service。
-- payload 与 lib/services/subcenter.c 协议一致(reqtype 统一 REQUEST_TYPE.REQ_SC,首字节 u8 op 标识子命令):
--   SUB            = | u8 0x01 | u16 tlen | topic |
--   SUB_SHARED     = | u8 0x02 | u16 tlen | topic | u16 glen | group |
--   UNSUB          = | u8 0x03 | u16 tlen | topic |
--   UNSUB_SHARED   = | u8 0x04 | u16 tlen | topic | u16 glen | group |
--   PUB            = | u8 0x05 | u16 tlen | topic | u32 plen | payload |
--   PUB_RETAINED   = | u8 0x06 | u16 tlen | topic | u32 plen | payload |
--   LIST           = | u8 0x07 |
--   QUERY_RETAINED = | u8 0x08 | u16 plen | pattern |
--   SET_META       = | u8 0x09 | u16 mlen | meta |
--   RETAINED_LIST  = | u8 0x0A |
-- subcenter → 订阅者推送(REQUEST_TYPE.REQ_SC_DELIVER):
--   | u8 kind | name_t publisher | u16 mlen | meta | u16 glen | group | u16 tlen | topic | u32 plen | payload |   (kind: 0=普通 1=共享;group 仅共享非空)
-- subscribe 内部自动两步:SUB → QUERY_RETAINED 把首批 retained 转交 handler。
-- 性能说明:_handlers 表线性扫描,适合 < 100 pattern;> 100 考虑分组路由。

local srey = require("lib.srey")
-- 子命令操作码,与 subcenter.c 内部 sc_op 枚举对齐
local OP_SUB             = "\x01"
local OP_SUB_SHARED      = "\x02"
local OP_UNSUB           = "\x03"
local OP_UNSUB_SHARED    = "\x04"
local OP_PUB             = "\x05"
local OP_PUB_RETAINED    = "\x06"
local OP_LIST            = "\x07"
local OP_QUERY_RETAINED  = "\x08"
local OP_SET_META        = "\x09"
local OP_RETAINED_LIST   = "\x0A"

-- per-task handler 路由表:pattern → handler。task 内单线程协作,无锁
-- 普通订阅(_handlers:pattern→handler)与共享订阅(_shared_handlers:pattern→group→handler,多 group 同 topic 各自独立)各一张表,按 deliver kind 分别路由
---@type table<string, fun(topic:string, payload:string, publisher:integer, meta:string?)>
local _handlers = {}
---@type table<string, table<string, fun(topic:string, payload:string, publisher:integer, meta:string?)>>
local _shared_handlers = {}

local sc_client = {}

-- 段切分:把 path 按 '/' 拆成数组(保留空段,供 _topic_match 严格段数比较)
---@param s string
---@return string[]
local function _split_segs(s)
    local segs = {}
    local start = 1
    while true do
        local i = string.find(s, "/", start, true)
        if not i then
            segs[#segs + 1] = string.sub(s, start)
            break
        end
        segs[#segs + 1] = string.sub(s, start, i - 1)
        start = i + 1
    end
    return segs
end

-- 通配匹配:精确 topic 是否匹配含通配的 pattern(对齐 C 层 path_matches_pattern)
-- 规则:'+' 匹配单层(段非空);'#' 匹配剩余任意层(必须末尾);其他段精确比较
---@param pattern string
---@param topic string
---@return boolean
local function _topic_match(pattern, topic)
    local pats = _split_segs(pattern)
    local lits = _split_segs(topic)
    local pn = #pats
    local ln = #lits
    local pi = 1
    local li = 1
    while pi <= pn do
        local p = pats[pi]
        if "#" == p then
            return pi == pn   -- # 必须末尾
        end
        if li > ln then
            return false      -- literal 段用完但 pattern 还有
        end
        if "+" == p then
            if "" == lits[li] then
                return false  -- + 必须有非空段
            end
        elseif p ~= lits[li] then
            return false
        end
        pi = pi + 1
        li = li + 1
    end
    return li == ln + 1
end

-- 拆 deliver wire:| u8 kind | name_t publisher | u16 mlen | meta | u16 glen | group | u16 tlen | topic | u32 plen | payload |
-- name_t = uint64_t(见 lib/base/structs.h);所有数值字段网络序;group 仅共享投递非空
---@param raw string
---@return integer kind 0=普通投递 1=共享投递
---@return integer publisher
---@return string topic
---@return string payload
---@return string? meta meta 字节(mlen=0 时返 nil)
---@return string group 共享投递的组名(普通投递为 "")
local function _unpack_deliver(raw)
    local kind, pos = string.unpack(">B", raw)
    local publisher, pos1 = string.unpack(">I8", raw, pos)
    local meta, pos2 = string.unpack(">s2", raw, pos1)
    local group, pos3 = string.unpack(">s2", raw, pos2)
    local topic, pos4 = string.unpack(">s2", raw, pos3)
    local payload = string.unpack(">s4", raw, pos4)
    if "" == meta then
        meta = nil
    end
    return kind, publisher, topic, payload, meta, group
end

-- 拆 query_retained 响应 wire:多条 retained 拼接
-- 每条:| name_t retained_publisher | u16 mlen | meta | u16 tlen | topic | u32 plen | payload |
---@param raw string
---@return {publisher:integer, topic:string, payload:string, meta:string?}[] list
local function _unpack_retained_list(raw)
    local list = {}
    local total = #raw
    local pos = 1
    while pos <= total do
        local publisher, p1 = string.unpack(">I8", raw, pos)
        local meta, p2 = string.unpack(">s2", raw, p1)
        local topic, p3 = string.unpack(">s2", raw, p2)
        local payload, p4 = string.unpack(">s4", raw, p3)
        if "" == meta then
            meta = nil
        end
        list[#list + 1] = {
            publisher = publisher,
            topic = topic,
            payload = payload,
            meta = meta,
        }
        pos = p4
    end
    return list
end

-- 拆 topics 响应:| u16 tlen | topic | u32 normal_count | u32 shared_groups_count |
---@param raw string
---@return {topic:string, normal:integer, shared:integer}[] list
local function _unpack_topics(raw)
    local list = {}
    local total = #raw
    local pos = 1
    while pos <= total do
        local topic, p1 = string.unpack(">s2", raw, pos)
        local normal, p2 = string.unpack(">I4", raw, p1)
        local shared, p3 = string.unpack(">I4", raw, p2)
        list[#list + 1] = { topic = topic, normal = normal, shared = shared }
        pos = p3
    end
    return list
end

-- 拆 retained_topics 响应:| u16 tlen | topic | name_t publisher | u32 size | u16 meta_size |
---@param raw string
---@return {topic:string, publisher:integer, size:integer, meta_size:integer}[] list
local function _unpack_retained_topics(raw)
    local list = {}
    local total = #raw
    local pos = 1
    while pos <= total do
        local topic, p1 = string.unpack(">s2", raw, pos)
        local publisher, p2 = string.unpack(">I8", raw, p1)
        local size, p3 = string.unpack(">I4", raw, p2)
        local meta_size, p4 = string.unpack(">I2", raw, p3)
        list[#list + 1] = {
            topic = topic,
            publisher = publisher,
            size = size,
            meta_size = meta_size,
        }
        pos = p4
    end
    return list
end

-- ── 内部:收到 REQ_SC_DELIVER 时由 srey.lua dispatch 调用 ──────────────────
-- 按 deliver 的 kind 选普通(_handlers)或共享(_shared_handlers)表,对所有匹配 topic 的 pattern srey.xpcall 调 handler。
-- 同一订阅者订多个匹配 pattern 时 handler 会被调用多次(业务自行去重),与 C 层语义一致。
---@param data lightuserdata
---@param size integer
function sc_client._on_deliver(data, size)
    if not data or size <= 0 then
        return
    end
    local raw = srey.ud_str(data, size)
    local kind, publisher, topic, payload, meta, group = _unpack_deliver(raw)
    -- snapshot 匹配 handler 列表,避免 handler 内 subscribe/unsubscribe 改表触发迭代 UB
    local matched = {}
    if 1 == kind then
        -- 共享:按 (匹配 pattern, 投递 group) 精确取 handler,多 group 同 topic 互不串
        for pattern, groups in pairs(_shared_handlers) do
            if _topic_match(pattern, topic) then
                local handler = groups[group]
                if handler then
                    matched[#matched + 1] = handler
                end
            end
        end
    else
        for pattern, handler in pairs(_handlers) do
            if _topic_match(pattern, topic) then
                matched[#matched + 1] = handler
            end
        end
    end
    for i = 1, #matched do
        srey.xpcall(matched[i], topic, payload, publisher, meta)
    end
end

-- ── 公开 API:协程版 ─────────────────────────────────────────────────────

---订阅 topic(可含通配 + / #)并注册 handler。内部自动两步:发 SUB 命令 + 立即 query_retained
---把当前匹配的 retained 转交本 handler 调用一次(对齐 MQTT subscribe-get-retained 语义)。
---重复订阅相同 pattern 会覆盖旧 handler。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name(C 层 sc_start 注册一致)
---@param topic string 订阅模式;可含 + / # 通配
---@param handler fun(topic:string, payload:string, publisher:integer, meta:string?) 收消息时调用,topic 是匹配到的精确 topic;publisher 为 0(INVALID_TNAME)时表示 publisher 已失效
---@return boolean ok 成功 true;topic 非法或 subcenter 不可达 false
function sc_client.subscribe(sc_name, topic, handler)
    if not topic or "" == topic or not handler then
        WARN("sc subscribe: topic/handler empty.")
        return false
    end
    local old = _handlers[topic]
    _handlers[topic] = handler
    local sub_payload = OP_SUB .. string.pack(">s2", topic)
    local _, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, sub_payload)
    if nil == size then
        -- 仅当当前值仍是本次 handler(未被并发/重订覆盖)时才回滚,避免抹掉他人写入
        if handler == _handlers[topic] then
            _handlers[topic] = old
        end
        return false
    end
    -- 自动 query_retained 转交首批匹配 retained 给 handler
    local q_payload = OP_QUERY_RETAINED .. string.pack(">s2", topic)
    local rdata, rsize = srey.request(sc_name, REQUEST_TYPE.REQ_SC, q_payload)
    if nil == rsize then
        return true   -- 订阅已成功,query 失败不影响
    end
    if nil == rdata or 0 == rsize then
        return true   -- 无匹配 retained
    end
    local raw = srey.ud_str(rdata, rsize)
    local list = _unpack_retained_list(raw)
    for i = 1, #list do
        local r = list[i]
        srey.xpcall(handler, r.topic, r.payload, r.publisher, r.meta)
    end
    return true
end

---共享订阅:同 group 内多个订阅者轮询接收 publish。不收 retained。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param topic string 订阅模式
---@param group string 共享组名(非空)
---@param handler fun(topic:string, payload:string, publisher:integer, meta:string?)
---@return boolean ok 成功 true;参数非法或 subcenter 不可达 false
function sc_client.subscribe_shared(sc_name, topic, group, handler)
    if not topic or "" == topic or not group or "" == group or not handler then
        WARN("sc subscribe_shared: param empty.")
        return false
    end
    local groups = _shared_handlers[topic]
    if not groups then
        groups = {}
        _shared_handlers[topic] = groups
    end
    local old = groups[group]
    groups[group] = handler
    local payload = OP_SUB_SHARED .. string.pack(">s2", topic) .. string.pack(">s2", group)
    local _, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, payload)
    if nil == size then
        if handler == groups[group] then
            groups[group] = old
            if nil == next(groups) then
                _shared_handlers[topic] = nil
            end
        end
        return false
    end
    return true
end

---取消订阅。未订阅过的 topic 幂等返 OK。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param topic string 订阅模式;须与 subscribe 时完全一致
---@return boolean ok 成功 true;topic 非法或 subcenter 不可达 false
function sc_client.unsubscribe(sc_name, topic)
    if not topic or "" == topic then
        WARN("sc unsubscribe: topic empty.")
        return false
    end
    local old = _handlers[topic]
    _handlers[topic] = nil
    local payload = OP_UNSUB .. string.pack(">s2", topic)
    local _, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, payload)
    if nil == size then
        if nil == _handlers[topic] then
            _handlers[topic] = old
        end
        return false
    end
    return true
end

---取消共享订阅。未订阅过的 topic+group 幂等返 OK。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param topic string
---@param group string
---@return boolean ok 成功 true;参数非法或 subcenter 不可达 false
function sc_client.unsubscribe_shared(sc_name, topic, group)
    if not topic or "" == topic or not group or "" == group then
        WARN("sc unsubscribe_shared: param empty.")
        return false
    end
    local groups = _shared_handlers[topic]
    local old = groups and groups[group]
    if groups then
        groups[group] = nil
        if nil == next(groups) then
            _shared_handlers[topic] = nil
        end
    end
    local payload = OP_UNSUB_SHARED .. string.pack(">s2", topic) .. string.pack(">s2", group)
    local _, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, payload)
    if nil == size then
        if nil ~= old then
            local g = _shared_handlers[topic]
            if not g then
                g = {}
                _shared_handlers[topic] = g
            end
            if nil == g[group] then
                g[group] = old
            end
        end
        return false
    end
    return true
end

---发布消息到精确 topic。fire-and-forget。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param topic string 精确 topic;不允许含通配
---@param data string? payload;nil 等价空 payload
---@return boolean ok 成功 true;topic 非法或 subcenter 不可达 false
function sc_client.publish(sc_name, topic, data)
    if not topic or "" == topic then
        WARN("sc publish: topic empty.")
        return false
    end
    local payload = OP_PUB .. string.pack(">s2", topic) .. string.pack(">s4", data or "")
    local _, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, payload)
    if nil == size then
        return false
    end
    return true
end

---发布保留消息。data 为 nil/空串等价"清空 retained 槽位,不 deliver"。
---retained 大小受 SC_RETAINED_MAX_SIZE(1MB)上限,超过拒绝。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param topic string 精确 topic
---@param data string? retained payload(nil 清空槽位)
---@return boolean ok 成功 true;topic 非法或 subcenter 不可达 false
function sc_client.publish_retained(sc_name, topic, data)
    if not topic or "" == topic then
        WARN("sc publish_retained: topic empty.")
        return false
    end
    local payload = OP_PUB_RETAINED .. string.pack(">s2", topic) .. string.pack(">s4", data or "")
    local _, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, payload)
    if nil == size then
        return false
    end
    return true
end

---查询匹配 pattern 的所有当前 retained 消息。
---单次返回上限 SC_QUERY_RETAINED_BURST_MAX(1000),超过截断 + WARN(C 层日志)。
---subscribe 内部已调用此函数转交首批 retained 给 handler;业务通常无需直接调。
---必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param pattern string 查询模式;可含 + / # 通配
---@return {publisher:integer, topic:string, payload:string, meta:string?}[]? list 匹配 retained 列表;nil 表示 subcenter 不可达
function sc_client.query_retained(sc_name, pattern)
    if not pattern or "" == pattern then
        WARN("sc query_retained: pattern empty.")
        return nil
    end
    local payload = OP_QUERY_RETAINED .. string.pack(">s2", pattern)
    local data, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, payload)
    if nil == size then
        return nil
    end
    if nil == data or 0 == size then
        return {}
    end
    return _unpack_retained_list(srey.ud_str(data, size))
end

---列出所有订阅 topic(仅订阅信息,不含 retained)。调试用,topic 量大时谨慎调。
---必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@return {topic:string, normal:integer, shared:integer}[]? list 空时返空表;subcenter 不可达返 nil
function sc_client.topics(sc_name)
    local data, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, OP_LIST)
    if nil == size then
        return nil
    end
    if nil == data or 0 == size then
        return {}
    end
    return _unpack_topics(srey.ud_str(data, size))
end

---列出所有 retained topic 元信息(不返 retained payload,避免数据量大)。调试用。
---必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@return {topic:string, publisher:integer, size:integer, meta_size:integer}[]? list 空时返空表;subcenter 不可达返 nil
function sc_client.retained_topics(sc_name)
    local data, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, OP_RETAINED_LIST)
    if nil == size then
        return nil
    end
    if nil == data or 0 == size then
        return {}
    end
    return _unpack_retained_topics(srey.ud_str(data, size))
end

---注册或更新当前 task 的发布者元数据。
---后续该 task 所有 publish/publish_retained 都自动携带 meta 投递给订阅者。
---publisher 应在 task 退出前调 set_meta(sc_name, nil) 主动清理。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param meta string? 元数据;nil 或空串等价"清除元数据";上限 SC_META_MAX_SIZE(1KB)
---@return boolean ok 成功 true;subcenter 不可达 false
function sc_client.set_meta(sc_name, meta)
    local payload = OP_SET_META .. string.pack(">s2", meta or "")
    local _, size = srey.request(sc_name, REQUEST_TYPE.REQ_SC, payload)
    if nil == size then
        return false
    end
    return true
end

return sc_client
