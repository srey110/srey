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
        pg:copy_in_data(data, #data)
        t:check(pg:copy_in_done(), "copy_in_done")
    else
        t:fail("copy_in_begin")
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

    -- ping 自动重连：quit 关闭连接后 ping 应检测到死连接并重连
    pg:quit()
    t:check(pg:ping(), "pgsql ping auto-reconnect after quit")

    pg:quit()
end)
end)
