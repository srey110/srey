-- PostgreSQL 集成测试：依赖 docker-compose 已起 postgres 容器（admin/12345678/test）

local srey   = require("lib.srey")
local runner = require("test.runner")
local pgsql  = require("lib.pgsql")
local pbind  = require("srey.pgsql.bind")
local cpg    = require("srey.pgsql")
local preader = require("srey.pgsql.reader")

-- 数据库地址：在 VM 上跑时改成宿主机 IP，只改这一处
local DB_HOST = "127.0.0.1"

-- reader:get 对拍用的列：{ 列名, 对应的单列取值方法 }；numeric 没有单列方法，get 对它必须抛错
local PG_COLS = {
    { "b", "bool" }, { "i2", "integer" }, { "i4", "integer" }, { "i8", "integer" },
    { "f4", "double" }, { "f8", "double" }, { "tx", "text" }, { "vc", "text" }, { "bc", "text" },
    { "nm", "text" }, { "by", "bytea" }, { "ts", "timestamp" }, { "tz", "timestamp" },
    { "dt", "date" }, { "u", "uuid" }, { "nu" },
}

local function _count_rows(reader)
    local cnt = 0
    while not reader:eof() do
        cnt = cnt + 1
        reader:next()
    end
    return cnt
end

-- 逐行逐列比 reader:get 与单列取值器（text / bytea 走 asstr），并核 asstr 与借用指针拷出来的一致。
-- 返回每行 { 列名 = get 的值 }，供文本 / 二进制两种格式互比。
-- 首行上另核列不存在（含名字带结尾 '\0'，按整串比查不到）时 get 抛错、integer 返回 false
local function _pg_get_vs_single(t, rd, label)
    local rows = {}
    if not rd:eof() then
        local eok, emsg = pcall(rd.get, rd, "nosuch")
        t:check(not eok and string.find(tostring(emsg), "'nosuch'", 1, true),
                label .. " get 不存在的列抛错且带列名: " .. tostring(emsg))
        eok, emsg = pcall(rd.integer, rd, "nosuch")
        t:check(eok and false == emsg, label .. " integer 不存在的列返回 false、不抛错: " .. tostring(emsg))
        eok = pcall(rd.get, rd, "i4\0")
        t:check(not eok, label .. " get 名字带结尾 \\0 按列不存在抛错")
        eok, emsg = pcall(rd.integer, rd, "i4\0")
        t:check(eok and false == emsg, label .. " integer 名字带结尾 \\0 返回 false: " .. tostring(emsg))
    end
    while not rd:eof() do
        local row = {}
        rows[#rows + 1] = row
        local tag = label .. " row" .. #rows .. " "
        for _, c in ipairs(PG_COLS) do
            local col, m = c[1], c[2]
            local gok, gv = pcall(rd.get, rd, col)
            if nil == m then
                t:eq(false, gok, tag .. col .. " 不支持的类型 get 抛错")
            else
                local sok, sv = rd[m](rd, col, true)
                if t:check(sok and gok, tag .. col .. " get 与 " .. m .. " 都读得出: " .. tostring(gv)) then
                    t:check(gv == sv and math.type(gv) == math.type(sv),
                            string.format("%s%s get=%s 与 %s=%s 一致", tag, col, tostring(gv), m, tostring(sv)))
                    row[col] = gv
                end
                if "text" == m or "bytea" == m then
                    local _, p, l = rd[m](rd, col)
                    t:eq(sv, p and srey.ud_str(p, l), tag .. col .. " asstr 与借用指针拷出的一致")
                end
            end
        end
        rd:next()
    end
    return rows
end

-- TLS：SSLRequest → 握手 → SCRAM（服务端开 TLS 时选 SCRAM-SHA-256-PLUS）→ 查一条。
-- 服务端没开 TLS（show ssl 为 off，SSLRequest 会回 'N'）或没注册客户端 SSL 上下文时跳过；pg 是明文主连接，拿来探测
local function _pg_tls_case(t, pg)
    local srs = pg:query("show ssl")
    local sok, sslset = false, nil
    if srs and "userdata" == type(srs[1]) then
        sok, sslset = srs[1]:text("ssl", true)
    end
    if not t:check(sok, "show ssl 读得出: " .. tostring(sslset)) then
        return
    end
    if "on" ~= sslset then
        printd("[%s] server ssl is off, pgsql tls case skipped", t.name)
        return
    end
    if not srey.ssl_qury(SSL_NAME.CLIENT) then
        printd("[%s] ssl context not registered, pgsql tls case skipped", t.name)
        return
    end
    local ps = pgsql.new(DB_HOST, 5432, SSL_NAME.CLIENT, "admin", "12345678", "test")
    if not t:check(ps:connect(), "pgsql TLS 连接并完成 SCRAM 认证") then
        return
    end
    local trs = ps:query("select ssl, system_user from pg_stat_ssl where pid = pg_backend_pid()")
    local bok, bv, uok, su = false, nil, false, nil
    if trs and "userdata" == type(trs[1]) then
        bok, bv = trs[1]:bool("ssl")
        uok, su = trs[1]:text("system_user", true)
    end
    t:check(bok and true == bv, "TLS 连接上服务端看到的会话是 SSL")
    -- pg_hba 若不是 scram-sha-256(md5 / trust)就根本没走通道绑定，上面那条连上也说明不了 PLUS
    t:check(uok and "string" == type(su) and nil ~= su:find("^scram%-sha%-256:"),
            "TLS 连接走的是 SCRAM 认证: " .. tostring(su))
    ps:quit()
end

srey.startup(function()
runner.run(function(t)
    local pg = pgsql.new(DB_HOST, 5432, SSL_NAME.NONE,
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
    -- 刚插了 2 行，affected_rows 就该是 2；>= 0 那种写法恒真，返 0 也照样过
    t:eq(2, pg:affected_rows(), "affected_rows 与本次 INSERT 的行数一致")
    -- readyforquery 返回状态字符码(73 'I' 空闲 / 84 'T' 事务中 / 69 'E' 事务失败 / 0 未收到),
    -- 不是 boolean。必须 t:eq 比值:Lua 里 0 也为真,t:check 连"没收到 ReadyForQuery"都拦不住。
    -- 这里刚跑完一条非事务命令,应回到 'I'
    t:eq(73, pg:readyforquery(), "命令完成后连接回到 ReadyForQuery('I')")

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
    -- reader.at / pgsql.affected_at / pgsql.result_count / pgsql.complete：直接对原始响应包调，下标 1 基。
    -- 响应包只到本协程下次挂起前有效，断言全在 syn_send 之后、不挂起的这一段里做完
    local aok, aret = pg.serial(function()
        local qpack, qsize = cpg.pack_query("select 1 as a; select 2 as b;"
            .. " insert into srey_test (id, name, score) values (200, 'at', 0)")
        local pgpack = srey.syn_send(pg:sock_id(), qpack, qsize, 0)
        if not pgpack then
            return "query 无应答"
        end
        t:eq(3, cpg.result_count(pgpack), "result_count 每条语句一个")
        local rb = preader.at(pgpack, 2)
        local bok, bv = false, nil
        if rb then
            bok, bv = rb:integer("b")
        end
        t:check(bok and 2 == bv, "reader.at(2) 取第二条语句的结果: " .. tostring(bv))
        t:eq(nil, preader.at(pgpack, 2), "同一下标只能取走一次")
        t:eq(nil, preader.at(pgpack, 3), "无结果集的语句给 nil")
        t:eq(nil, preader.at(pgpack, 4), "下标越界给 nil")
        t:eq(nil, preader.at(pgpack, 0), "下标 0 给 nil")
        local ra = preader.at(pgpack, 1)
        local aok1, av = false, nil
        if ra then
            aok1, av = ra:integer("a")
        end
        t:check(aok1 and 1 == av, "reader.at(1) 乱序取也对: " .. tostring(av))
        t:eq(1, cpg.affected_at(pgpack, 1), "affected_at(1) 是第一条 SELECT 的行数")
        t:eq(1, cpg.affected_at(pgpack, 3), "affected_at(3) 是 INSERT 的行数")
        t:eq(0, cpg.affected_at(pgpack, 4), "affected_at 越界为 0")
        t:eq("INSERT 0 1", cpg.complete(pgpack), "complete 是最后一条的标签")
        return true
    end)
    t:check(aok and true == aret, "reader.at 一组跑完: " .. tostring(aret))

    -- 组包抛错（sql 不是字符串）：serial 只打 ERROR 日志，erro() 也得有文案
    t:check(not pg:query({}), "sql 非字符串 query 返 false")
    t:check(pg:erro() ~= "", "组包抛错时 pg:erro() 不为空")

    -- 预处理 + 执行
    local stmt = pg:prepare("stmt_sel", "select name from srey_test where id = $1", 1, { 23 })
    if stmt then
        local bind = pbind.new(1)
        bind:int32(1)
        local r2 = stmt:execute(bind)
        if r2 then
            t:eq(1, _count_rows(r2), "stmt execute one row")
            -- 返回结果集时 affected 也刷成返回的行数，口径同 pg:query
            t:eq(1, stmt:affected_rows(), "stmt execute 有结果集时 affected 为返回行数")
        else
            t:fail("pgsql stmt execute reader nil")
        end
        -- stmt 同理：实绑个数与 bind.new 声明不符时组包抛错
        t:check(not stmt:execute(pbind.new(1)), "绑定个数不符 execute 返 false")
        t:check(stmt:erro() ~= "", "组包抛错时 stmt:erro() 不为空")
        stmt:close()
    else
        t:fail("pgsql prepare nil")
    end

    -- reader:get 按列类型分派，换算必须与单列取值器一致：simple query 是文本格式，
    -- prepare 出的 stmt 默认二进制格式，两种各跑一遍；三行依次是上界、下界、全 NULL
    t:check(pg:query([[create temp table srey_types (ord int, b bool, i2 int2, i4 int4, i8 int8,
        f4 float4, f8 float8, tx text, vc varchar(10), bc char(5), nm name, by bytea, ts timestamp,
        tz timestamptz, dt date, u uuid, nu numeric(10,3))]]), "create temp srey_types")
    t:check(pg:query([[insert into srey_types values
        (1, true, 32767, 2147483647, 9223372036854775807, 3.14, 2.718281828459045, 'h' || chr(233) || 'llo', 'vc',
         'ab', 'nm', '\x00ff10', '2024-05-21 12:34:56.123456', '2024-05-21 12:34:56.789+08', '2024-02-29',
         'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', 1234567.125),
        (2, false, -32768, -2147483648, -9223372036854775808, -1.5, -1e300, '', '', '', '', '',
         '1970-01-01 00:00:00', '1999-12-31 23:59:59.999999+00', '1999-12-31',
         '00000000-0000-0000-0000-000000000000', -0.001),
        (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)]]),
        "insert srey_types: " .. tostring(pg:erro()))
    local trs = pg:query("select * from srey_types order by ord")
    local trows = {}
    if t:check(trs and "userdata" == type(trs[1]), "srey_types 文本格式查询") then
        trows = _pg_get_vs_single(t, trs[1], "text")
    end
    local brows = {}
    local tst = pg:prepare("srey_types_sel", "select * from srey_types order by ord", 0, nil)
    if t:check(tst, "srey_types prepare") then
        local brd = tst:execute(nil)
        if t:check("userdata" == type(brd), "srey_types 二进制格式执行") then
            brows = _pg_get_vs_single(t, brd, "binary")
        end
        tst:close()
    end
    if t:eq(3, #trows, "文本格式 3 行") and t:eq(3, #brows, "二进制格式 3 行") then
        t:eq(math.maxinteger, trows[1].i8, "int8 上界原样")
        t:eq(math.mininteger, trows[2].i8, "int8 下界原样")
        t:eq("h\195\169llo", trows[1].tx, "text 原样（含多字节）")
        t:eq("ab   ", trows[1].bc, "char(5) 带补齐空格")
        t:eq(nil, next(trows[3]), "全 NULL 行 get 全为 nil")
        -- 两种格式除 float4 精度与 bytea 编码外必须相同
        for r = 1, 3 do
            for _, c in ipairs(PG_COLS) do
                if nil ~= c[2] and "f4" ~= c[1] and "by" ~= c[1] then
                    t:eq(trows[r][c[1]], brows[r][c[1]], "row" .. r .. " " .. c[1] .. " 文本与二进制一致")
                end
            end
        end
        -- bytea：文本格式给 '\x' 加十六进制原文，二进制格式给原始字节（C 层口径，不替调用方解码）
        t:eq("\\x00ff10", trows[1].by, "bytea 文本格式是十六进制原文")
        t:eq("\0\255\16", brows[1].by, "bytea 二进制格式是原始字节")
        t:check(math.abs(trows[1].f4 - brows[1].f4) < 1e-6, "float4 两种格式只差 float 精度")
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
    -- 服务端中途报错（坏行）：producer 不挂起，错误原文随 CopyDone 的应答带回；服务端出错后
    -- 会丢掉随后的 CopyData / CopyDone，只回一次 ErrorResponse + ReadyForQuery，连接不错位
    local badsent = false
    t:check(not pg:copy_in("copy srey_test (id, name, score) from stdin", function()
        if badsent then
            return nil
        end
        badsent = true
        return "srey_bad_id\tzed\t1.0\n"
    end), "copy_in 服务端报错返 false")
    t:check(string.find(pg:erro(), "srey_bad_id", 1, true), "erro 带服务端原文: " .. pg:erro())
    t:check(pg:query("select 1"), "服务端报错后连接仍可用")

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

    -- COPY FROM STDIN 误交给 query / copy_out：库须发 CopyFail 把服务端拉出 COPY IN 模式，
    -- 否则服务端攥着事务与表锁，下一条无关命令必失败
    t:check(not pg:query("copy srey_test (id, name, score) from stdin"), "query 收到 COPY_IN 返 false")
    t:check(pg:erro() ~= "", "query 收到 COPY_IN 写了 err")
    t:check(pg:query("select 1"), "query 误收 COPY_IN 后连接仍可用")
    t:check(not pg:copy_out("copy srey_test (id, name, score) from stdin"), "copy_out 收到 COPY_IN 返 false")
    t:check(pg:query("select 1"), "copy_out 误收 COPY_IN 后连接仍可用")
    -- 预处理语句走扩展协议，没法补 Sync，库直接关连接；ping 重连回来
    local cst = pg:prepare("stmt_copy", "copy srey_test (id, name, score) from stdin")
    if t:check(cst, "COPY FROM STDIN 可以 prepare: " .. tostring(pg:erro())) then
        local g = pg.generation
        t:check(not cst:execute(nil), "stmt execute 收到 COPY_IN 返 false")
        -- 代次前进才说明连接已当场关掉；下面的 ping 无论如何都会让它前进，故夹在两者之间查
        t:check(pg.generation ~= g, "stmt 误收 COPY_IN 当场关连接（代次前进）")
        t:check(pg:ping(), "stmt 误收 COPY_IN 关连接后 ping 重连")
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
    local ist = pg:prepare("stmt_reco", "select 1 as v")
    t:check(ist, "stmt 作废用例 prepare: " .. tostring(pg:erro()))
    pg:quit()
    t:check(pg:ping(), "pgsql ping auto-reconnect after quit")
    -- quit + ping 让代次前进：服务端已随旧连接清掉该语句，execute 拒绝，close 不再发包
    if ist then
        t:check(not ist:execute(nil), "重连后旧 stmt execute 返 false")
        t:check(string.find(ist:erro(), "invalidated", 1, true), "stmt erro 写明随重连失效: " .. ist:erro())
        t:check(ist:close(), "重连后旧 stmt close 返 true")
    end

    -- 命令没等到应答：锁内就地关连接（established 清零、代次前进），之后 ping 能重连。
    -- 读超时有 1s 粒度（最坏多等 1s），pg_sleep 给足 2 秒
    local oldnr = srey.get_netread_timeout()
    srey.set_netread_timeout(300)
    local g = pg.generation
    local rsl = pg:query("select pg_sleep(2)")
    srey.set_netread_timeout(oldnr)
    t:eq(false, rsl, "没等到应答 query 返 false")
    t:check(string.find(pg:erro(), "send failed", 1, true), "没等到应答 erro 为发送失败: " .. pg:erro())
    t:eq(false, pg.established, "没等到应答即锁内关连接")
    t:check(pg.generation ~= g, "没等到应答代次前进")
    t:check(pg:ping(), "无应答关连接后 ping 重连")
    t:check(pg:query("select 1"), "重连后查询照常")

    -- 并发：多协程同一条连接各查自己的常量，回读必须原样。没有串行化时命令交错，
    -- 一条 pgsql 命令要读到 ReadyForQuery 才算完，交错会让响应对错协程
    local N, ROUNDS = 4, 6
    -- 用 srey.fork_wait 而不是手写 done 计数 + 有界轮询,理由同 db_mysql.lua
    local fns = {}
    for i = 1, N do
        fns[i] = function()
            local want = 1000 + i
            for _ = 1, ROUNDS do
                local qrs = pg:query(string.format("select %d as v", want))
                if not qrs or "userdata" ~= type(qrs[1]) then
                    return "query failed"
                end
                local rok, v = qrs[1]:integer("v")
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
                "pgsql 并发协程 " .. i .. ": " .. tostring(res[i].ok and res[i].val or res[i][1]))
    end

    -- 活连接上提前回收句柄：发 Terminate 并关连接，C 块由网络线程最后释放
    local p2 = pgsql.new(DB_HOST, 5432, SSL_NAME.NONE, "admin", "12345678", "test")
    if t:check(p2:connect(), "pgsql 第二条连接") then
        p2.pg:__gc()
        t:eq(false, pcall(p2.pg.sock_id, p2.pg), "__gc 后句柄入口报错")
    end
    p2 = nil
    -- 不显式 __gc：连着的句柄丢掉引用后由 GC 回收，走的是同一条路径
    do
        local p3 = pgsql.new(DB_HOST, 5432, SSL_NAME.NONE, "admin", "12345678", "test")
        t:check(p3:connect(), "pgsql 第三条连接")
    end
    collectgarbage()
    collectgarbage()
    srey.sleep(200)-- 让两条连接的关闭在网络线程上走完
    t:check(pg:ping(), "别的连接在活着时被回收后，本连接照常可用")

    _pg_tls_case(t, pg)

    pg:quit()
end)
end)
