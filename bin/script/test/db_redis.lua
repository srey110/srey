-- Redis 集成测试：依赖 docker-compose 已起 redis 容器（无密码）

local srey   = require("lib.srey")
local runner = require("test.runner")
local redis  = require("lib.redis")

-- 数据库地址：在 VM 上跑时改成宿主机 IP，只改这一处
local DB_HOST = "127.0.0.1"

-- 同步发送一条命令并解包响应；命令用 pack_ud 编进 C 内存，copy=0 交给 syn_send（走真 redis 一圈）
local function _exec(sk, ...)
    local cmd, size = redis.pack_ud(...)
    local rtn = srey.syn_send(sk, cmd, size, 0)
    if not rtn then
        return nil
    end
    return redis.unpack(rtn)
end

srey.startup(function()
runner.run(function(t)
    local sk = redis.connect(DB_HOST, 6379, SSL_NAME.NONE, nil, 0)
    if not sk.valid then
        t:fail("redis connect")
        return
    end

    -- AUTH 分支：测试库没设口令，带口令连时服务端对 AUTH 回错，connect 关掉连接、返回失效的 sk
    local bad = redis.connect(DB_HOST, 6379, SSL_NAME.NONE, "wrong-password", 0)
    t:check(not bad.valid, "redis 带口令连无口令库返回失效 sk")
    t:eq("PONG", _exec(sk, "PING"), "AUTH 失败的连接不影响主连接")

    -- SET / GET / DEL string
    t:eq("OK",    _exec(sk, "SET", "srey:test", "hello"), "SET")
    t:eq("hello", _exec(sk, "GET", "srey:test"),           "GET")
    t:eq(1,       _exec(sk, "DEL", "srey:test"),           "DEL")

    -- 前置清理：下面两处按"新建"断言返回值，上一轮的残留会让它们失败
    _exec(sk, "DEL", "srey:hash", "srey:counter")

    -- HSET / HGET hash
    t:eq(1,    _exec(sk, "HSET", "srey:hash", "f1", "v1"), "HSET")
    t:eq("v1", _exec(sk, "HGET", "srey:hash", "f1"),       "HGET")

    -- INCR counter
    t:eq(1, _exec(sk, "INCR", "srey:counter"), "INCR first")
    t:eq(2, _exec(sk, "INCR", "srey:counter"), "INCR second")

    -- 错误应答：顶层 (false, 文本)，分得清同内容的字符串值；EXEC 数组里的错误是 { err = 文本 }
    local ev, emsg = _exec(sk, "GET", "srey:hash")
    t:eq(false, ev, "错误应答顶层返回 false")
    t:check("string" == type(emsg) and emsg:find("^WRONGTYPE"), "错误文本走第二返回值: " .. tostring(emsg))
    t:eq("OK", _exec(sk, "MULTI"), "MULTI")
    t:eq("QUEUED", _exec(sk, "GET", "srey:hash"), "事务里入队会失败的 GET")
    t:eq("QUEUED", _exec(sk, "INCR", "srey:counter"), "事务里入队 INCR")
    local ex = _exec(sk, "EXEC")
    if t:check("table" == type(ex) and "table" == type(ex[1]), "EXEC 里的错误元素是表") then
        t:check(ex[1].err and ex[1].err:find("^WRONGTYPE"), "错误元素的 err 带服务端文本: " .. tostring(ex[1].err))
        t:eq(3, ex[2], "EXEC 里其余元素不受影响")
    end

    -- TYPE 命令（验证 simple string）
    local typeval = _exec(sk, "TYPE", "srey:hash")
    t:eq("hash", typeval, "TYPE hash")

    -- EXISTS
    t:eq(1, _exec(sk, "EXISTS", "srey:hash"), "EXISTS hash")
    t:eq(0, _exec(sk, "EXISTS", "srey:nonexist"), "EXISTS nonexist")

    -- LPUSH / LRANGE list
    t:check(_exec(sk, "DEL", "srey:list") ~= nil, "DEL list pre-clean")
    t:eq(3, _exec(sk, "LPUSH", "srey:list", "c", "b", "a"), "LPUSH 3 items")
    local lr = _exec(sk, "LRANGE", "srey:list", 0, -1)
    if t:check(type(lr) == "table" and #lr == 3, "LRANGE returns 3-item array") then
        t:eq("a", lr[1], "LRANGE [1]")
    end

    -- ── RESP3 路径：切到 protover 3 触发 map / set / 嵌套聚合 unpack ──
    do
        local hello = _exec(sk, "HELLO", "3")
        if t:check(type(hello) == "table", "HELLO 3 returns table") then
            -- HELLO 3 返回 map，常见字段：server / version / proto / id / mode / role
            t:check(hello.server ~= nil, "HELLO 3 map has 'server' key")
            t:eq(3, hello.proto, "HELLO 3 reports proto=3")
            t:check(type(hello.id) == "number", "HELLO 3 reports numeric id")
            t:check(type(hello.version) == "string", "HELLO 3 reports version string")
        end
    end
    do
        -- set 类型：SMEMBERS 在 RESP3 下返回 set marker '~'
        _exec(sk, "DEL", "srey:set")
        local n = _exec(sk, "SADD", "srey:set", "a", "b", "c")
        t:eq(3, n, "SADD returns 3")
        local members = _exec(sk, "SMEMBERS", "srey:set")
        if t:check(type(members) == "table" and #members == 3,
                   "SMEMBERS RESP3 set decoded as 3-array") then
            -- 元素应包含 a/b/c（set 无序）
            local set = { [members[1]] = true, [members[2]] = true, [members[3]] = true }
            t:check(set.a and set.b and set.c, "SMEMBERS contains a/b/c")
        end
        _exec(sk, "DEL", "srey:set")
    end
    do
        -- map 嵌套 array：CONFIG GET 在 RESP3 下返回 map（key->value）
        local cfg = _exec(sk, "CONFIG", "GET", "maxmemory")
        t:check(type(cfg) == "table" and cfg.maxmemory ~= nil,
                "CONFIG GET maxmemory RESP3 map has maxmemory key")
    end
    do
        -- 空 set / 空 array 路径
        _exec(sk, "DEL", "srey:emptyset")
        local empty = _exec(sk, "SMEMBERS", "srey:emptyset")
        if t:check(type(empty) == "table", "empty SMEMBERS returns table") then
            t:eq(0, #empty, "empty SMEMBERS length 0")
        end
    end
    do
        -- 字段名与解析器哨兵同名：RESP3 下 HGETALL 返回 map，而 unpack 一度把 resp_type /
        -- resp_nelem 和载荷放在同一张表里，用户这两个字段会盖掉哨兵 —— 结构塌成数组、
        -- 字段被当哨兵删掉，且全程无报错。Redis 的字段名是任意二进制串，这不需要恶意服务端
        _exec(sk, "DEL", "srey:sentinel")
        t:eq(2, _exec(sk, "HSET", "srey:sentinel", "resp_type", "x", "a", "1"),
             "HSET 含 resp_type 字段")
        local h = _exec(sk, "HGETALL", "srey:sentinel")
        if t:check(type(h) == "table", "HGETALL 返回 table") then
            t:eq("x", h.resp_type, "撞哨兵的字段 resp_type 保留原值（塌成数组时这里是 nil）")
            t:eq("1", h.a, "同 map 内其余字段仍按 k/v 解出（塌成数组时这里是 nil）")
            t:eq(nil, h[1], "结构未退化成数组")
        end
        -- resp_nelem 同理：它不参与 map 判定，症状是字段静默丢失
        _exec(sk, "DEL", "srey:sentinel")
        t:eq(2, _exec(sk, "HSET", "srey:sentinel", "resp_nelem", "9", "b", "2"),
             "HSET 含 resp_nelem 字段")
        local h2 = _exec(sk, "HGETALL", "srey:sentinel")
        if t:check(type(h2) == "table", "HGETALL 返回 table(resp_nelem 轮)") then
            t:eq("9", h2.resp_nelem, "撞哨兵的字段 resp_nelem 保留原值")
            t:eq("2", h2.b, "同 map 内其余字段不受影响")
        end
        _exec(sk, "DEL", "srey:sentinel")
    end

    do
        -- RESP3 推送：订阅确认算回复；message 推送不唤醒等回复的协程，syn_recv_any 能收到。
        -- 发布走另一条连接、发出去不等回复；挂着 BLPOP 时发布，推送一定落在它等满 0.2 秒的空回复之前
        local CH = "srey:chan"
        local pub = redis.connect(DB_HOST, 6379, SSL_NAME.NONE, nil, 0)
        if t:check(pub.valid, "发布连接") then
            local function _publish(text)
                local cmd, size = redis.pack_ud("PUBLISH", CH, text)
                srey.send(pub, cmd, size, 0)
            end
            local sub = _exec(sk, "SUBSCRIBE", CH)
            t:check("table" == type(sub) and "subscribe" == sub[1], "SUBSCRIBE 确认唤醒等待者")
            _publish("first")
            t:eq(nil, (_exec(sk, "BLPOP", "srey:nolist", "0.2")), "推送不唤醒等回复的协程，等来的是 BLPOP 的空回复")
            _publish("second")
            local msg = redis.unpack((srey.syn_recv_any(sk)))
            if t:check("table" == type(msg), "syn_recv_any 收到 message 推送") then
                t:eq("message", msg[1], "推送类型")
                t:eq(CH, msg[2], "推送频道")
                t:eq("second", msg[3], "推送内容")
            end
            local unsub = _exec(sk, "UNSUBSCRIBE", CH)
            t:check("table" == type(unsub) and "unsubscribe" == unsub[1], "UNSUBSCRIBE 确认唤醒等待者")
            t:eq("PONG", _exec(sk, "PING"), "退订后命令照常")
            srey.close(pub)
        end
    end

    do
        -- Redis 8.8 新命令：组包走通用格式化、回复是已有的 RESP 类型，框架不用改。低版本跳过
        local hv = _exec(sk, "HELLO", "3")
        local ver = "table" == type(hv) and hv.version or ""
        local major, minor = tostring(ver):match("^(%d+)%.(%d+)")
        if (tonumber(major) or 0) * 100 + (tonumber(minor) or 0) < 808 then
            printd("[db_redis] Redis 8.8 commands need 8.8+, case skipped (version %s)", tostring(ver))
        else
            _exec(sk, "DEL", "srey:increx", "srey:arr", "srey:xs")
            local r = _exec(sk, "INCREX", "srey:increx", "BYINT", 5, "UBOUND", 8)
            t:check("table" == type(r) and 5 == r[1] and 5 == r[2], "INCREX 回 [新值, 实际增量]")
            r = _exec(sk, "INCREX", "srey:increx", "BYINT", 5, "UBOUND", 8)
            t:check("table" == type(r) and 5 == r[1] and 0 == r[2], "INCREX 超上界不加，实际增量为 0")
            t:eq(2, _exec(sk, "ARSET", "srey:arr", 0, "a", "b"), "ARSET 回写入个数")
            t:eq("b", _exec(sk, "ARGET", "srey:arr", 1), "ARGET 取值")
            t:eq(nil, _exec(sk, "ARGET", "srey:arr", 5), "ARGET 未设的下标为 nil")
            local id = _exec(sk, "XADD", "srey:xs", "*", "f", "v")
            t:eq("OK", _exec(sk, "XGROUP", "CREATE", "srey:xs", "g", "0"), "XGROUP CREATE")
            t:check(nil ~= _exec(sk, "XREADGROUP", "GROUP", "g", "c1", "STREAMS", "srey:xs", ">"), "XREADGROUP 读到消息")
            t:eq(1, _exec(sk, "XNACK", "srey:xs", "g", "FAIL", "IDS", 1, id), "XNACK 回释放条数")
            _exec(sk, "DEL", "srey:increx", "srey:arr", "srey:xs")
        end
    end

    -- 清理
    _exec(sk, "DEL", "srey:hash", "srey:counter", "srey:list")
    srey.close(sk)
end)
end)
