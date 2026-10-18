-- MySQL 集成测试：依赖 docker-compose 已起 mysql 容器（admin/12345678/test + test_bind 表）

local srey   = require("lib.srey")
local runner = require("test.runner")
local mysql  = require("lib.mysql")
local mbind  = require("srey.mysql.bind")
local cmysql = require("srey.mysql")-- C 绑定，按包取计数与错误
local cstmt  = require("srey.mysql.stmt")
local core   = require("srey.core")

-- 数据库地址：在 VM 上跑时改成宿主机 IP，只改这一处
local DB_HOST = "127.0.0.1"
-- 不带客户端证书、不校验服务端的 TLS 上下文(同 C 侧 clientnull)：MySQL 会向客户端要证书，
-- 带上 SSL_NAME.CLIENT 那张项目自签证书会被容器以 unknown ca 拒掉
local SSL_NOCERT = "db_mysql_nocert"

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
-- 二进制路上 200 / 3000000000 不能读成负数；超出 int64 的值 get 抛错且错误信息带列名，integer 按读取失败返回 false；
-- 列不存在（含名字带结尾 '\0'，按整串比查不到）时 get 抛错、integer 返回 false
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
    eok, emsg = pcall(rd.get, rd, "nosuch")
    t:check(not eok and string.find(tostring(emsg), "'nosuch'", 1, true),
            label .. " get 不存在的列抛错且带列名: " .. tostring(emsg))
    eok, emsg = pcall(rd.integer, rd, "nosuch")
    t:check(eok and false == emsg, label .. " integer 不存在的列返回 false、不抛错: " .. tostring(emsg))
    eok = pcall(rd.get, rd, "tiu\0")
    t:check(not eok, label .. " get 名字带结尾 \\0 按列不存在抛错")
    eok, emsg = pcall(rd.integer, rd, "tiu\0")
    t:check(eok and false == emsg, label .. " integer 名字带结尾 \\0 返回 false: " .. tostring(emsg))
end

-- 先走 TLS 连一次并查一条：caching_sha2_password 账号在服务端缓存未命中（如容器刚起）时只在 TLS 上做完整认证，
-- 明文连接直接认证失败；TLS 上认证成功后服务端缓存住，随后的明文连接才能走快速认证。不带 SSL 的构建跳过
local function _tls_case(t)
    if not core.with_ssl() then
        printd("[%s] built without SSL, mysql tls case skipped", t.name)
        return
    end
    if not srey.ssl_qury(SSL_NOCERT)
        and nil == core.cert_register(SSL_NOCERT, "", "", "") then
        t:fail("注册不带证书的客户端 TLS 上下文")
        return
    end
    local ms = mysql.new(DB_HOST, 3306, SSL_NOCERT, "admin", "12345678", "test", "utf8mb4", 0)
    if not t:check(ms:connect(), "mysql TLS 连接并完成认证") then
        return
    end
    local rs = ms:query("select 1 as v")
    local vok, v = false, nil
    if rs and rs[1] then
        vok, v = rs[1]:integer("v")
    end
    t:check(vok and 1 == v, "mysql TLS 连接上查询: " .. tostring(v))
    ms:quit()
end

