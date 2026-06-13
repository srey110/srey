-- subcenter Lua 协程客户端:10 个请求 API 经 srey.subcenter C 绑定投递(wire 打包在 C 侧 sc_* 完成),
-- 再由 srey 协程层挂起等响应;投递推送(REQ_SC_DELIVER)经 subcenter.parse_deliver 解析。
-- 响应列表(query_retained/topics/retained_topics)经对应 subcenter.parse_* C 绑定解析;订阅者本地路由(_handlers/_topic_match)仍在 Lua。
-- subscribe 内部自动两步:SUB → query_retained 把首批 retained 转交 handler。
-- 性能说明:_handlers 表线性扫描,适合 < 100 pattern;> 100 考虑分组路由。
local srey       = require("lib.srey")
local subcenter  = require("srey.subcenter")
local MSG_TYPE   = srey.MSG_TYPE
local subscribe = subcenter.subscribe
local subscribe_shared = subcenter.subscribe_shared
local unsubscribe = subcenter.unsubscribe
local unsubscribe_shared = subcenter.unsubscribe_shared
local publish = subcenter.publish
local publish_retained = subcenter.publish_retained
local query_retained = subcenter.query_retained
local topics = subcenter.topics
local retained_topics = subcenter.retained_topics
local set_meta = subcenter.set_meta
local parse_deliver = subcenter.parse_deliver
local parse_retained = subcenter.parse_retained
local parse_topics = subcenter.parse_topics
local parse_retained_topics = subcenter.parse_retained_topics

-- per-task handler 路由表:pattern → handler。task 内单线程协作,无锁
-- 普通订阅(_handlers:pattern→handler)与共享订阅(_shared_handlers:pattern→group→handler,多 group 同 topic 各自独立)各一张表,按 deliver kind 分别路由
---@type table<string, fun(topic:string, payload:string, publisher:integer, meta:string?)>
local _handlers = {}
---@type table<string, table<string, fun(topic:string, payload:string, publisher:integer, meta:string?)>>
local _shared_handlers = {}

local sc_client = {}

-- 投递成功(post_ok)后挂起等响应;返回 msg(含空命中)或 nil(未投递/超时/erro 非 OK)
local function _wait_resp(post_ok, sess, op)
    if not post_ok then
        WARN("sc %s failed: subcenter unreachable or invalid args.", op)
        return nil
    end
    local msg = srey._coro_wait(true, sess, MSG_TYPE.RESPONSE, srey.get_request_timeout())
    if MSG_TYPE.TIMEOUT == msg.mtype then
        WARN("sc %s timeout, session %s.", op, tostring(sess))
        return nil
    end
    if ERR_OK ~= msg.erro then
        return nil
    end
    return msg
end

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

-- ── 内部:收到 REQ_SC_DELIVER 时由 srey.lua dispatch 调用 ──────────────────
-- 投递 wire 由 C 绑定 subcenter.parse_deliver 解析为下表;按 kind 选普通(_handlers)或共享(_shared_handlers)表,
-- 对所有匹配 topic 的 pattern srey.xpcall 调 handler。同一订阅者订多个匹配 pattern 时 handler 被调多次(业务自行去重)。
---@class sc_deliver_msg
---@field kind integer 0=普通投递 1=共享投递
---@field publisher integer 发布者句柄;0(INVALID_TNAME)表示已失效
---@field topic string 匹配到的精确 topic
---@field payload string 载荷(空为 "")
---@field meta string? 发布者元数据(无则 nil)
---@field group string 共享投递组名;普通投递为 ""
---@param data lightuserdata
---@param size integer
function sc_client._on_deliver(data, size)
    if not data or size <= 0 then
        return
    end
    ---@type sc_deliver_msg?
    local d = parse_deliver(data, size)
    if not d then
        return   -- wire 截断/损坏
    end
    local topic = d.topic
    -- snapshot 匹配 handler 列表,避免 handler 内 subscribe/unsubscribe 改表触发迭代 UB
    local matched = {}
    if 1 == d.kind then
        -- 共享:按 (匹配 pattern, 投递 group) 精确取 handler,多 group 同 topic 互不串
        for pattern, groups in pairs(_shared_handlers) do
            if _topic_match(pattern, topic) then
                local handler = groups[d.group]
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
        srey.xpcall(matched[i], topic, d.payload, d.publisher, d.meta)
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
    local sess = srey.id()
    if not _wait_resp(subscribe(sc_name, sess, topic), sess, "subscribe") then
        -- 仅当当前值仍是本次 handler(未被并发/重订覆盖)时才回滚,避免抹掉他人写入
        if handler == _handlers[topic] then
            _handlers[topic] = old
        end
        return false
    end
    -- 自动 query_retained 转交首批匹配 retained 给 handler(query 失败不影响订阅成功)
    local qsess = srey.id()
    local msg = _wait_resp(query_retained(sc_name, qsess, topic), qsess, "query_retained")
    if msg and msg.data and msg.size > 0 then
        local list = parse_retained(msg.data, msg.size)
        for i = 1, #list do
            local r = list[i]
            srey.xpcall(handler, r.topic, r.payload, r.publisher, r.meta)
        end
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
    local sess = srey.id()
    if not _wait_resp(subscribe_shared(sc_name, sess, topic, group), sess, "subscribe_shared") then
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
    local sess = srey.id()
    if not _wait_resp(unsubscribe(sc_name, sess, topic), sess, "unsubscribe") then
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
    local sess = srey.id()
    if not _wait_resp(unsubscribe_shared(sc_name, sess, topic, group), sess, "unsubscribe_shared") then
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
    local sess = srey.id()
    return nil ~= _wait_resp(publish(sc_name, sess, topic, data), sess, "publish")
