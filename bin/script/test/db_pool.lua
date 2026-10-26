-- 连接池真实库集成：MySQL/PostgreSQL 各用 2 条连接跑 4 个并发查询，确认池能并发复用；
-- MongoDB 跑 4 个并发 count，确认 conn 的 connect/ping/quit 与池的借还在真实协议上可用。

local srey      = require("lib.srey")
local runner    = require("test.runner")
local db_pool = require("lib.db_pool")
local mysql     = require("lib.mysql")
local pgsql     = require("lib.pgsql")
local mongo     = require("lib.mongo")
local bson      = require("lib.bson")
local core      = require("srey.core")

local DB_HOST = "127.0.0.1"

-- MySQL 用不带客户端证书的 TLS 上下文（同 db_mysql.lua）：明文下 caching_sha2 完整认证可能失败
local SSL_NOCERT = "db_pool_nocert"
local function _mysql_ssl()
    if not core.with_ssl() then
        return SSL_NAME.NONE
    end
    if not srey.ssl_qury(SSL_NOCERT) then
        core.cert_register(SSL_NOCERT, "", "", "")
    end
    return SSL_NOCERT
end

local function _run_mysql(t)
    local ssl = _mysql_ssl()
    local pool = db_pool.new{
        name = "mysql_pool", max = 2, backoff = 1000,
        factory = function()
            return mysql.new(DB_HOST, 3306, ssl, "admin", "12345678", "test", "utf8mb4", 0)
        end,
    }
    local warm_ok, warm_err = pool:warm(2)
    if not warm_ok then
        t:fail("mysql pool warm: " .. tostring(warm_err))
        return
    end
    local done, failed, in_use, peak = 0, 0, 0, 0
    for _ = 1, 4 do
        srey.fork(function()
            local c = pool:get(5000)
            if not c then
                failed = failed + 1
                return
            end
            in_use = in_use + 1
            if in_use > peak then
                peak = in_use
            end
            local rs = c:query("select sleep(0.08) as s")
            if not rs or not rs[1] then
                failed = failed + 1
            end
            in_use = in_use - 1
            pool:put(c)
            done = done + 1
        end)
    end
    for _ = 1, 300 do
        if done + failed >= 4 then
            break
        end
        srey.sleep(10)
    end
    t:eq(4, done, "mysql 4 个并发查询完成")
    t:eq(0, failed, "mysql 无失败")
    t:eq(2, peak, "mysql 2 条连接并发峰值 2")
    pool:free()
end

local function _run_pgsql(t)
    local pool = db_pool.new{
        name = "pgsql_pool", max = 2, backoff = 1000,
        factory = function()
            return pgsql.new(DB_HOST, 5432, SSL_NAME.NONE, "admin", "12345678", "test")
        end,
    }
    local warm_ok, warm_err = pool:warm(2)
    if not warm_ok then
        t:fail("pgsql pool warm: " .. tostring(warm_err))
        return
    end
    local done, failed, in_use, peak = 0, 0, 0, 0
    for _ = 1, 4 do
        srey.fork(function()
            local c = pool:get(5000)
            if not c then
                failed = failed + 1
                return
            end
            in_use = in_use + 1
            if in_use > peak then
                peak = in_use
            end
            local rs = c:query("select pg_sleep(0.08)")
            if not rs or not rs[1] then
                failed = failed + 1
            end
            in_use = in_use - 1
            pool:put(c)
            done = done + 1
        end)
    end
    for _ = 1, 300 do
        if done + failed >= 4 then
            break
        end
        srey.sleep(10)
    end
    t:eq(4, done, "pgsql 4 个并发查询完成")
    t:eq(0, failed, "pgsql 无失败")
    t:eq(2, peak, "pgsql 2 条连接并发峰值 2")
    pool:free()
end

local function _run_mongo(t)
    local empty = bson.encode({})
    local p, s = empty:data()
    local pool = db_pool.new{
        name = "mongo_pool", max = 2, backoff = 1000,
        factory = function()
            return mongo.new(DB_HOST, 27017, SSL_NAME.NONE,
                             "test", "admin", "12345678", "admin", "SCRAM-SHA-256")
        end,
    }
    local warm_ok, warm_err = pool:warm(2)
    if not warm_ok then
        t:fail("mongo pool warm: " .. tostring(warm_err))
        return
    end
    local done, failed = 0, 0
    for _ = 1, 4 do
        srey.fork(function()
            local c = pool:get(5000)
            if not c then
                failed = failed + 1
                return
            end
            local n = c:count("srey_test", p, s)
            if "number" ~= type(n) then
                failed = failed + 1
            end
            pool:put(c)
            done = done + 1
        end)
    end
    for _ = 1, 300 do
        if done + failed >= 4 then
            break
        end
        srey.sleep(10)
    end
    t:eq(4, done, "mongo 4 个并发计数完成")
    t:eq(0, failed, "mongo 无失败")
    pool:free()
end

-- 事务没结束就归还：池先回滚再交给下一个人，同一条连接、不重连（同 C 侧 task_mysql / task_pgsql 的 _pool_txn）
local function _run_txn(t, name, factory, in_trans)
    local pool = db_pool.new{ name = name, max = 1, backoff = 1000, factory = factory }
    local c = pool:get(5000)
    if not t:check(nil ~= c, name .. " txn: 借到连接") then
        pool:free()
        return
    end
    t:check(c:query("begin") and in_trans(c), name .. " txn: begin 后在事务里")
    pool:put(c)
    local c2 = pool:get(5000)
    t:check(c == c2, name .. " txn: 还是同一条")
    t:eq(1, pool:stat_get().connects, name .. " txn: 回滚不重连")
    t:check(c2 and not in_trans(c2), name .. " txn: 交出来时已回滚")
    if c2 then
        pool:put(c2)
    end
    pool:free()
end

srey.startup(function()
runner.run(function(t)
    _run_mysql(t)
    _run_pgsql(t)
    _run_mongo(t)
    local ssl = _mysql_ssl()
    _run_txn(t, "mysql_pool", function()
        return mysql.new(DB_HOST, 3306, ssl, "admin", "12345678", "test", "utf8mb4", 0)
    end, function(c)
        return c.mysql:in_trans()
    end)
    _run_txn(t, "pgsql_pool", function()
        return pgsql.new(DB_HOST, 5432, SSL_NAME.NONE, "admin", "12345678", "test")
    end, function(c)
        return 73 ~= c:readyforquery()
    end)
end)
end)
