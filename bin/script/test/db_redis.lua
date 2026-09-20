-- Redis 集成测试：依赖 docker-compose 已起 redis 容器（无密码）

local srey   = require("lib.srey")
local runner = require("test.runner")
local redis  = require("lib.redis")

-- 同步发送一条命令并解包响应
local function _exec(sk, ...)
    local cmd = redis.pack(...)
    local rtn = srey.syn_send(sk, cmd, #cmd, 1)
    if not rtn then
        return nil
    end
    return redis.unpack(rtn)
end

srey.startup(function()
runner.run(function(t)
    local sk = redis.connect("127.0.0.1", 6379, SSL_NAME.NONE, nil, 0)
    if not sk.valid then
        t:fail("redis connect")
        return
    end

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

    -- 清理
    _exec(sk, "DEL", "srey:hash", "srey:counter", "srey:list")
    srey.close(sk)
end)
end)