end

---发布保留消息。data 为 nil/空串等价"清空 retained 槽位,不 deliver"。
---retained 大小受 SC_RETAINED_MAX_SIZE(1MB)上限,超过拒绝。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param topic string 精确 topic
---@param data string? retained payload(nil 清空槽位)
---@return boolean ok 成功 true;topic 非法或 subcenter 不可达 false
function sc_client.publish_retained(sc_name, topic, data)
    local sess = srey.id()
    return nil ~= _wait_resp(publish_retained(sc_name, sess, topic, data), sess, "publish_retained")
end

---查询匹配 pattern 的所有当前 retained 消息。
---单次返回上限 SC_QUERY_RETAINED_BURST_MAX(1000),超过截断 + WARN(C 层日志)。
---subscribe 内部已调用此函数转交首批 retained 给 handler;业务通常无需直接调。
---必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param pattern string 查询模式;可含 + / # 通配
---@return {publisher:integer, topic:string, payload:string, meta:string?}[]? list 匹配 retained 列表;nil 表示 subcenter 不可达
function sc_client.query_retained(sc_name, pattern)
    local sess = srey.id()
    local msg = _wait_resp(query_retained(sc_name, sess, pattern), sess, "query_retained")
    if not msg then
        return nil
    end
    if not msg.data or 0 == msg.size then
        return {}
    end
    return parse_retained(msg.data, msg.size)
end

---列出所有订阅 topic(仅订阅信息,不含 retained)。调试用,topic 量大时谨慎调。
---必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@return {topic:string, normal:integer, shared:integer}[]? list 空时返空表;subcenter 不可达返 nil
function sc_client.topics(sc_name)
    local sess = srey.id()
    local msg = _wait_resp(topics(sc_name, sess), sess, "topics")
    if not msg then
        return nil
    end
    if not msg.data or 0 == msg.size then
        return {}
    end
    return parse_topics(msg.data, msg.size)
end

---列出所有 retained topic 元信息(不返 retained payload,避免数据量大)。调试用。
---必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@return {topic:string, publisher:integer, size:integer, meta_size:integer}[]? list 空时返空表;subcenter 不可达返 nil
function sc_client.retained_topics(sc_name)
    local sess = srey.id()
    local msg = _wait_resp(retained_topics(sc_name, sess), sess, "retained_topics")
    if not msg then
        return nil
    end
    if not msg.data or 0 == msg.size then
        return {}
    end
    return parse_retained_topics(msg.data, msg.size)
end

---注册或更新当前 task 的发布者元数据。
---后续该 task 所有 publish/publish_retained 都自动携带 meta 投递给订阅者。
---publisher 应在 task 退出前调 set_meta(sc_name, nil) 主动清理。必须在协程中调用。
---@param sc_name TASK_NAME subcenter task name
---@param meta string? 元数据;nil 或空串等价"清除元数据";上限 SC_META_MAX_SIZE(1KB)
---@return boolean ok 成功 true;subcenter 不可达 false
function sc_client.set_meta(sc_name, meta)
    local sess = srey.id()
    return nil ~= _wait_resp(set_meta(sc_name, sess, meta), sess, "set_meta")
end

return sc_client
