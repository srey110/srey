-- MongoDB 集成测试：依赖 docker-compose 已起 mongo 容器（admin/12345678 authdb=admin）

local srey   = require("lib.srey")
local runner = require("test.runner")
local mongo  = require("lib.mongo")
local bson   = require("lib.bson")
local mgmod  = require("mongo")

srey.startup(function()
runner.run("db_mongo", function(t)
    local mg = mongo.new("127.0.0.1", 27017, SSL_NAME.NONE,
                         "test", "admin", "12345678", "admin", "SCRAM-SHA-256")
    if not mg:connect() then
        t:fail("mongo connect failed")
        return
    end
    t:check(mg:ping(), "mongo ping")
    mg:collection("srey_test")
    mg:drop()

    -- insert 3 docs（sequence table → ARRAY）
    local docs = bson.encode({
        { id = 1, name = "alice",   score = 90 },
        { id = 2, name = "bob",     score = 75 },
        { id = 3, name = "charlie", score = 60 },
    })
    local dptr, dsz = docs:data()
    local ok, n = mg:insert("srey_test", dptr, dsz)
    t:check(ok, "mongo insert ok")
    t:eq(3, n, "mongo insert n=3")

    -- count
    local empty = bson.encode({})
    local eptr, esz = empty:data()
    local cnt = mg:count("srey_test", eptr, esz)
    t:eq(3, cnt, "mongo count 3")

    -- find 全部
    local mgopack = mg:find("srey_test", eptr, esz)
    if mgopack then
        local fptr, fsz = mgmod.doc(mgopack)
        local resp = bson.decode(fptr, fsz)
        t:check(resp.cursor and resp.cursor.firstBatch, "find returns firstBatch")
        t:eq(3, #resp.cursor.firstBatch, "find returns 3 docs")
    else
        t:fail("mongo find")
    end

    -- update id=1 score → 100
    local updates = bson.encode({
        { q = { id = 1 }, u = { ["$set"] = { score = 100 } } },
    })
    local uptr, usz = updates:data()
    local uok, un = mg:update("srey_test", uptr, usz)
    t:check(uok, "mongo update ok")
    t:eq(1, un, "mongo update n=1")

    -- delete id=3
    local deletes = bson.encode({
        { q = { id = 3 }, limit = 1 },
    })
    local xptr, xsz = deletes:data()
    local dok, dn = mg:delete("srey_test", xptr, xsz)
    t:check(dok, "mongo delete ok")
    t:eq(1, dn, "mongo delete n=1")

    -- count 剩 2
    cnt = mg:count("srey_test", eptr, esz)
    t:eq(2, cnt, "mongo count after delete")

    -- ping 自动重连：quit 关闭连接后 ping 应检测到死连接并重连
    mg:quit()
    t:check(mg:ping(), "mongo ping auto-reconnect after quit")
    cnt = mg:count("srey_test", eptr, esz)
    t:eq(2, cnt, "mongo count after reconnect")

    -- MORETOCOME fire-and-forget：set 后 insert 不等响应(返 true 无 n)；
    -- 标志仍置位下 count(读内部 clear/restore)应正常并反映 fire-forget 写
    mg:set_flag(mg.FLAGS.MORETOCOME)
    local fdoc = bson.encode({ { id = 200, name = "fire", score = 1 } })
    local fptr, fsz = fdoc:data()
    t:check(mg:insert("srey_test", fptr, fsz), "mongo MORETOCOME insert fire-forget")
    cnt = mg:count("srey_test", eptr, esz)
    mg:clear_flag()
    t:eq(3, cnt, "mongo count after MORETOCOME insert")

    -- 事务：commit 后事务内插入可见，rollback 后不可见。同一 session 连做两个事务，
    -- 覆盖 txnNumber 递增与 startTransaction 只附加于每个事务首个操作
    local sess = mg:startsession()
    if not sess then
        t:fail("mongo startsession")
    else
        t:check(sess:refresh(), "session refresh")

        t:check(sess:begin(), "txn begin (commit path)")
        local tdoc = bson.encode({ { id = 300, name = "txn-commit", score = 7 } })
        local tptr, tsz = tdoc:data()
        local tok, tn = mg:insert("srey_test", tptr, tsz)
        t:check(tok, "txn insert ok")
        t:eq(1, tn, "txn insert n=1")
        t:check(sess:commit(), "txn commit")
        cnt = mg:count("srey_test", eptr, esz)
        t:eq(4, cnt, "commit 后事务内插入可见")

        t:check(sess:begin(), "txn begin (rollback path)")
        local rdoc = bson.encode({ { id = 301, name = "txn-abort", score = 8 } })
        local rptr, rsz = rdoc:data()
        t:check(mg:insert("srey_test", rptr, rsz), "txn insert (rollback path) ok")
        t:check(sess:rollback(), "txn rollback")
        cnt = mg:count("srey_test", eptr, esz)
        t:eq(4, cnt, "rollback 后事务内插入不可见")

        -- 连接不再绑定该 session 时，pack_commit / pack_abort 须返回 nil：组包取的是
        -- 连接当前绑定的 session，分叉时会把本次提交挂到别人的事务上。
        -- 直接调 clear_session 造出分叉——真实来源是 _connect 在 clear_session 之后、
        -- generation 递增之前失败退出，那个窗口里 sess 的 gen 校验仍会通过
        t:check(sess:begin(), "txn begin (unbound path)")
        mg.mongo:clear_session()
        t:eq(nil, sess.session:pack_commit(), "绑定分叉后 pack_commit 返回 nil")
        t:eq(nil, sess.session:pack_abort(), "绑定分叉后 pack_abort 返回 nil")
        -- 绑定已被清掉，sess 手上那个事务已无从提交，直接释放它

        sess:close()
    end

    mg:quit()
end)
end)
