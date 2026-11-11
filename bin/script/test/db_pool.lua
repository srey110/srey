-- 连接池真实库集成：MySQL/PostgreSQL 各用 2 条连接跑 4 个并发查询，确认池能并发复用；
-- MongoDB 跑 4 个并发 count，确认 conn 的 connect/ping/quit 与池的借还在真实协议上可用。

local srey      = require("lib.srey")
local runner    = require("test.runner")
local db_pool = require("lib.db_pool")
local mysql     = require("lib.mysql")
local pgsql     = require("lib.pgsql")
local mongo     = require("lib.mongo")
local bson      = require("lib.bson")
local sssl      = require("srey.ssl")

local DB_HOST = "127.0.0.1"

-- MySQL 用不带客户端证书的 TLS 上下文（同 db_mysql.lua）：明文下 caching_sha2 完整认证可能失败
local SSL_NOCERT = "db_pool_nocert"
local function _mysql_ssl()
    if not sssl.with_ssl() then
        return SSL_NAME.NONE
    end
    if not srey.ssl_query(SSL_NOCERT) then
        sssl.cert_register(SSL_NOCERT, "", "", "")
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
    -- 空闲期间连接被关掉：established 还是 true，借出前要看出连接标识已失效、就地重连，不借出死连接
    srey.close(c.conn:sock_id())
    for _ = 1, 50 do-- 等 CLOSE 处理完(最多约 1 秒)
        if not c.conn:sock_id().valid then
            break
        end
        srey.sleep(20)
    end
    local c3 = pool:get(5000)
    t:eq(2, pool:stat_get().connects, name .. " txn: 空闲期被关掉的借出前重连")
    t:check(c3 and c3:query("select 1"), name .. " txn: 重连后查询照常")
    if c3 then
        pool:put(c3)
    end
    pool:free()
end

-- mysql 的会话状态（同 C 侧 task_mysql 的 _pool_txn）：autocommit=0 下语句回 ERR 按在事务里算、归还回滚不重连；
-- 只 set autocommit=0 就归还的，归还时把 autocommit 还原
local function _run_mysql_session(t, factory)
    local pool = db_pool.new{ name = "mysql_pool_sess", max = 1, backoff = 1000, factory = factory }
    local c = pool:get(5000)
    if not t:check(nil ~= c, "mysql_pool sess: 借到连接") then
        pool:free()
        return
    end
    t:check(c:query("set autocommit=0"), "mysql_pool sess: set autocommit=0")
    c:query("select * from srey_pool_no_such_table")
    t:check(c:in_trans(), "mysql_pool sess: 语句回 ERR 后按在事务里算")
    pool:put(c)
    local c2 = pool:get(5000)
    t:check(c == c2, "mysql_pool sess: 还是同一条")
    t:eq(1, pool:stat_get().connects, "mysql_pool sess: 回滚不重连")
    t:check(c2 and not c2:in_trans() and (c2.mysql:autocommit()), "mysql_pool sess: 交出来时已回滚、autocommit 已还原")
    if c2 then
        t:check(c2:query("set autocommit=0") and not c2:in_trans() and not (c2.mysql:autocommit()),
                "mysql_pool sess: 只关 autocommit、不执行语句")
        pool:put(c2)
    end
    local c3 = pool:get(5000)
    t:eq(1, pool:stat_get().connects, "mysql_pool sess: 还原 autocommit 不重连")
    t:check(c3 and (c3.mysql:autocommit()), "mysql_pool sess: 只关 autocommit 就归还的也还原了")
    if c3 then
        pool:put(c3)
    end
    pool:free()
end

-- mongo 事务没收尾就归还：池回滚解绑，下一个人能开自己的事务(同 C 侧 task_mongo 的 _pool_flow)。空事务回滚纯本地，不需要 replica set
local function _run_mongo_txn(t)
    local pool = db_pool.new{
        name = "mongo_pool_txn", max = 1, backoff = 1000,
        factory = function()
            return mongo.new(DB_HOST, 27017, SSL_NAME.NONE,
                             "test", "admin", "12345678", "admin", "SCRAM-SHA-256")
        end,
    }
    local c = pool:get(5000)
    if not t:check(nil ~= c, "mongo_pool txn: 借到连接") then
        pool:free()
        return
    end
    local sess = c:startsession()
    t:check(nil ~= sess and sess:begin(), "mongo_pool txn: begin 成功")
    pool:put(c)
    local c2 = pool:get(5000)
    t:check(c == c2, "mongo_pool txn: 还是同一条")
    local sess2 = c2 and c2:startsession()
    t:check(nil ~= sess2 and sess2:begin(), "mongo_pool txn: 下一个人能开自己的事务")
    if sess2 then
        sess2:rollback()
        sess2:close()
    end
    if sess then
        sess:close()
    end
    if c2 then
        pool:put(c2)
    end
    -- 事务里写过一条再归还：reset 真发 abortTransaction，连接保留，那条写不可见。需要副本集(同 db_mongo 事务段)
    local c3 = pool:get(5000)
    if t:check(c == c3, "mongo_pool txn: 再借还是同一条") then
        local TCOL = "srey_test_pool_txn_lua"
        c3:drop(TCOL)-- 集合本就可能不存在，返回值不判
        local sess3 = c3:startsession()
        t:check(nil ~= sess3 and sess3:begin(), "mongo_pool txn: 写之前 begin 成功")
        local doc = bson.encode({ { id = 500, name = "pool-txn" } })
        local dptr, dsz = doc:data()
        t:check(c3:insert(TCOL, dptr, dsz), "mongo_pool txn: 事务内 insert ok")
        pool:put(c3)
        local c4 = pool:get(5000)
        t:check(c == c4, "mongo_pool txn: reset 发 abort 后连接保留")
        if c4 then
            local empty = bson.encode({})
            local eptr, esz = empty:data()
            t:eq(0, c4:count(TCOL, eptr, esz), "mongo_pool txn: 事务里那条不可见")
            c4:drop(TCOL)-- 用完删掉，别留在库里
        end
        if sess3 then
            sess3:close()
        end
        if c4 then
            pool:put(c4)
        end
    elseif c3 then
        pool:put(c3)
    end
    pool:free()
end

srey.startup(function()
runner.run(function(t)
    _run_mysql(t)
    _run_pgsql(t)
    _run_mongo(t)
    _run_mongo_txn(t)
    local ssl = _mysql_ssl()
    local function mysql_factory()
        return mysql.new(DB_HOST, 3306, ssl, "admin", "12345678", "test", "utf8mb4", 0)
    end
    _run_txn(t, "mysql_pool", mysql_factory, function(c)
        return c:in_trans()
    end)
    _run_mysql_session(t, mysql_factory)
    _run_txn(t, "pgsql_pool", function()
        return pgsql.new(DB_HOST, 5432, SSL_NAME.NONE, "admin", "12345678", "test")
    end, function(c)
        return 73 ~= c:readyforquery()
    end)
end)
end)
