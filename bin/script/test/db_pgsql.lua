-- PostgreSQL 集成测试：依赖 docker-compose 已起 postgres 容器（admin/12345678/test）

local srey   = require("lib.srey")
local runner = require("test.runner")
local pgsql  = require("lib.pgsql")
local pbind  = require("srey.pgsql.bind")

local function _count_rows(reader)
    local cnt = 0
    while not reader:eof() do
        cnt = cnt + 1
        reader:next()
    end
    return cnt
end

srey.startup(function()
runner.run(function(t)
    local pg = pgsql.new("127.0.0.1", 5432, SSL_NAME.NONE,
                         "admin", "12345678", "test")
    if not pg:connect() then
        t:fail("pgsql connect")
        return
    end
    t:check(pg:ping(), "pgsql ping")

    -- 重建测试表
    t:check(pg:query("drop table if exists srey_test"),
            "drop table")
    t:check(pg:query("create table srey_test (id int primary key, name text not null, score double precision)"),
            "create table")

    -- 普通 INSERT
    t:check(pg:query("insert into srey_test (id, name, score) values (1, 'alice', 90.5), (2, 'bob', 75.0)"),
            "insert 2 rows")
    t:check(pg:affected_rows() >= 0, "affected_rows non-negative")

    -- SELECT + reader（单语句：数组恰好一个元素）
    local rs = pg:query("select id, name, score from srey_test order by id")
    if rs and "userdata" == type(rs[1]) then
        t:eq(1, #rs, "single statement yields 1 result")
        t:eq(2, _count_rows(rs[1]), "select 2 rows")
    else
        t:fail("pgsql select reader nil")
    end

    -- 多语句 simple query：每条语句一个结果，有结果集给 reader，无结果集给影响行数
    local multi = pg:query("select 1 as a; select 2 as b")
    if multi and 2 == #multi then
        local okA, va = multi[1]:integer("a")
        local okB, vb = multi[2]:integer("b")
        t:check(okA and 1 == va and okB and 2 == vb, "multi-statement: two result sets not crossed")
    else
        t:fail("pgsql multi-statement expected 2 results")
    end
    -- 混合：INSERT 无结果集给整数行数，SELECT 给 reader
    local mix = pg:query("insert into srey_test (id, name, score) values (100, 'multi', 1.0);"
                         .. " select name from srey_test where id = 100")
    if mix and 2 == #mix then
        t:eq(1, mix[1], "multi-statement: insert element is affected rows")
        t:check("userdata" == type(mix[2]), "multi-statement: select element is reader")
        local okN, ptr, nlen = mix[2]:text("name")
        t:check(okN and "multi" == srey.ud_str(ptr, nlen), "multi-statement: select value correct")
    else
        t:fail("pgsql insert+select expected 2 results")
    end
    -- 两条写语句：前一条的影响行数不被后一条覆盖
    local wr = pg:query("insert into srey_test (id, name, score) values (101, 'multi2', 2.0);"
                        .. " update srey_test set score = 9.0 where id in (100, 101)")
    if wr and 2 == #wr then
        t:check(1 == wr[1] and 2 == wr[2], "multi-statement: per-statement affected rows kept")
        t:eq(2, pg:affected_rows(), "affected_rows() still reports the last statement")
    else
        t:fail("pgsql insert+update expected 2 results")
    end
    -- 隐式单事务：第二条主键冲突则整体回滚，query 报失败且首条不生效
    t:check(not pg:query("insert into srey_test (id, name, score) values (102, 'gone', 0);"
                         .. " insert into srey_test (id, name, score) values (1, 'dup', 0)"),
            "multi-statement: duplicate key fails the whole query")
    local back = pg:query("select name from srey_test where id = 102")
    t:check(back and "userdata" == type(back[1]) and 0 == back[1]:size(),
            "multi-statement: first insert rolled back")

    -- 预处理 + 执行
    local stmt = pg:prepare("stmt_sel", "select name from srey_test where id = $1", 1, { 23 })
    if stmt then
        local bind = pbind.new(1)
        bind:int32(1)
        local r2 = stmt:execute(bind)
        if r2 then
            t:eq(1, _count_rows(r2), "stmt execute one row")
        else
            t:fail("pgsql stmt execute reader nil")
        end
        stmt:close()
    else
        t:fail("pgsql prepare nil")
    end

    -- COPY IN
    local data = "10\tcharlie\t60.0\n11\tdiana\t85.5\n12\teric\t95.25\n"
    -- copy_in 已折叠为一个方法：producer 返回数据块，返回 nil 收尾
    local sent = false
    local gotncol
    t:check(pg:copy_in("copy srey_test (id, name, score) from stdin", function(_, ncol)
        gotncol = ncol
        if sent then
            return nil
        end
        sent = true
        return data, #data
    end), "copy_in 正常完成")
    t:eq(3, gotncol, "producer 收到服务端 CopyInResponse 的列数")
    -- producer 抛错：库须替调用方发 CopyFail 把服务端拉出 COPY IN 模式，连接仍可用
    t:check(not pg:copy_in("copy srey_test (id, name, score) from stdin", function()
        error("producer boom")
    end), "producer 抛错返 false")
    t:check(pg:erro() ~= "", "producer 抛错写了 err")
    t:check(pg:query("select 1"), "producer 抛错后连接仍可用")
    -- producer 不是函数属调用方错误
    t:check(not pg:copy_in("copy srey_test (id, name, score) from stdin", nil),
            "producer 非函数返 false")
    t:check(pg:erro() ~= "", "producer 非函数写了 err")
    -- producer 返回的块类型不对：抛点在组包/发送而不在 producer 内，同样要走 CopyFail
    t:check(not pg:copy_in("copy srey_test (id, name, score) from stdin", function()
        return 42
    end), "producer 返回非法块返 false")
    t:check(pg:erro() ~= "", "非法块写了 err")
    t:check(pg:query("select 1"), "非法块后连接仍可用")

    -- COPY IN 中止：CopyFail 的正常应答就是 ErrorResponse，所以"中止成功"也走 ERR 包。
    -- 服务端文本走第二返回值而不写 err —— 写了的话 erro() 会把一次正常中止报成失败，
    -- 成为本文件"err 非空 == 上一次操作失败"这条读法的唯一例外
    local aok, areason = pg:copy_in("copy srey_test (id, name, score) from stdin", function()
        return false, "aborted by test"-- producer 返 (false, reason) 即主动中止
    end)
    t:check(aok, "copy_in 主动中止返 true")
    t:check(areason and #areason > 0, "服务端 ErrorResponse 文本走第二返回值")
    t:eq("", pg:erro(), "成功路径不写 err")
    -- 中止后连接须仍可用（CopyFail 之后服务端会回 ReadyForQuery）
    t:check(pg:query("select 1"), "abort 后连接仍可用")

    -- reason 不是字符串：pack_copy_fail 内部是 luaL_checkstring，不转换就在发出 CopyFail
    -- 之前抛出，而这条抛出在 pcall 作用域之外，会一路掠到 serial 的 xpcall——
    -- 服务端从此停在 COPY IN 模式攥着开放事务和表锁，err 还是空的。
    -- 所以这里真正要验的是最后那条"连接仍可用"
    local tok = pg:copy_in("copy srey_test (id, name, score) from stdin", function()
        return false, { code = 5 }
    end)
    t:check(tok, "reason 为 table 时中止照常完成")
    t:check(pg:query("select 1"), "table reason 中止后连接仍可用(服务端未卡在 COPY IN)")
    local bok = pg:copy_in("copy srey_test (id, name, score) from stdin", function()
        return false, true
    end)
    t:check(bok, "reason 为 boolean 时中止照常完成")
    t:check(pg:query("select 1"), "boolean reason 中止后连接仍可用")

    -- COPY OUT
    local out, outlen = pg:copy_out("copy srey_test to stdout")
    if out then
        t:check(outlen > 0, "copy_out outlen > 0")
    else
        t:fail("copy_out")
    end

    -- cancel：空闲连接上发 CancelRequest 为无害 no-op，验证独立连接发送路径打通
    t:check(pg:cancel(), "cancel smoke")
    t:check(pg:ping(), "ping after cancel")

    -- selectdb：quit + 切库 + 重连（此处切回同库 test），重连后连接仍可用
    t:check(pg:selectdb("test"), "selectdb reconnect")
    t:check(pg:ping(), "ping after selectdb")

    -- 库名超长：校验在 quit 之前，故必须直接返 false 且不动现有连接（不白断一条可用连接）
    t:check(not pg:selectdb(string.rep("d", 64)), "selectdb 超长库名返 false")
    t:check(pg:get_db() == "test", "selectdb 失败后库名不变")
    t:check(pg:ping(), "selectdb 失败后原连接仍可用")

    -- ping 自动重连：quit 关闭连接后 ping 应检测到死连接并重连
    pg:quit()
    t:check(pg:ping(), "pgsql ping auto-reconnect after quit")

    -- 并发：多协程同一条连接各查自己的常量，回读必须原样。没有串行化时命令交错，
    -- 一条 pgsql 命令要读到 ReadyForQuery 才算完，交错会让响应对错协程
    local N, ROUNDS = 4, 6
    local got, done = {}, 0
    for i = 1, N do
        srey.fork(function()
            local want = 1000 + i
            for _ = 1, ROUNDS do
                local qrs = pg:query(string.format("select %d as v", want))
                if not qrs or "userdata" ~= type(qrs[1]) then
                    got[i] = "query failed"
                    done = done + 1
                    return
                end
                local rok, v = qrs[1]:integer("v")
                if not rok or v ~= want then
                    got[i] = string.format("got %s want %d", tostring(v), want)
                    done = done + 1
                    return
                end
            end
            got[i] = true
            done = done + 1
        end)
    end
    -- 有界等待，理由同 db_mysql.lua：无界 while 会把 fork 协程抛错变成整份汇总挂住
    for _ = 1, 1500 do            -- 1500 x 20ms = 30s 上限
        if done >= N then break end
        srey.sleep(20)
    end
    t:eq(N, done, "并发协程全部完成 (" .. done .. "/" .. N .. ")")
    for i = 1, N do
        t:check(true == got[i], "pgsql 并发协程 " .. i .. ": " .. tostring(got[i]))
    end

    pg:quit()
end)
end)
