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

    mg:quit()
end)
end)
