-- Redis 集成测试：依赖 docker-compose 已起 redis 容器（无密码）

local srey   = require("lib.srey")
local runner = require("test.runner")
local redis  = require("lib.redis")

-- 同步发送一条命令并解包响应
local function _exec(fd, skid, ...)
    local cmd = redis.pack(...)
    local rtn = srey.syn_send(fd, skid, cmd, #cmd, 1)
    if not rtn then
        return nil
    end
    return redis.unpack(rtn)
end

srey.startup(function()
runner.run("db_redis", function(t)
    local fd, skid = redis.connect("127.0.0.1", 6379, SSL_NAME.NONE, nil, 0)
    if INVALID_SOCK == fd then
        t:fail("redis connect")
        return
    end

    -- SET / GET / DEL string
    t:eq("OK",    _exec(fd, skid, "SET", "srey:test", "hello"), "SET")
    t:eq("hello", _exec(fd, skid, "GET", "srey:test"),           "GET")
    t:eq(1,       _exec(fd, skid, "DEL", "srey:test"),           "DEL")

    -- HSET / HGET hash
    t:eq(1,    _exec(fd, skid, "HSET", "srey:hash", "f1", "v1"), "HSET")
    t:eq("v1", _exec(fd, skid, "HGET", "srey:hash", "f1"),       "HGET")

    -- INCR counter
    t:eq(1, _exec(fd, skid, "INCR", "srey:counter"), "INCR first")
    t:eq(2, _exec(fd, skid, "INCR", "srey:counter"), "INCR second")

    -- TYPE 命令（验证 simple string）
    local typeval = _exec(fd, skid, "TYPE", "srey:hash")
    t:eq("hash", typeval, "TYPE hash")

    -- EXISTS
    t:eq(1, _exec(fd, skid, "EXISTS", "srey:hash"), "EXISTS hash")
    t:eq(0, _exec(fd, skid, "EXISTS", "srey:nonexist"), "EXISTS nonexist")

    -- LPUSH / LRANGE list
    t:check(_exec(fd, skid, "DEL", "srey:list") ~= nil, "DEL list pre-clean")
    t:eq(3, _exec(fd, skid, "LPUSH", "srey:list", "c", "b", "a"), "LPUSH 3 items")
    local lr = _exec(fd, skid, "LRANGE", "srey:list", 0, -1)
    t:check(type(lr) == "table" and #lr == 3, "LRANGE returns 3-item array")
    t:eq("a", lr[1], "LRANGE [1]")

    -- ── RESP3 路径：切到 protover 3 触发 map / set / 嵌套聚合 unpack ──
    do
        local hello = _exec(fd, skid, "HELLO", "3")
        t:check(type(hello) == "table", "HELLO 3 returns table")
        -- HELLO 3 返回 map，常见字段：server / version / proto / id / mode / role
        t:check(hello.server ~= nil, "HELLO 3 map has 'server' key")
        t:eq(3, hello.proto, "HELLO 3 reports proto=3")
        t:check(type(hello.id) == "number", "HELLO 3 reports numeric id")
        t:check(type(hello.version) == "string", "HELLO 3 reports version string")
    end
    do
        -- set 类型：SMEMBERS 在 RESP3 下返回 set marker '~'
        _exec(fd, skid, "DEL", "srey:set")
        local n = _exec(fd, skid, "SADD", "srey:set", "a", "b", "c")
        t:eq(3, n, "SADD returns 3")
        local members = _exec(fd, skid, "SMEMBERS", "srey:set")
        t:check(type(members) == "table" and #members == 3,
                "SMEMBERS RESP3 set decoded as 3-array")
        -- 元素应包含 a/b/c（set 无序）
        local set = { [members[1]] = true, [members[2]] = true, [members[3]] = true }
        t:check(set.a and set.b and set.c, "SMEMBERS contains a/b/c")
        _exec(fd, skid, "DEL", "srey:set")
    end
    do
        -- map 嵌套 array：CONFIG GET 在 RESP3 下返回 map（key->value）
        local cfg = _exec(fd, skid, "CONFIG", "GET", "maxmemory")
        t:check(type(cfg) == "table" and cfg.maxmemory ~= nil,
                "CONFIG GET maxmemory RESP3 map has maxmemory key")
    end
    do
        -- 空 set / 空 array 路径
        _exec(fd, skid, "DEL", "srey:emptyset")
        local empty = _exec(fd, skid, "SMEMBERS", "srey:emptyset")
        t:check(type(empty) == "table", "empty SMEMBERS returns table")
        t:eq(0, #empty, "empty SMEMBERS length 0")
    end

    -- 清理
    _exec(fd, skid, "DEL", "srey:hash", "srey:counter", "srey:list")
    srey.close(fd, skid)
end)
end)
