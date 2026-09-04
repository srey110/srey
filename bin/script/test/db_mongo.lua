-- MongoDB 集成测试：依赖 docker-compose 已起 mongo 容器（admin/12345678 authdb=admin）

local srey   = require("lib.srey")
local runner = require("test.runner")
local mongo  = require("lib.mongo")
local bson   = require("lib.bson")
local mgmod  = require("srey.mongo")

srey.startup(function()
runner.run(function(t)
    local mg = mongo.new("127.0.0.1", 27017, SSL_NAME.NONE,
                         "test", "admin", "12345678", "admin", "SCRAM-SHA-256")
    if not mg:connect() then
        t:fail("mongo connect failed")
        return
    end
    t:check(mg:ping(), "mongo ping")
    -- drop 的返回值不判：集合本就可能不存在（首次跑），服务端回 "ns not found"。
    -- 改用 count 确认起点确实是空的——这才是后面所有条数断言的前提
    mg:drop("srey_test")
    local empty = bson.encode({})
    local eptr, esz = empty:data()
    t:eq(0, mg:count("srey_test", eptr, esz), "drop 之后集合为空(后续条数断言的起点)")

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
    local cnt = mg:count("srey_test", eptr, esz)
    t:eq(3, cnt, "mongo count 3")

    -- find 全部
    local mgopack = mg:find("srey_test", eptr, esz)
    if mgopack then
        local fptr, fsz = mgmod.doc(mgopack)
        local resp = bson.decode(fptr, fsz)
        local fb = resp and resp.cursor and resp.cursor.firstBatch
        t:check(fb ~= nil, "find returns firstBatch")
        t:eq(3, fb and #fb or -1, "find returns 3 docs")
        -- 字段值也要比：原来只取 #firstBatch，于是写路径丢掉 name/score 字段
        -- （或 $set 打到不存在的字段名上）只要条数还对就全看不出来。
        -- find 不保证顺序，按 id 建索引再逐条核
        local byid = {}
        for _, d in ipairs(fb or {}) do
            byid[d.id] = d
        end
        t:check(byid[1] and "alice" == byid[1].name and 90 == byid[1].score, "doc id=1 字段完好")
        t:check(byid[2] and "bob" == byid[2].name and 75 == byid[2].score, "doc id=2 字段完好")
        t:check(byid[3] and "charlie" == byid[3].name and 60 == byid[3].score, "doc id=3 字段完好")
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
    -- 回读确认改动真落到了那个字段上：只看 n=1 的话，$set 打到不存在的字段名上
    -- 服务端仍按 q 上的 id 算出 n=1，用例照样通过
    local q1 = bson.encode({ id = 1 })
    local q1ptr, q1sz = q1:data()
    local vpack = mg:find("srey_test", q1ptr, q1sz)
    if vpack then
        local vptr, vsz = mgmod.doc(vpack)
        local vr = bson.decode(vptr, vsz)
        local vb = vr and vr.cursor and vr.cursor.firstBatch
        t:check(vb and 1 == #vb and 100 == vb[1].score, "update 后回读 score=100")
    else
        t:fail("mongo find after update")
    end

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

    do
        -- 组包抛出时连接级 flags 必须恢复：读命令要等响应，所以组包期间 MORETOCOME 被临时清零，
        -- 而 pack_* 的 (指针,长度) 入口对负数长度是 luaL_argcheck 当场抛，抛点正落在清零与恢复
        -- 之间。Lua 没有 RAII，不兜一层就把 MORETOCOME 永久摘掉，且 clear_flag() 查不出它已经丢了
        mg:set_flag(mg.FLAGS.MORETOCOME)
        local pok = pcall(mg.find, mg, "srey_test", eptr, -1)
        t:eq(false, pok, "find 传负数长度按契约抛出")
        t:eq(mg.FLAGS.MORETOCOME, mg:clear_flag(),
             "组包抛出后 MORETOCOME 仍在（未恢复则为 0）")
    end

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
        -- 直接调 clear_session 造出分叉——真实来源是重连：_connect 入口会解绑，
        -- 而调用方手上那个 sess 对象还在，gen 校验也仍会通过
        t:check(sess:begin(), "txn begin (unbound path)")
        mg.mongo:clear_session()
        t:eq(nil, sess.session:pack_commit(), "绑定分叉后 pack_commit 返回 nil")
        t:eq(nil, sess.session:pack_abort(), "绑定分叉后 pack_abort 返回 nil")
        -- 绑定已被清掉，sess 手上那个事务已无从提交，直接释放它

        sess:close()
    end

    -- 并发：多协程在同一条连接上并发命令，锁点在 _wsend/_rsend 两个漏斗上。
    -- 每个协程查自己那条文档、断言回来的 id 就是自己要的那个——响应对错协程时
    -- 拿到的 id 不是自己的。发相同请求的话每个协程的正确答案是同一个，断言分不出对错，
    -- 口径同 db_mysql / db_pgsql 的 select 1000+i as v。
    -- id 自备：1..3 那批被前面的 delete / update 动过，id=3 已经没了
    local N, ROUNDS = 4, 6
    local seed = {}
    for i = 1, N do
        seed[i] = { id = 1000 + i, name = "conc" }
    end
    local sdoc = bson.encode(seed)
    local sptr, ssz = sdoc:data()
    local sok, sn = mg:insert("srey_test", sptr, ssz)
    t:check(sok, "并发段种子文档插入 ok")
    t:eq(N, sn, "并发段种子文档 n=" .. N)

    local got, done = {}, 0
    for i = 1, N do
        srey.fork(function()
            for _ = 1, ROUNDS do
                if not mg:ping() then
                    got[i] = "ping failed"
                    done = done + 1
                    return
                end
                local q = bson.encode({ id = 1000 + i })
                local qptr, qsz = q:data()
                local mp = mg:find("srey_test", qptr, qsz)
                if not mp then
                    got[i] = "find failed"
                    done = done + 1
                    return
                end
                local fp, fl = mgmod.doc(mp)
                local r = bson.decode(fp, fl)
                local batch = r and r.cursor and r.cursor.firstBatch
                if not batch or 1 ~= #batch or (1000 + i) ~= batch[1].id then
                    got[i] = string.format("got id %s want %d",
                        tostring(batch and batch[1] and batch[1].id), 1000 + i)
                    done = done + 1
                    return
                end
            end
            got[i] = true
            done = done + 1
        end)
    end
    -- 有界等待，理由同 db_mysql.lua：无界 while 会把 fork 协程抛错变成整份汇总挂住
    for _ = 1, 1500 do -- 1500 x 20ms = 30s 上限
        if done >= N then break end
        srey.sleep(20)
    end
    t:eq(N, done, "并发协程全部完成 (" .. done .. "/" .. N .. ")")
    for i = 1, N do
        t:check(true == got[i], "mongo 并发协程 " .. i .. ": " .. tostring(got[i]))
    end

    mg:quit()
end)
end)
