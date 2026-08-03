-- PostgreSQL 集成测试：依赖 docker-compose 已起 postgres 容器（admin/12345678/test）

local srey   = require("lib.srey")
local runner = require("test.runner")
local pgsql  = require("lib.pgsql")
local pbind  = require("pgsql.bind")

local function _count_rows(reader)
    local cnt = 0
    while not reader:eof() do
        cnt = cnt + 1
        reader:next()
    end
    return cnt
end

srey.startup(function()
runner.run("db_pgsql", function(t)
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

    -- SELECT + reader
    local reader = pg:query("select id, name, score from srey_test order by id")
    if reader then
        t:eq(2, _count_rows(reader), "select 2 rows")
    else
        t:fail("pgsql select reader nil")
    end

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
    local fmt = pg:copy_in_begin("copy srey_test (id, name, score) from stdin")
    if fmt then
        -- 逐块检查返回值：服务端对少收几块无感知，某块被丢弃后 copy_in_done 照样回 CommandComplete，
        -- 不检查的话一次残缺的 COPY 会被报成成功
        t:check(pg:copy_in_data(data, #data), "copy_in_data 返 true")
        t:check(pg:copy_in_done(), "copy_in_done")
        -- data 为 nil 属调用方错误，须返 false 且 erro() 说明原因
        t:check(not pg:copy_in_data(nil), "copy_in_data(nil) 返 false")
        t:check(pg:erro() ~= "", "copy_in_data(nil) 写了 err")
    else
        t:fail("copy_in_begin")
    end

    -- COPY IN 中止：CopyFail 的正常应答就是 ErrorResponse，所以"中止成功"也走 ERR 包。
    -- 服务端文本走第二返回值而不写 err —— 写了的话 erro() 会把一次正常中止报成失败，
    -- 成为本文件"err 非空 == 上一次操作失败"这条读法的唯一例外
    if pg:copy_in_begin("copy srey_test (id, name, score) from stdin") then
        local aok, areason = pg:copy_in_abort("aborted by test")
        t:check(aok, "copy_in_abort 返 true")
        t:check(areason and #areason > 0, "服务端 ErrorResponse 文本走第二返回值")
        t:eq("", pg:erro(), "成功路径不写 err")
        -- 中止后连接须仍可用（CopyFail 之后服务端会回 ReadyForQuery）
        t:check(pg:query("select 1"), "abort 后连接仍可用")
    else
        t:fail("copy_in_begin (abort path)")
    end

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

    pg:quit()
end)
end)
