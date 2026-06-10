-- db 绑定层单元测试（不连数据库）：
-- mysql.bind / pgsql.bind 的 setter 链 + 边界类型处理 + mongo.session 的 pack 路径

local srey   = require("lib.srey")
local runner = require("test.runner")
local utils  = require("srey.utils")
local mbind  = require("mysql.bind")
local pbind  = require("pgsql.bind")
local mongo  = require("mongo")
local mgsess = require("mongo.session")
local mysql  = require("mysql")
local pgsql  = require("pgsql")
local bson   = require("lib.bson")

srey.startup(function()
runner.run("db_bind", function(t)
    -- ── mysql.bind ─────────────────────────────────────────────────────
    do
        local b = mbind.new()
        t:check(b ~= nil, "mysql.bind.new")
        -- clear 是幂等操作，未填值前调也无副作用
        b:clear()
        -- 命名参数（query attribute 用）
        b:integer("k_int8", 127)
        b:integer("k_int64", 9007199254740991)  -- 2^53-1（lua_Integer 安全范围上限）
        b:float("k_float", 1.5)
        b:double("k_double", 3.14159265358979)
        b:string("k_str", "srey-mysql-bind")
        b:datetime("k_dt", os.time())
        b:time("k_tm", 0, 0, 1, 30, 0)
        b:null("k_null")
        -- 重复 clear + 再填，验证可复用
        b:clear()
        b:integer(nil, 42)  -- 匿名参数（stmt 占位符 ?）
        b:string(nil, "stmt-bind")
        -- GC 析构走 __gc → _lmysql_bind_free
        b = nil
        collectgarbage()
        t:check(true, "mysql.bind GC roundtrip ok")
    end

    -- ── pgsql.bind ─────────────────────────────────────────────────────
    do
        -- new(nparam) 预设参数数
        local b = pbind.new(8)
        t:check(b ~= nil, "pgsql.bind.new(8)")

        -- 各类型 setter：每个 set 自动占用一个槽位
        b:int16(32767)
        b:int32(-2147483648)
        b:int64(9007199254740991)
        b:float(2.5)
        b:double(3.14159)
        b:text("pg-bind-test")
        b:bytea("\x00\x01\x02\xff")
        b:bool(true)
        b:clear()

        -- 非 number 类型自动转 NULL（bool 接受 boolean/number）
        local b2 = pbind.new(5)
        b2:bool(nil)     -- 转 NULL
        b2:int32(nil)    -- 转 NULL
        b2:double(nil)   -- 转 NULL
        b2:text(nil)     -- 转 NULL
        b2:null()        -- 显式 NULL
        b2 = nil

        -- 时间相关
        local b3 = pbind.new(3)
        b3:timestamp(os.time())
        b3:date(20260522)
        b3:uuid("0123456789abcdef0123456789abcdef")   -- 32 字符
        b3 = nil
        collectgarbage()
        t:check(true, "pgsql.bind GC roundtrip ok")
    end

    -- ── mongo.session ──────────────────────────────────────────────────
    do
        -- 不连接，仅构造 mongo ctx；session.new 需要 16 字节 uuid
        local mg = mongo.new("127.0.0.1", 27017, nil, "testdb")
        t:check(mg ~= nil, "mongo.new (无连接)")

        -- uuid 长度校验：!= 16 字节返回 nil
        local bad = mgsess.new(mg, "short", 10)
        t:eq(nil, bad, "session.new uuid 短长度拒绝")

        -- 合法 16 字节 uuid 创建 session
        local uuid = string.rep("\xab", 16)
        local sess = mgsess.new(mg, uuid, 30)
        t:check(sess ~= nil, "session.new 16 字节 uuid")

        -- pack_endsession：无需 begin，直接打包 endSessions 命令
        local pack, size = sess:pack_endsession()
        t:check(pack ~= nil and size > 0, "session pack_endsession")
        -- wire 上含 endSessions 字段名（mongo OP_MSG body 一般为 BSON 字符串）
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("endSessions", 1, true) ~= nil, "endSessions in wire")
        utils.ud_free(pack)

        -- pack_refresh
        pack, size = sess:pack_refresh()
        t:check(pack ~= nil and size > 0, "session pack_refresh")
        txt = srey.ud_str(pack, size)
        t:check(txt:find("refreshSessions", 1, true) ~= nil, "refreshSessions in wire")
        utils.ud_free(pack)

        -- begin → pack_commit / pack_abort：commit/abort 依赖 begin 构造 options
        sess:begin()
        pack, size = sess:pack_commit()
        t:check(pack ~= nil and size > 0, "session pack_commit (after begin)")
        txt = srey.ud_str(pack, size)
        t:check(txt:find("commitTransaction", 1, true) ~= nil, "commitTransaction in wire")
        utils.ud_free(pack)
        sess:done()

        sess:begin()
        pack, size = sess:pack_abort()
        t:check(pack ~= nil and size > 0, "session pack_abort (after begin)")
        txt = srey.ud_str(pack, size)
        t:check(txt:find("abortTransaction", 1, true) ~= nil, "abortTransaction in wire")
        utils.ud_free(pack)
        sess:done()

        -- free + 重复 free 幂等
        sess:free()
        sess:free()
        sess = nil
        mg = nil
        collectgarbage()
        t:check(true, "mongo.session GC roundtrip ok")
    end

    -- ── mysql packer 系列（不连接，仅 buffer 构造）────────────────────
    do
        local m = mysql.new("127.0.0.1", 3306, nil, "admin", "x", "testdb", "utf8mb4")
        t:check(m ~= nil, "mysql.new (无连接)")

        local pack, size = m:pack_ping()
        t:check(pack ~= nil and size > 0, "mysql pack_ping non-empty")
        utils.ud_free(pack)

        pack, size = m:pack_quit()
        t:check(pack ~= nil and size > 0, "mysql pack_quit non-empty")
        utils.ud_free(pack)

        pack, size = m:pack_selectdb("newdb")
        t:check(pack ~= nil and size > 0, "mysql pack_selectdb non-empty")
        t:check(srey.ud_str(pack, size):find("newdb", 1, true) ~= nil, "selectdb wire 含库名")
        utils.ud_free(pack)

        pack, size = m:pack_query("SELECT 1")
        t:check(pack ~= nil and size > 0, "mysql pack_query non-empty")
        t:check(srey.ud_str(pack, size):find("SELECT 1", 1, true) ~= nil, "query wire 含 SQL")
        utils.ud_free(pack)

        -- 带 bind 的 query
        local b = mbind.new()
        b:integer("k", 42)
        pack, size = m:pack_query("SELECT ? AS v", b)
        t:check(pack ~= nil and size > 0, "mysql pack_query with bind non-empty")
        utils.ud_free(pack)

        pack, size = m:pack_stmt_prepare("SELECT * FROM t WHERE id=?")
        t:check(pack ~= nil and size > 0, "mysql pack_stmt_prepare non-empty")
        t:check(srey.ud_str(pack, size):find("WHERE id=?", 1, true) ~= nil, "prepare wire 含 SQL")
        utils.ud_free(pack)

        m = nil
        collectgarbage()
        t:check(true, "mysql packer GC roundtrip ok")
    end

    -- ── pgsql packer 系列（不连接，仅 buffer 构造）────────────────────
    do
        local p = pgsql.new("127.0.0.1", 5432, nil, "admin", "x", "testdb")
        t:check(p ~= nil, "pgsql.new (无连接)")

        local pack, size = pgsql.pack_query("SELECT 1")
        t:check(pack ~= nil and size > 0, "pgsql pack_query non-empty")
        t:check(srey.ud_str(pack, size):find("SELECT 1", 1, true) ~= nil, "pg query wire 含 SQL")
        utils.ud_free(pack)

        pack, size = pgsql.pack_terminate()
        t:check(pack ~= nil and size > 0, "pgsql pack_terminate non-empty")
        utils.ud_free(pack)

        pack, size = pgsql.pack_stmt_prepare("st1", "SELECT $1::int", 1, { 23 })
        t:check(pack ~= nil and size > 0, "pgsql pack_stmt_prepare non-empty")
        t:check(srey.ud_str(pack, size):find("st1", 1, true) ~= nil, "prepare wire 含 stmt name")
        utils.ud_free(pack)

        pack, size = pgsql.pack_stmt_close("st1")
        t:check(pack ~= nil and size > 0, "pgsql pack_stmt_close non-empty")
        utils.ud_free(pack)

        -- pack_stmt_execute 不带 bind
        pack, size = pgsql.pack_stmt_execute("st1")
        t:check(pack ~= nil and size > 0, "pgsql pack_stmt_execute (no bind) non-empty")
        utils.ud_free(pack)

        -- pack_stmt_execute 带 bind
        local b = pbind.new(1)
        b:int32(42)
        pack, size = pgsql.pack_stmt_execute("st1", b)
        t:check(pack ~= nil and size > 0, "pgsql pack_stmt_execute with bind non-empty")
        utils.ud_free(pack)

        -- COPY 流操作
        pack, size = pgsql.pack_copy_data("1,2,3\n")
        t:check(pack ~= nil and size > 0, "pgsql pack_copy_data non-empty")
        utils.ud_free(pack)

        pack, size = pgsql.pack_copy_done()
        t:check(pack ~= nil and size > 0, "pgsql pack_copy_done non-empty")
        utils.ud_free(pack)

        pack, size = pgsql.pack_copy_fail("oops")
        t:check(pack ~= nil and size > 0, "pgsql pack_copy_fail non-empty")
        t:check(srey.ud_str(pack, size):find("oops", 1, true) ~= nil, "copy_fail wire 含 reason")
        utils.ud_free(pack)

        p = nil
        collectgarbage()
        t:check(true, "pgsql packer GC roundtrip ok")
    end

    -- ── mongo packer 系列（不连接，仅 buffer 构造）────────────────────
    do
        local mg = mongo.new("127.0.0.1", 27017, nil, "testdb")
        t:check(mg ~= nil, "mongo.new (无连接) for packer")
        mg:collection("coll1")

        local pack, size = mg:pack_hello()
        t:check(pack ~= nil and size > 0, "mongo pack_hello non-empty")
        t:check(srey.ud_str(pack, size):find("hello", 1, true) ~= nil, "hello wire 含 hello")
        utils.ud_free(pack)

        pack, size = mg:pack_ping()
        t:check(pack ~= nil and size > 0, "mongo pack_ping non-empty")
        t:check(srey.ud_str(pack, size):find("ping", 1, true) ~= nil, "ping wire 含 ping")
        utils.ud_free(pack)

        pack, size = mg:pack_drop()
        t:check(pack ~= nil and size > 0, "mongo pack_drop non-empty")
        t:check(srey.ud_str(pack, size):find("drop", 1, true) ~= nil, "drop wire 含 drop")
        utils.ud_free(pack)

        -- 构造 docs/updates/deletes BSON 数组（单元素：含一个嵌套 doc）
        local docs = bson.encode({ { a = 1 } })
        local docs_ptr, docs_sz = docs:data()

        pack, size = mg:pack_insert(docs_ptr, docs_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_insert non-empty")
        t:check(srey.ud_str(pack, size):find("insert", 1, true) ~= nil, "insert wire 含 insert")
        utils.ud_free(pack)

        -- updates 数组：含 q (filter) + u (update doc)
        local updates = bson.encode({ { q = { a = 1 }, u = { ["$set"] = { b = 2 } } } })
        local upd_ptr, upd_sz = updates:data()
        pack, size = mg:pack_update(upd_ptr, upd_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_update non-empty")
        t:check(srey.ud_str(pack, size):find("update", 1, true) ~= nil, "update wire 含 update")
        utils.ud_free(pack)

        -- deletes 数组：含 q (filter) + limit
        local deletes = bson.encode({ { q = { a = 1 }, limit = 0 } })
        local del_ptr, del_sz = deletes:data()
        pack, size = mg:pack_delete(del_ptr, del_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_delete non-empty")
        t:check(srey.ud_str(pack, size):find("delete", 1, true) ~= nil, "delete wire 含 delete")
        utils.ud_free(pack)

        -- find 无 filter
        pack, size = mg:pack_find()
        t:check(pack ~= nil and size > 0, "mongo pack_find (no filter) non-empty")
        t:check(srey.ud_str(pack, size):find("find", 1, true) ~= nil, "find wire 含 find")
        utils.ud_free(pack)

        -- find 带 filter
        local filter = bson.encode({ a = 1 })
        local f_ptr, f_sz = filter:data()
        pack, size = mg:pack_find(f_ptr, f_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_find with filter non-empty")
        utils.ud_free(pack)

        -- aggregate pipeline
        local pipeline = bson.encode({ { ["$match"] = { a = 1 } }, { ["$count"] = "n" } })
        local pl_ptr, pl_sz = pipeline:data()
        pack, size = mg:pack_aggregate(pl_ptr, pl_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_aggregate non-empty")
        t:check(srey.ud_str(pack, size):find("aggregate", 1, true) ~= nil, "aggregate wire 含 aggregate")
        utils.ud_free(pack)

        pack, size = mg:pack_getmore(123456789)
        t:check(pack ~= nil and size > 0, "mongo pack_getmore non-empty")
        t:check(srey.ud_str(pack, size):find("getMore", 1, true) ~= nil, "getmore wire 含 getMore")
        utils.ud_free(pack)

        -- killcursors 数组
        local cursorids = bson.encode({ bson.mkint64(123), bson.mkint64(456) })
        local c_ptr, c_sz = cursorids:data()
        pack, size = mg:pack_killcursors(c_ptr, c_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_killcursors non-empty")
        t:check(srey.ud_str(pack, size):find("killCursors", 1, true) ~= nil, "killcursors wire 含 killCursors")
        utils.ud_free(pack)

        -- distinct 无 query
        pack, size = mg:pack_distinct("fieldA")
        t:check(pack ~= nil and size > 0, "mongo pack_distinct (no query) non-empty")
        t:check(srey.ud_str(pack, size):find("distinct", 1, true) ~= nil, "distinct wire 含 distinct")
        utils.ud_free(pack)

        -- findandmodify (remove=1)
        pack, size = mg:pack_findandmodify(f_ptr, f_sz, 1, 0, nil, 0)
        t:check(pack ~= nil and size > 0, "mongo pack_findandmodify (remove) non-empty")
        t:check(srey.ud_str(pack, size):find("findAndModify", 1, true) ~= nil, "findandmodify wire 含 findAndModify")
        utils.ud_free(pack)

        -- findandmodify (update doc)
        local update_doc = bson.encode({ ["$set"] = { b = 99 } })
        local u_ptr, u_sz = update_doc:data()
        pack, size = mg:pack_findandmodify(f_ptr, f_sz, 0, 0, u_ptr, u_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_findandmodify (update) non-empty")
        utils.ud_free(pack)

        -- count 无 query
        pack, size = mg:pack_count()
        t:check(pack ~= nil and size > 0, "mongo pack_count (no query) non-empty")
        t:check(srey.ud_str(pack, size):find("count", 1, true) ~= nil, "count wire 含 count")
        utils.ud_free(pack)

        -- createindexes 数组：{ key={a=1}, name="a_1" }
        local indexes = bson.encode({ { key = { a = 1 }, name = "a_1" } })
        local i_ptr, i_sz = indexes:data()
        pack, size = mg:pack_createindexes(i_ptr, i_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_createindexes non-empty")
        t:check(srey.ud_str(pack, size):find("createIndexes", 1, true) ~= nil, "createindexes wire 含 createIndexes")
        utils.ud_free(pack)

        -- dropindexes：index 名数组
        local dropidx = bson.encode({ "a_1" })
        local d_ptr, d_sz = dropidx:data()
        pack, size = mg:pack_dropindexes(d_ptr, d_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_dropindexes non-empty")
        t:check(srey.ud_str(pack, size):find("dropIndexes", 1, true) ~= nil, "dropindexes wire 含 dropIndexes")
        utils.ud_free(pack)

        pack, size = mg:pack_startsession()
        t:check(pack ~= nil and size > 0, "mongo pack_startsession non-empty")
        t:check(srey.ud_str(pack, size):find("startSession", 1, true) ~= nil, "startsession wire 含 startSession")
        utils.ud_free(pack)

        -- bulkwrite：ops 数组 + nsinfo 数组
        local ops = bson.encode({ { insert = 0, document = { a = 1 } } })
        local op_ptr, op_sz = ops:data()
        local nsinfo = bson.encode({ { ns = "testdb.coll1" } })
        local n_ptr, n_sz = nsinfo:data()
        pack, size = mg:pack_bulkwrite(op_ptr, op_sz, n_ptr, n_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_bulkwrite non-empty")
        t:check(srey.ud_str(pack, size):find("bulkWrite", 1, true) ~= nil, "bulkwrite wire 含 bulkWrite")
        utils.ud_free(pack)

        mg = nil
        collectgarbage()
        t:check(true, "mongo packer GC roundtrip ok")
    end
end)
end)
