-- MySQL 集成测试：依赖 docker-compose 已起 mysql 容器（admin/12345678/test + test_bind 表）

local srey   = require("lib.srey")
local runner = require("test.runner")
local mysql  = require("lib.mysql")
local mbind  = require("srey.mysql.bind")

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

    -- 普通 SELECT：逐字段读回来比对，而不是只数行数。原来只有 _count_rows(reader) == 3，
    -- 于是 bind 的十种类型映射（double 恒写 0、string 截成空串、datetime 写错 epoch、
    -- time 的时分秒顺序弄反）只要行数还是 3 就全看不出来
    local rsel = mctx:query("select * from test_bind order by t_int8")
    local reader = rsel and rsel[1]
    if reader then
        local n = 0
        while not reader:eof() do
            n = n + 1
            local ok8, v8 = reader:integer("t_int8")
            local ok16, v16 = reader:integer("t_int16")
            local ok32, v32 = reader:integer("t_int32")
            local ok64, v64 = reader:integer("t_int64")
            local okd, vd = reader:double("t_double")
            local oks, sptr, slen = reader:string("t_string")
            t:check(ok8 and ok16 and ok32 and ok64 and okd and oks,
                    "row " .. n .. " 各列都读得出来")
            t:eq(n, v8, "row " .. n .. " t_int8")
            t:eq(100 + n, v16, "row " .. n .. " t_int16")
            t:eq(1000 + n, v32, "row " .. n .. " t_int32")
            t:eq(100000 + n, v64, "row " .. n .. " t_int64")
            -- 经 query attribute 的文本通道往返，double 留一点容差
            t:check(okd and vd and math.abs(vd - (3.14 + n)) < 1e-9,
                    "row " .. n .. " t_double (" .. tostring(vd) .. ")")
            t:eq("srey-mysql-test", oks and srey.ud_str(sptr, slen) or nil,
                 "row " .. n .. " t_string")
            reader:next()
        end
        t:eq(3, n, "select all rows")
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
        -- 显式关句柄：不发 COM_STMT_CLOSE 的话服务端那份要留到连接关闭才回收
        t:check(stmt:close(), "stmt close sends COM_STMT_CLOSE")
        t:eq(false, stmt:close(), "stmt close 幂等（已关闭返回 false）")
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
        t:check(cstmt:close(), "CALL stmt close sends COM_STMT_CLOSE")
    else
        t:fail("prepare call srey_multi nil")
    end
    mctx:query("drop procedure if exists srey_multi")
    -- 紧接普通查询验证 CALL 多结果集已收干净、无残留错位
    local rcafter = mctx:query("select 7 as v")
    t:check(rcafter and 1 == #rcafter and rcafter[1], "single query after CALL multi-result: no misalignment")

    -- 并发：多协程同一条连接各查自己的常量，回读必须原样。没有串行化时命令交错，
    -- MySQL 半双工加上每连接一份的解析状态，表现为串号、少行乃至解析崩掉
    local N, ROUNDS = 4, 6
    local got, done = {}, 0
    for i = 1, N do
        srey.fork(function()
            local want = 1000 + i
            for _ = 1, ROUNDS do
                local r = mctx:query(string.format("select %d as v", want))
                if not (r and 1 == #r and r[1]) then
                    got[i] = "query failed"
                    done = done + 1
                    return
                end
                local rok, v = r[1]:integer("v")
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
    -- 有界等待。fork 出去的协程抛错时 done 永远到不了 N（srey.fork 的错误由 _coro_cb 的
    -- xpcall 吞掉、只打 ERROR 日志、不向外传播），无界 while 会让本 task 的 runner.run
    -- 永远走不到 t:done()，整份汇总要么不出现要么靠超时兜底报 MISS，真原因只在日志另一处
    for _ = 1, 1500 do            -- 1500 x 20ms = 30s 上限
        if done >= N then break end
        srey.sleep(20)
    end
    t:eq(N, done, "并发协程全部完成 (" .. done .. "/" .. N .. ")")
    for i = 1, N do
        t:check(true == got[i], "mysql 并发协程 " .. i .. ": " .. tostring(got[i]))
    end

    mctx:quit()
end)
end)
