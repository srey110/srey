-- MySQL 集成测试：依赖 docker-compose 已起 mysql 容器（admin/12345678/test + test_bind 表）

local srey   = require("lib.srey")
local runner = require("test.runner")
local mysql  = require("lib.mysql")
local mbind  = require("mysql.bind")

local function _count_rows(reader)
    local cnt = 0
    while not reader:eof() do
        cnt = cnt + 1
        reader:next()
    end
    return cnt
end

srey.startup(function()
runner.run("db_mysql", function(t)
    local mctx = mysql.new("127.0.0.1", 3306, SSL_NAME.NONE,
                           "admin", "12345678", "test", "utf8mb4", 0)
    if not mctx:connect() then
        t:fail("mysql connect")
        return
    end
    t:check(mctx:version() ~= nil and #mctx:version() > 0, "mysql version")

    -- 切库 / ping
    t:check(mctx:selectdb("test"), "selectdb test")
    t:check(mctx:ping(),            "mysql ping")

    -- 清表
    local rdel = mctx:query("delete from test_bind")
    t:check(rdel and true == rdel[1], "delete test_bind")

    -- 通过 query attribute 批量插入 3 行
    local bind = mbind.new()
    local sql = "insert into test_bind"
        .. " (t_int8,t_int16,t_int32,t_int64,t_float,t_double,t_string,t_datetime,t_time,t_nil)"
        .. " values("
        .. "mysql_query_attribute_string('t_int8'),"
        .. "mysql_query_attribute_string('t_int16'),"
        .. "mysql_query_attribute_string('t_int32'),"
        .. "mysql_query_attribute_string('t_int64'),"
        .. "mysql_query_attribute_string('t_float'),"
        .. "mysql_query_attribute_string('t_double'),"
        .. "mysql_query_attribute_string('t_string'),"
        .. "mysql_query_attribute_string('t_datetime'),"
        .. "mysql_query_attribute_string('t_time'),"
        .. "mysql_query_attribute_string('t_nil'))"
    local insert_ok = true
    for i = 1, 3 do
        bind:clear()
        bind:integer("t_int8", i)
        bind:integer("t_int16", 100 + i)
        bind:integer("t_int32", 1000 + i)
        bind:integer("t_int64", 100000 + i)
        bind:double("t_float", 1.5 + i)
        bind:double("t_double", 3.14 + i)
        bind:string("t_string", "srey-mysql-test")
        bind:datetime("t_datetime", os.time())
        bind:time("t_time", 0, 0, 1, 30, 0)
        bind:null("t_nil")
        local rins = mctx:query(sql, bind)
        if not rins or false == rins[1] then
            insert_ok = false
            break
        end
    end
    t:check(insert_ok, "bulk insert 3 rows")

    -- 普通 SELECT
    local rsel = mctx:query("select * from test_bind order by t_int8")
    local reader = rsel and rsel[1]
    if reader then
        t:eq(3, _count_rows(reader), "select all rows")
    else
        t:fail("select reader nil")
    end

    -- 预处理 + 执行
    local stmt = mctx:prepare("select t_int32 from test_bind where t_int8 = ?")
    if stmt then
        bind:clear()
        bind:integer(nil, 2)
        local rexe = stmt:execute(bind)
        local r2 = rexe and rexe[1]
        if r2 then
            t:eq(1, _count_rows(r2), "stmt execute one row")
        else
            t:fail("stmt execute reader nil")
        end
    else
        t:fail("mysql prepare nil")
    end

    -- ping 自动重连：quit 关闭连接后 ping 应检测到死连接并重连
    mctx:quit()
    t:check(mctx:ping(), "mysql ping auto-reconnect after quit")
    local rrc = mctx:query("select 1")
    t:check(rrc and rrc[1], "mysql query after reconnect")

    -- 多语句多结果集（P1）：select 1;select 2 → 两个独立结果集
    local rmulti = mctx:query("select 1 as a;select 2 as b")
    t:check(rmulti and 2 == #rmulti, "multi-statement returns 2 result sets")
    -- 紧接普通查询验证多结果集已收干净、无残留错位：应恰好 1 个结果集
    local rafter = mctx:query("select 42 as v")
    t:check(rafter and 1 == #rafter and rafter[1], "single query after multi-result: no misalignment")

    -- 预处理 CALL 存储过程多结果集（P1，二进制协议）：CALL 内 2 个 SELECT → execute 续接循环收齐 2 结果集 + CALL 完成 OK 包
    mctx:query("drop procedure if exists srey_multi")
    local rproc = mctx:query("create procedure srey_multi() begin select 1 as a; select 2 as b; end")
    t:check(rproc and true == rproc[1], "create procedure srey_multi")
    local cstmt = mctx:prepare("call srey_multi()")
    if cstmt then
        local rcall = cstmt:execute()
        t:check(rcall and 3 == #rcall, "stmt CALL returns 3 packs (2 resultsets + trailing OK)")
        if rcall and 3 == #rcall then
            t:eq(1, _count_rows(rcall[1]), "CALL result set 1 one row")
            t:eq(1, _count_rows(rcall[2]), "CALL result set 2 one row")
            t:check(true == rcall[3], "CALL trailing OK packet")
        end
    else
        t:fail("prepare call srey_multi nil")
    end
    mctx:query("drop procedure if exists srey_multi")
    -- 紧接普通查询验证 CALL 多结果集已收干净、无残留错位
    local rcafter = mctx:query("select 7 as v")
    t:check(rcafter and 1 == #rcafter and rcafter[1], "single query after CALL multi-result: no misalignment")

    mctx:quit()
end)
end)
