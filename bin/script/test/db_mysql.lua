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

-- reader:get / reader:integer 的两个边角，query(文本协议)与 stmt(二进制协议)各验一遍：
-- 字面量 NULL 列(MYSQL_TYPE_NULL，不在任何取值分组)给 nil 不抛错；UNSIGNED 整数列按无符号读，
-- 二进制路上 200 / 3000000000 不能读成负数；超出 int64 的值 get 抛错且错误信息带列名，integer 按读取失败返回 false
local function _check_edge(t, label, rd)
    if not t:check(rd and not rd:eof(), label .. " 有一行") then
        return
    end
    local gok, tiu, iu, bmax, cu, lit, n = pcall(rd.get, rd, "tiu", "iu", "bmax", "cu", "lit", "n")
    t:check(gok, label .. " get 不抛错: " .. tostring(tiu))
    if gok then
        t:eq(200, tiu, label .. " get TINYINT UNSIGNED 200")
        t:eq(3000000000, iu, label .. " get INT UNSIGNED 3000000000")
        t:eq(math.maxinteger, bmax, label .. " get BIGINT UNSIGNED INT64_MAX")
        t:eq(200, cu, label .. " get CAST(200 AS UNSIGNED)")
        t:eq(nil, lit, label .. " get 字面量 NULL 列是 nil")
        t:eq(nil, n, label .. " get INT NULL 是 nil")
    end
    local ok, v = rd:integer("tiu")
    t:check(ok and 200 == v, label .. " integer TINYINT UNSIGNED: " .. tostring(v))
    ok, v = rd:integer("iu")
    t:check(ok and 3000000000 == v, label .. " integer INT UNSIGNED: " .. tostring(v))
    ok, v = rd:integer("lit")
    t:check(ok and nil == v, label .. " integer 字面量 NULL 列是成功 + 无值")
    local eok, emsg = pcall(rd.get, rd, "bover")
    t:check(not eok and string.find(tostring(emsg), "'bover' unsigned value exceeds int64", 1, true),
            label .. " get 超 int64 抛错: " .. tostring(emsg))
    eok, emsg = pcall(rd.integer, rd, "bover")
    t:check(eok and false == emsg, label .. " integer 超 int64 返回 false、不抛错: " .. tostring(emsg))
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
    local dts = {}-- 每行写进 t_datetime 的秒级时间戳，回读时按行比对
    for i = 1, 3 do
        bind:clear()
        bind:integer("t_int8", i)
        bind:integer("t_int16", 100 + i)
        bind:integer("t_int32", 1000 + i)
        bind:integer("t_int64", 100000 + i)
        bind:double("t_float", 1.5 + i)
        bind:double("t_double", 3.14 + i)
        bind:string("t_string", "srey-mysql-test")
        dts[i] = os.time()
        bind:datetime("t_datetime", dts[i])
        bind:time("t_time", 0, 0, 1, 30, 0)
        bind:null("t_nil")
        local rins = mctx:query(sql, bind)
        if not rins or false == rins[1] then
            insert_ok = false
            break
        end
    end
    t:check(insert_ok, "bulk insert 3 rows")
    -- last_id / affectd_rows 是业务判"插了几行 / 自增主键是多少"的唯一入口,返错值不会让
    -- 任何断言变红。单行 INSERT 的影响行数恒为 1;表有自增主键,last_id 必 > 0
    t:eq(1, mctx:affectd_rows(), "单行 INSERT 的 affectd_rows 是 1")
    t:check((mctx:last_id() or 0) > 0, "自增主键的 last_id > 0")

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
            -- 剩下四列也得读回来：注释点名要拦的 datetime / time 走的正是它们
            local okf, vf = reader:float("t_float")
            t:check(okf and vf and math.abs(vf - (1.5 + n)) < 1e-4,
                    "row " .. n .. " t_float (" .. tostring(vf) .. ")")
            -- DATETIME 是秒精度，回来的是微秒时间戳
            local okdt, vdt = reader:datetime("t_datetime")
            t:eq(dts[n] * 1000000, okdt and vdt or nil, "row " .. n .. " t_datetime")
            -- 写进去的是 (is_negative=0, days=0, hour=1, minute=30, second=0)，
            -- 时分秒顺序弄反的话这里会读成 0:1:30 或 30:1:0
            local okt, tneg, tday, thour, tmin, tsec = reader:time("t_time")
            t:check(okt, "row " .. n .. " t_time 读得出来")
            t:eq(0, tneg and 1 or 0, "row " .. n .. " t_time 非负")
            t:eq(0, tday, "row " .. n .. " t_time days")
            t:eq(1, thour, "row " .. n .. " t_time hour")
            t:eq(30, tmin, "row " .. n .. " t_time minute")
            t:eq(0, tsec, "row " .. n .. " t_time second")
            -- NULL 列：读取成功但不返回值
            local okn, vn = reader:integer("t_nil")
            t:check(okn, "row " .. n .. " t_nil 读取成功")
            t:eq(nil, vn, "row " .. n .. " t_nil 是 NULL，不带值")
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

    -- UNSIGNED 列与字面量 NULL 列：临时表只活在这条连接上，须放在下面的 quit 之前
    mctx:query("DROP TEMPORARY TABLE IF EXISTS srey_unsigned")
    local rtmp = mctx:query("CREATE TEMPORARY TABLE srey_unsigned (tiu TINYINT UNSIGNED, iu INT UNSIGNED,"
        .. " bmax BIGINT UNSIGNED, bover BIGINT UNSIGNED, n INT)")
    t:check(rtmp and true == rtmp[1], "create temp table srey_unsigned")
    rtmp = mctx:query("INSERT INTO srey_unsigned VALUES (200, 3000000000, 9223372036854775807, 9223372036854775808, NULL)")
    t:check(rtmp and true == rtmp[1], "insert srey_unsigned")
    local esql = "SELECT tiu, iu, bmax, bover, CAST(200 AS UNSIGNED) AS cu, NULL AS lit, n FROM srey_unsigned"
    local requery = mctx:query(esql)
    _check_edge(t, "query", requery and requery[1])
    local estmt = mctx:prepare(esql)
    if t:check(estmt, "prepare edge select") then
        local rexe = estmt:execute()
        _check_edge(t, "stmt", rexe and rexe[1])
        estmt:close()
    end
    -- 不带表的 SELECT NULL，query 与 stmt 各验一遍
    local function _check_select_null(label, rs)
        local nrd = rs and rs[1]
        if t:check(nrd, label .. " SELECT NULL 有结果集") then
            local nok, nv, one = pcall(nrd.get, nrd, "n", "one")
            t:check(nok and nil == nv and 1 == one, label .. " SELECT NULL get: " .. tostring(nv))
        end
    end
    _check_select_null("query", mctx:query("SELECT NULL AS n, 1 AS one"))
    local nst = mctx:prepare("SELECT NULL AS n, 1 AS one")
    if t:check(nst, "prepare SELECT NULL") then
        _check_select_null("stmt", nst:execute())
        nst:close()
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
    -- 用 srey.fork_wait 而不是手写 done 计数 + 有界轮询:协程体抛错时 fork_wait 照样收敛
    -- (r[i].ok=false、r[i][1] 就是错误原文,且全部完成即刻返回);手写版是 done 到不了 N、
    -- 空烧满 1500x20ms=30s,真原因只在日志另一处
    local fns = {}
    for i = 1, N do
        fns[i] = function()
            local want = 1000 + i
            for _ = 1, ROUNDS do
                local r = mctx:query(string.format("select %d as v", want))
                if not (r and 1 == #r and r[1]) then
                    return "query failed"
                end
                local rok, v = r[1]:integer("v")
                if not rok or v ~= want then
                    return string.format("got %s want %d", tostring(v), want)
                end
            end
            return true
        end
    end
    local res = srey.fork_wait(fns)
    t:eq(N, #res, "fork_wait 收齐 N 项 (" .. #res .. "/" .. N .. ")")
    for i = 1, N do
        t:check(res[i].ok and true == res[i].val,
                "mysql 并发协程 " .. i .. ": " .. tostring(res[i].ok and res[i].val or res[i][1]))
    end

    mctx:quit()
end)
end)
