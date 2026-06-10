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
    t:check(mctx:query("delete from test_bind"), "delete test_bind")

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
        if not mctx:query(sql, bind) then
            insert_ok = false
            break
        end
    end
    t:check(insert_ok, "bulk insert 3 rows")

    -- 普通 SELECT
    local reader = mctx:query("select * from test_bind order by t_int8")
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
        local r2 = stmt:execute(bind)
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
    t:check(mctx:query("select 1"), "mysql query after reconnect")

    mctx:quit()
end)
end)