srey.startup(function()
runner.run(function(t)
    _tls_case(t)
    local mctx = mysql.new(DB_HOST, 3306, SSL_NAME.NONE,
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

    -- last_id 超 int64：显式写进 BIGINT UNSIGNED 自增列的值经 OK 包回带，超出 int64 时返回 false，不回绕成负数
    mctx:query("DROP TEMPORARY TABLE IF EXISTS srey_ai")
    local rai = mctx:query("CREATE TEMPORARY TABLE srey_ai (id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY)")
    t:check(rai and true == rai[1], "create temp table srey_ai")
    rai = mctx:query("INSERT INTO srey_ai VALUES (9223372036854775808)")
    t:check(rai and true == rai[1], "insert srey_ai 2^63")
    t:eq(false, mctx:last_id(), "last_id 超 int64 返回 false")
    t:eq(1, mctx:affectd_rows(), "同一 OK 包的 affectd_rows 照常")

    -- 空串回绑：reader:string 给的空串指针原样交给 bind:string 仍是空串，不能变成 SQL NULL
    local rempty = mctx:query("SELECT '' AS e")
    local erd = rempty and rempty[1]
    local nullst = mctx:prepare("SELECT ? IS NULL AS isn")
    if t:check(erd and nullst, "空串回绑：查询与 prepare") then
        local sok, eptr, elen = erd:string("e")
        t:check(sok and 0 == elen, "reader:string 空串长度 0")
        bind:clear()
        bind:string(nil, eptr, elen)
        local rn = nullst:execute(bind)
        local nrd = rn and rn[1]
        local iok, isn = false, nil
        if nrd then
            iok, isn = nrd:integer("isn")
        end
        t:check(iok and 0 == isn, "空串回绑后不是 SQL NULL: " .. tostring(isn))
        nullst:close()
    end

    -- stmt:reset 正向：执行一次后 reset，同一语句换参数照常执行
    local rst = mctx:prepare("select t_int32 from test_bind where t_int8 = ?")
    if t:check(rst, "prepare reset 用例") then
        bind:clear()
        bind:integer(nil, 2)
        local r1 = rst:execute(bind)
        t:check(r1 and r1[1], "reset 前 execute")
        t:check(rst:reset(), "stmt reset 成功")
        bind:clear()
        bind:integer(nil, 3)
        local r2 = rst:execute(bind)
        local rok, v = false, nil
        if r2 and r2[1] then
            rok, v = r2[1]:integer("t_int32")
        end
        t:check(rok and 1003 == v, "reset 后换参数 execute: " .. tostring(v))
    end

    -- ping 自动重连：quit 关闭连接后 ping 应检测到死连接并重连
    mctx:quit()
    t:check(mctx:ping(), "mysql ping auto-reconnect after quit")
    local rrc = mctx:query("select 1")
    t:check(rrc and rrc[1], "mysql query after reconnect")
    -- 上面的 quit + ping 让代次前进：旧 statement_id 服务端已清掉，stmt 各入口拒绝，close 不再发包
    if rst then
        t:eq(nil, rst:execute(bind), "重连后旧 stmt execute 返回 nil")
        t:eq(false, rst:reset(), "重连后旧 stmt reset 返回 false")
        t:check(rst:close(), "重连后旧 stmt close 返回 true")
    end

    -- 命令没等到应答：锁内就地关连接（established 清零、代次前进），之后 ping 能重连。
    -- 首包与多结果集续读两条路各走一遍；读超时有 1s 粒度（最坏多等 1s），SLEEP 给足 2 秒
    for _, c in ipairs({ { "首包", "select sleep(2)" }, { "续读", "select 1 as a; select sleep(2) as b" } }) do
        local oldnr = srey.get_netread_timeout()
        srey.set_netread_timeout(300)
        local g = mctx.generation
        local rsl = mctx:query(c[2])
        srey.set_netread_timeout(oldnr)
        t:eq(nil, rsl, c[1] .. "没等到应答 query 返回 nil")
        t:eq(false, mctx.established, c[1] .. "没等到应答即锁内关连接")
        t:check(mctx.generation ~= g, c[1] .. "没等到应答代次前进")
        t:check(mctx:ping(), c[1] .. "无应答关连接后 ping 重连")
        local rq = mctx:query("select 1")
        t:check(rq and rq[1], c[1] .. "重连后查询照常")
    end

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
    local callst = mctx:prepare("call srey_multi()")
    if callst then
        local rcall = callst:execute()
        t:check(rcall and 3 == #rcall, "stmt CALL returns 3 packs (2 resultsets + trailing OK)")
        if rcall and 3 == #rcall then
            t:eq(1, _count_rows(rcall[1]), "CALL result set 1 one row")
            t:eq(1, _count_rows(rcall[2]), "CALL result set 2 one row")
            t:check(true == rcall[3], "CALL trailing OK packet")
        end
        t:check(callst:close(), "CALL stmt close sends COM_STMT_CLOSE")
    else
        t:fail("prepare call srey_multi nil")
    end
    mctx:query("drop procedure if exists srey_multi")
    -- 紧接普通查询验证 CALL 多结果集已收干净、无残留错位
    local rcafter = mctx:query("select 7 as v")
    t:check(rcafter and 1 == #rcafter and rcafter[1], "single query after CALL multi-result: no misalignment")

    -- 多语句逐条计数：每条 INSERT 的影响行数与自增 ID 随各自的包交付（C 绑定 pack_* 按包取），
    -- query 返回后连接级 affectd_rows / last_id / erro 是最后一个 OK / ERR 包的。临时表只活在这条连接上，自增从 1 起
    mctx:query("DROP TEMPORARY TABLE IF EXISTS srey_counts")
    local rcnt = mctx:query("CREATE TEMPORARY TABLE srey_counts (id INT AUTO_INCREMENT PRIMARY KEY, v INT)")
    t:check(rcnt and true == rcnt[1], "create temp table srey_counts")
    local csql = "insert into srey_counts(v) values(1),(2);insert into srey_counts(v) values(3)"
    local cok, per = mctx.serial(function()
        local pk, ps = mctx.mysql:pack_query(csql)
        local mpk, sk = mctx:_send_wait(pk, ps)
        local out = {}
        while mpk do
            mctx.mysql:record(mpk)
            local _, more = cmysql.pack_type(mpk)
            local _, code = cmysql.pack_erro(mpk)
            out[#out + 1] = { cmysql.pack_affected_rows(mpk), cmysql.pack_last_id(mpk), code }
            if not more then
                return out
            end
            mpk = mctx:_recv_wait(sk)
        end
        return nil
    end)
    if t:check(cok and per and 2 == #per, "多语句逐条取到 2 个 OK 包") then
        t:eq(2, per[1][1], "第 1 条 affected 2")
        t:eq(1, per[1][2], "第 1 条 last_id 是这批的第一个")
        t:eq(0, per[1][3], "OK 包的错误号为 0")
        t:eq(1, per[2][1], "第 2 条 affected 1")
        t:eq(3, per[2][2], "第 2 条 last_id 3")
    end
    local rq = mctx:query(csql)
    t:check(rq and 2 == #rq and true == rq[1] and true == rq[2], "query 多语句 INSERT")
    t:eq(1, mctx:affectd_rows(), "连接级 affectd_rows 是最后一条的")
    t:eq(6, mctx:last_id(), "连接级 last_id 是最后一条的")
    local rdup = mctx:query("insert into srey_counts(v) values(7);insert into srey_counts(id, v) values(1, 8)")
    t:check(rdup and 2 == #rdup and true == rdup[1] and false == rdup[2], "多语句第 2 条主键冲突")
    local _, dcode = mctx:erro()
    t:eq(1062, dcode, "连接级 erro 是冲突那条的 1062")
    t:eq(7, mctx:last_id(), "冲突的 ERR 包不改 last_id")

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

    -- 第二条连接：stmt.new 的宿主核对、活连接上提前回收句柄、宿主回收后 stmt 各入口报错
    local m2 = mysql.new(DB_HOST, 3306, SSL_NAME.NONE, "admin", "12345678", "test", "utf8mb4", 0)
    if t:check(m2:connect(), "mysql 第二条连接") then
        -- m2 上 prepare 回来的包交给 mctx 的句柄：须报错，不能把 stmt 锚到错的宿主上
        local sok, serr = m2.serial(function()
            local pk, ps = m2.mysql:pack_stmt_prepare("SELECT 1")
            local mpk = srey.syn_send(m2.mysql:sock_id(), pk, ps, 0)
            if not mpk then
                return "prepare 无应答"
            end
            local nok, nerr = pcall(cstmt.new, mctx.mysql, mpk)
            return not nok and nerr
        end)
        t:check(sok and string.find(tostring(serr), "another mysql", 1, true),
                "stmt.new 宿主不是收包连接时报错: " .. tostring(serr))
        local st2 = m2:prepare("SELECT 1")
        if t:check(st2, "m2 prepare") then
            -- 活连接上提前回收：发 QUIT 并关连接，C 块由网络线程最后释放
            m2.mysql:__gc()
            t:eq(false, pcall(m2.mysql.sock_id, m2.mysql), "宿主 __gc 后自身入口报错")
            for _, fn in ipairs({ "pack_stmt_execute", "pack_stmt_reset", "pack_stmt_close", "sock_id" }) do
                local eok, emsg = pcall(st2.stmt[fn], st2.stmt)
                t:check(not eok and string.find(tostring(emsg), "owner mysql already freed", 1, true),
                        "宿主 __gc 后 stmt:" .. fn .. " 报错: " .. tostring(emsg))
            end
            -- 等网络线程 udfree 交还它那份，stmt 的 __gc 就是最后放手的一方，C 块由它释放
            srey.sleep(200)
            t:eq(true, pcall(st2.stmt.__gc, st2.stmt), "宿主关完后 stmt __gc 不抛错")
            t:eq(true, pcall(st2.stmt.__gc, st2.stmt), "stmt __gc 幂等")
        end
    end
    m2 = nil
    -- stmt 先释放、宿主照常可用：stmt 交还的只是自己那份引用
    local st4 = mctx:prepare("SELECT 1")
    if t:check(st4, "prepare st4") then
        t:check(st4:close(), "st4 close")
        st4.stmt:__gc()
    end
    st4 = nil
    collectgarbage()
    local r4 = mctx:query("select 1")
    t:check(r4 and r4[1], "stmt 先释放后宿主照常可用")
    -- 不显式 __gc：连着的句柄丢掉引用后由 GC 回收，走的是同一条路径
    do
        local m3 = mysql.new(DB_HOST, 3306, SSL_NAME.NONE, "admin", "12345678", "test", "utf8mb4", 0)
        t:check(m3:connect(), "mysql 第三条连接")
    end
    collectgarbage()
    collectgarbage()
    srey.sleep(200)-- 让两条连接的关闭在网络线程上走完
    local rgc = mctx:query("select 1")
    t:check(rgc and rgc[1], "别的连接在活着时被回收后，本连接照常可用")

    mctx:quit()
end)
end)
