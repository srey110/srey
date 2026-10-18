-- MongoDB 集成测试：依赖 docker-compose 已起 mongo 容器（admin/12345678 authdb=admin）

local srey   = require("lib.srey")
local runner = require("test.runner")
local mongo  = require("lib.mongo")
local bson   = require("lib.bson")
local mgmod  = require("srey.mongo")

-- 数据库地址：在 VM 上跑时改成宿主机 IP，只改这一处
local DB_HOST = "127.0.0.1"

srey.startup(function()
runner.run(function(t)
    local mg = mongo.new(DB_HOST, 27017, SSL_NAME.NONE,
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
    -- 标志仍置位下 count(读命令组包不带它)应正常等回包并反映 fire-forget 写
    mg:set_flag(mg.FLAGS.MORETOCOME)
    local fdoc = bson.encode({ { id = 200, name = "fire", score = 1 } })
    local fptr, fsz = fdoc:data()
    t:check(mg:insert("srey_test", fptr, fsz), "mongo MORETOCOME insert fire-forget")
    cnt = mg:count("srey_test", eptr, esz)
    -- 读命令不碰连接级 flags，否则后续写命令悄悄从只发不等变成等回包
    t:eq(mg.FLAGS.MORETOCOME, mg:clear_flag(), "count(读命令)之后 MORETOCOME 仍在")
    t:eq(3, cnt, "mongo count after MORETOCOME insert")

    do
        -- 读命令的参数校验抛错时，连接级 MORETOCOME 必须原样留着：抛出路径上谁动了连接标志，
        -- 后续写命令就悄悄从只发不等变成等回包
        mg:set_flag(mg.FLAGS.MORETOCOME)
        local pok = pcall(mg.find, mg, "srey_test", eptr, -1)
        t:eq(false, pok, "find 传负数长度按契约抛出")
        t:eq(mg.FLAGS.MORETOCOME, mg:clear_flag(),
             "组包抛出后 MORETOCOME 仍在（未恢复则为 0）")
    end

    -- 事务：commit 后事务内插入可见，rollback 后不可见。同一 session 连做两个事务，
    -- 覆盖 txnNumber 递增与 startTransaction 只附加于每个事务首个操作。
    -- startsession 的 lsid 是本地生成的，下面的 refresh / begin / commit 就是本地会话能用的用例
    local sess = mg:startsession()
    if not sess then
        t:fail("mongo startsession")
    else
        -- 超时分钟数来自建连时 parse_hello 记下的值，没接上就是 0、expires_in 恒为 0
        t:check(sess:expires_in() > 0, "本地会话的超时取自 hello 的 logicalSessionTimeoutMinutes")
        t:check(sess:refresh(), "session refresh")

        t:check(sess:begin(), "txn begin (commit path)")
        local tdoc = bson.encode({ { id = 300, name = "txn-commit", score = 7 } })
        local tptr, tsz = tdoc:data()
        local tok, tn = mg:insert("srey_test", tptr, tsz)
        t:check(tok, "txn insert ok")
        t:eq(1, tn, "txn insert n=1")
        -- count 不进事务(同 C 侧 mongo_pack_count)：绑着事务照样能发、看不到还没提交的这条，也不把事务弄废。
        -- 按 id 过滤才真去扫数据：空过滤走集合元数据的记录数，那里会算上事务里还没提交的插入
        local tq = bson.encode({ id = 300 })
        local tqptr, tqsz = tq:data()
        t:eq(0, mg:count("srey_test", tqptr, tqsz), "绑着事务时 count 按事务外执行")
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
        -- 直接调 clear_session 造出分叉：重连不再解绑(只让事务作废)，分叉只剩手动清绑定这一个来源，
        -- 而调用方手上那个 sess 对象还在
        t:check(sess:begin(), "txn begin (unbound path)")
        mg.mongo:clear_session()
        t:eq(nil, sess.session:pack_commit(), "绑定分叉后 pack_commit 返回 nil")
        t:eq(nil, sess.session:pack_abort(), "绑定分叉后 pack_abort 返回 nil")
        -- 绑定已被清掉，sess 手上那个事务已无从提交，直接释放它

        -- 事务中途重连即作废(同 C 侧 _txn_stale_flow)：之后的写失败、不脱离事务自动提交；commit 返回 false，
        -- 但库已改发 abortTransaction 了结，服务端当场放锁，收尾的 drop 才不会被挡住。
        -- 用单独的集合：万一锁没放，也不拖累后面用 srey_test 的用例
        local SCOL = "srey_test_stale_lua"
        mg:drop(SCOL)-- 集合本就可能不存在，返回值不判
        t:check(sess:begin(), "txn begin (stale path)")
        local sdoc1 = bson.encode({ { id = 400, name = "stale-before" } })
        local s1ptr, s1sz = sdoc1:data()
        t:check(mg:insert(SCOL, s1ptr, s1sz), "stale: 事务内首条 insert ok")
        mg:quit()
        t:check(mg:ping(), "stale: ping 重连")
        t:check(sess.session:stale(), "stale: 重连后事务已作废")
        t:check(not mg:insert(SCOL, s1ptr, s1sz), "stale: 作废事务里的写失败")
        t:eq(false, sess:commit(), "stale: commit 返回 false")
        -- 解绑后普通写照常自动提交，也让集合实际存在(事务里隐式建的随中止撤掉了，drop 会报 ns not found)
        local sdoc2 = bson.encode({ { id = 401, name = "stale-after" } })
        local s2ptr, s2sz = sdoc2:data()
        t:check(mg:insert(SCOL, s2ptr, s2sz), "stale: 解绑后普通 insert ok")
        t:eq(1, mg:count(SCOL, eptr, esz), "stale: 只剩解绑后那条，事务里那条不可见")
        t:check(mg:drop(SCOL), "stale: commit 改发 abort 后锁已放，drop 成功")

        -- commit 挂起等回包期间另一个协程对同一 session 重新 begin(同 C 侧 _txn_rebegin_flow)：
        -- 收尾醒来不能 done 掉新事务。fork 的协程排到本条消息分发结束才起，commit 挂起时它正好跑 begin
        local RCOL = "srey_test_rebegin_lua"
        mg:drop(RCOL)-- 集合本就可能不存在，返回值不判
        t:check(sess:begin(), "rebegin: txn begin")
        local rdoc1 = bson.encode({ { id = 500, name = "rebegin-first" } })
        local r1ptr, r1sz = rdoc1:data()
        t:check(mg:insert(RCOL, r1ptr, r1sz), "rebegin: 第一个事务 insert ok")
        local rebegun
        srey.fork(function() rebegun = sess:begin() end)
        t:check(sess:commit(), "rebegin: 第一个事务 commit")
        t:eq(true, rebegun, "rebegin: commit 挂起期间同一 session 重新 begin 成功")
        local rdoc2 = bson.encode({ { id = 501, name = "rebegin-second" } })
        local r2ptr, r2sz = rdoc2:data()
        t:check(mg:insert(RCOL, r2ptr, r2sz), "rebegin: 新事务 insert ok")
        t:check(sess:commit(), "rebegin: 新事务 commit(绑定与 options 没被清掉)")
        t:eq(2, mg:count(RCOL, eptr, esz), "rebegin: 两个事务的写都可见")
        t:check(mg:drop(RCOL), "rebegin: drop")

        -- 组包之后、拿到锁之前连接换过(同 C 侧 _txn_moved_flow)：排队那条事务写带的是旧事务的上下文，
        -- 必须在拿到锁后判出来、不发、返回 false——漏判时它会被收进已作废的事务、回 n=1。
        -- fork_wait 按顺序起：先起的占锁睡一会儿，让后起的组好包排上队，再断连重连换掉 skid 后放锁
        local MCOL = "srey_test_moved_lua"
        mg:drop(MCOL)-- 集合本就可能不存在，返回值不判
        t:check(sess:begin(), "moved: txn begin")
        local mdoc1 = bson.encode({ { id = 600, name = "moved-first" } })
        local m1ptr, m1sz = mdoc1:data()
        t:check(mg:insert(MCOL, m1ptr, m1sz), "moved: 事务内首条 insert ok")
        local oldskid = mg.mongo:sock_id().skid
        local mk = mg.mongo:txn_mark()
        t:eq(oldskid, mk, "moved: 绑着事务时 txn_mark 记下当前 skid")
        t:eq(false, mg.mongo:txn_moved(mk), "moved: 没重连时 txn_moved 为 false")
        local mdoc2 = bson.encode({ { id = 601, name = "moved-queued" } })
        local m2ptr, m2sz = mdoc2:data()
        local mres = srey.fork_wait({
            function()
                return srey.serial_ret(false, mg.serial(function()
                    srey.sleep(50)
                    mg:quit()
                    return mg:ping()
                end))
            end,
            function()
                return (mg:insert(MCOL, m2ptr, m2sz))
            end,
        })
        t:check(mres[1].ok and true == mres[1].val, "moved: 占锁协程重连成功")
        t:check(oldskid ~= mg.mongo:sock_id().skid, "moved: skid 已换")
        t:check(mg.mongo:txn_moved(mk), "moved: 重连后 txn_moved 为 true")
        t:check(mres[2].ok and false == mres[2].val, "moved: 排队的事务写不发、返回 false")
        t:check(sess.session:stale(), "moved: 事务已作废")
        sess:rollback()-- 结果不判，同 C 侧：服务端还留着旧事务时这一下把它中止、放锁
        t:eq(0, mg:count(MCOL, eptr, esz), "moved: 两条都不可见")

        -- 空事务(同 C 侧 _txn_empty_flow)：begin 后一条命令都没发就收尾，不发包、本地解绑并返回 true；
        -- 真发出去服务端不认识这个 txnNumber，必回失败。重连作废后的空事务 commit 同样本地收尾
        t:check(sess:begin(), "empty: txn begin")
        t:check(sess.session:empty(), "empty: begin 后是空事务")
        t:check(sess:commit(), "empty: 空事务 commit 返回 true")
        t:check(sess:begin(), "empty: 解绑后可再 begin")
        t:check(sess:rollback(), "empty: 空事务 rollback 返回 true")
        t:check(sess:begin(), "empty: txn begin (stale)")
        mg:quit()
        t:check(mg:ping(), "empty: ping 重连")
        t:check(sess.session:stale(), "empty: 重连后事务已作废")
        t:check(sess:commit(), "empty: 作废的空事务 commit 也本地收尾返回 true")
        t:check(sess:begin(), "empty: 收尾后已解绑，可再 begin")
        t:check(sess:rollback(), "empty: 收尾")

        sess:close()
        -- endSessions 只发不等：服务端若回了包，紧跟的这条 insert 会读到它而拿不到 n=1
        local adoc = bson.encode({ { id = 302, name = "after-endsessions", score = 9 } })
        local aptr, asz = adoc:data()
        local aok, an = mg:insert("srey_test", aptr, asz)
        t:check(aok, "endSessions 之后普通 insert ok")
        t:eq(1, an, "endSessions 之后普通 insert n=1（没读到 endSessions 的回包）")
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

    -- 用 srey.fork_wait 而不是手写 done 计数 + 有界轮询,理由同 db_mysql.lua
    local fns = {}
    for i = 1, N do
        fns[i] = function()
            for _ = 1, ROUNDS do
                if not mg:ping() then
                    return "ping failed"
                end
                local q = bson.encode({ id = 1000 + i })
                local qptr, qsz = q:data()
                local mp = mg:find("srey_test", qptr, qsz)
                if not mp then
                    return "find failed"
                end
                local fp, fl = mgmod.doc(mp)
                local r = bson.decode(fp, fl)
                local batch = r and r.cursor and r.cursor.firstBatch
                if not batch or 1 ~= #batch or (1000 + i) ~= batch[1].id then
                    return string.format("got id %s want %d",
                        tostring(batch and batch[1] and batch[1].id), 1000 + i)
                end
            end
            return true
        end
    end
    local res = srey.fork_wait(fns)
    t:eq(N, #res, "fork_wait 收齐 N 项 (" .. #res .. "/" .. N .. ")")
    for i = 1, N do
        t:check(res[i].ok and true == res[i].val,
                "mongo 并发协程 " .. i .. ": " .. tostring(res[i].ok and res[i].val or res[i][1]))
    end

    -- 其余命令各一个正向用例，用单独的集合，结尾 drop。响应包只到本协程下次挂起前有效，取完就解码
    local CCOL = "srey_cmds_lua"
    local function _resp(mp)
        if not mp then
            return nil
        end
        return bson.decode(mgmod.doc(mp))
    end
    mg:drop(CCOL)-- 集合本就可能不存在，返回值不判
    local c1 = bson.encode({ { id = 1, grp = "a", v = 1 } })
    local c1ptr, c1sz = c1:data()
    t:check(mg:insert(CCOL, c1ptr, c1sz), "cmds: 首条文档插入 ok")

    -- 命令没等到应答（_rdo）：锁内就地关连接（established 清零、代次前进），之后 ping 能重连。
    -- 集合里只有一条，$where 只睡一次；读超时有 1s 粒度（最坏多等 1s），睡足 2 秒
    local oldnr = srey.get_netread_timeout()
    srey.set_netread_timeout(300)
    local g = mg.generation
    local wq = bson.encode({ ["$where"] = "sleep(2000) || true" })
    local wqptr, wqsz = wq:data()
    local slow = mg:find(CCOL, wqptr, wqsz)
    srey.set_netread_timeout(oldnr)
    t:eq(nil, slow, "没等到应答 find 返回 nil")
    t:eq(false, mg.established, "没等到应答即锁内关连接")
    t:check(mg.generation ~= g, "没等到应答代次前进")
    t:check(mg:ping(), "无应答关连接后 ping 重连")

    local c2 = bson.encode({ { id = 2, grp = "a", v = 2 }, { id = 3, grp = "b", v = 3 } })
    local c2ptr, c2sz = c2:data()
    t:check(mg:insert(CCOL, c2ptr, c2sz), "cmds: 其余文档插入 ok")
    -- aggregate
    local pl = bson.encode({ { ["$match"] = { grp = "a" } }, { ["$count"] = "n" } })
    local plptr, plsz = pl:data()
    local ar = _resp(mg:aggregate(CCOL, plptr, plsz))
    local ab = ar and ar.cursor and ar.cursor.firstBatch
    t:eq(2, ab and ab[1] and ab[1].n, "aggregate $match + $count")
    -- distinct
    local dr = _resp(mg:distinct(CCOL, "grp"))
    local dv = dr and dr.values or {}
    table.sort(dv)
    t:check(2 == #dv and "a" == dv[1] and "b" == dv[2], "distinct grp 为 a / b")
    -- findandmodify：new=true 回的是改后的文档
    local fq = bson.encode({ id = 1 })
    local fqptr, fqsz = fq:data()
    local fu = bson.encode({ ["$set"] = { v = 10 } })
    local fuptr, fusz = fu:data()
    local fo = bson.encode({ new = true })
    local foptr, fosz = fo:data()
    local fr = _resp(mg:findandmodify(CCOL, fqptr, fqsz, 0, 0, fuptr, fusz, foptr, fosz))
    t:eq(10, fr and fr.value and fr.value.v, "findandmodify 回改后的文档")
    -- find 开游标（每批 1 条）→ getmore 取完剩下的
    local bo = bson.encode({ batchSize = 1 })
    local boptr, bosz = bo:data()
    local gr = _resp(mg:find(CCOL, nil, nil, boptr, bosz))
    -- cursor.id 是 int64，bson.decode 解成 _bson_int64 包装对象，:val() 取整数
    local cw = gr and gr.cursor and gr.cursor.id
    local cid = cw and cw:val() or 0
    t:check(0 ~= cid and 1 == #gr.cursor.firstBatch, "find batchSize=1 开出游标")
    local mr = _resp(mg:getmore(CCOL, cid))
    local nb = mr and mr.cursor and mr.cursor.nextBatch
    t:eq(2, nb and #nb, "getmore 取回剩下 2 条")
    -- killcursors：再开一个游标，关掉它
    local kr = _resp(mg:find(CCOL, nil, nil, boptr, bosz))
    local kw = kr and kr.cursor and kr.cursor.id
    if t:check(kw and 0 ~= kw:val(), "killcursors 用的游标开出来了") then
        local kc = bson.encode({ kw })-- 包装对象原样按 int64 编回去
        local kcptr, kcsz = kc:data()
        t:check(mg:killcursors(CCOL, kcptr, kcsz), "killcursors ok")
    end
    -- createindexes / dropindexes
    local ix = bson.encode({ { key = { grp = 1 }, name = "srey_grp_idx" } })
    local ixptr, ixsz = ix:data()
    t:check(mg:createindexes(CCOL, ixptr, ixsz), "createindexes ok")
    local dx = bson.encode({ "srey_grp_idx" })
    local dxptr, dxsz = dx:data()
    t:check(mg:dropindexes(CCOL, dxptr, dxsz), "dropindexes ok")
    -- bulkwrite（MongoDB 8.0+，须在 admin 库上发）。op 文档靠首字段名区分种类，Lua 表键序不定，用构建器保序
    local ob = bson.new()
    ob:doc_begin("0")
    ob:int32("insert", 0)
    ob:doc_begin("document")
    ob:int32("id", 9)
    ob:utf8("grp", "bw")
    ob["end"](ob)
    ob["end"](ob)
    ob["end"](ob)
    local obptr, obsz = ob:data()
    local ns = bson.encode({ { ns = "test." .. CCOL } })
    local nsptr, nssz = ns:data()
    mg:db("admin")
    local br = _resp(mg:bulkwrite(obptr, obsz, nsptr, nssz))
    mg:db("test")
    -- 7.0 以前回 59(CommandNotFound)；7.x 命令在但关在 feature flag 后面，回 115(CommandNotSupported)
    if br and (59 == br.code or 115 == br.code) then
        printd("[%s] bulkWrite needs MongoDB 8.0+, case skipped (code %d)", t.name, br.code)
    else
        t:check(br and 1 == br.ok and 1 == br.nInserted, "bulkwrite 插入 1 条: " .. tostring(br and br.errmsg))
    end
    t:check(mg:drop(CCOL), "cmds: drop")

    do
        -- 活连接上丢掉整个对象再 GC：__gc 关连接、放手 Lua 份额，最后一份由网络线程 udfree 释放。
        -- 弱表确认句柄真被回收；留着的 sk 只是快照，不自己发关闭、只等 CLOSE，证明是 __gc 关的
        local gmg = mongo.new(DB_HOST, 27017, SSL_NAME.NONE,
                              "test", "admin", "12345678", "admin", "SCRAM-SHA-256")
        if not gmg:connect() then
            t:fail("gc: mongo connect failed")
        else
            local gsk = gmg.mongo:sock_id()
            local weak = setmetatable({ gmg.mongo }, { __mode = "v" })
            gmg = nil
            collectgarbage()
            collectgarbage()
            t:eq(nil, weak[1], "gc: 活连接上的 mongo 句柄已被回收")
            -- collectgarbage 到这里之间不让出，CLOSE 不会在登记等待前就被派发掉
            local cm = srey._coro_wait(gsk.skid, srey.MSG_TYPE.CLOSE, 3000)
            t:eq(srey.MSG_TYPE.CLOSE, cm and cm.mtype, "gc: __gc 自己关掉了连接")
            t:check(mg:ping(), "gc: 回收别的句柄后主连接照常可用")
        end
    end

    mg:quit()
end)
end)
