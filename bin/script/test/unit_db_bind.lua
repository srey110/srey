-- db 绑定层单元测试（不连数据库）：
-- mysql.bind / pgsql.bind 的 setter 链 + 边界类型处理 + mongo.session 的 pack 路径

local srey   = require("lib.srey")
local runner = require("test.runner")
local utils  = require("srey.utils")
local mbind  = require("mysql.bind")
local pbind  = require("pgsql.bind")
local mongo  = require("mongo")
local mgolib = require("lib.mongo")-- Lua 侧 ctx，测 ctor 的入参自查
local mgsess = require("mongo.session")
local mysql  = require("mysql")
local mreader = require("mysql.reader")-- reader.new / stmt.new 的第一个 lightuserdata 参数要测空指针
local mstmt  = require("mysql.stmt")
local pgsql  = require("pgsql")
local bson   = require("lib.bson")
local yyjson = require("yyjson")-- yyjson.null 是一个 NULL lightuserdata，用来测空指针拒收

srey.startup(function()
runner.run(function(t)
    -- ── mysql.bind ─────────────────────────────────────────────────────
    do
        local b = mbind.new()
        t:check(b ~= nil, "mysql.bind.new")
        -- clear 是幂等操作，未填值前调也无副作用
        b:clear()
        -- 命名参数（query attribute 用）
        b:integer("k_int8", 127)
        b:integer("k_int64", 9007199254740991)  -- 2^53-1（lua_Integer 安全范围上限）
        b:float("k_float", 1.5)
        b:double("k_double", 3.14159265358979)
        b:string("k_str", "srey-mysql-bind")
        b:datetime("k_dt", os.time())
        b:time("k_tm", 0, 0, 1, 30, 0)
        b:null("k_null")
        -- 重复 clear + 再填，验证可复用
        b:clear()
        b:integer(nil, 42)  -- 匿名参数（stmt 占位符 ?）
        b:string(nil, "stmt-bind")
        -- GC 析构走 __gc → _lmysql_bind_free
        b = nil
        collectgarbage()
        t:check(true, "mysql.bind GC roundtrip ok")
    end

    -- ── mysql: erro 带出错误号 ─────────────────────────────────────────
    -- 文案随服务端版本与 locale 变，区分可重试（1213 死锁 / 1205 锁等待超时）与
    -- 不可重试（1062 重复键）只能靠错误号；C 层 mysql_erro 本就有它，绑定层原来硬传 NULL
    do
        local my = mysql.new("127.0.0.1", 3306, nil, "u", "p", "d", "utf8mb4")
        t:eq(2, select("#", my:erro()), "mysql erro 返 2 个值（文案 + 错误号）")
        local emsg, ecode = my:erro()
        t:eq(0, ecode, "无错误时错误号为 0")
        t:check(nil ~= emsg, "无错误时文案非 nil")
    end

    -- ── pgsql.bind ─────────────────────────────────────────────────────
    do
        -- new(nparam) 预设参数数
        local b = pbind.new(8)
        t:check(b ~= nil, "pgsql.bind.new(8)")

        -- 各类型 setter：每个 set 自动占用一个槽位
        b:int16(32767)
        b:int32(-2147483648)
        b:int64(9007199254740991)
        b:float(2.5)
        b:double(3.14159)
        b:text("pg-bind-test")
        b:bytea("\x00\x01\x02\xff")
        b:bool(true)
        b:clear()

        -- 非 number 类型自动转 NULL（bool 接受 boolean/number）
        local b2 = pbind.new(5)
        b2:bool(nil)     -- 转 NULL
        b2:int32(nil)    -- 转 NULL
        b2:double(nil)   -- 转 NULL
        b2:text(nil)     -- 转 NULL
        b2:null()        -- 显式 NULL
        b2 = nil

        -- nparam 越界报错而不是截断：Bind/Parse 报文里的参数个数是 Int16。
        -- 65536 截成 0 会得到一个"绑什么都无视"的 bind，-1 截成 65535 反过来按最大参数数
        -- 建头部，两种情况调用方从返回值上都看不出来
        t:eq(false, pcall(pbind.new, -1),    "pgsql.bind.new 负 nparam 被拒")
        t:eq(false, pcall(pbind.new, 65536), "pgsql.bind.new 超 INT16_MAX 被拒")
        t:eq(true,  pcall(pbind.new, 0),     "pgsql.bind.new(0) 合法")
        t:eq(false, pcall(pgsql.pack_stmt_prepare, "st", "select 1", -1),
             "pack_stmt_prepare 负 nparam 被拒")
        t:eq(false, pcall(pgsql.pack_stmt_prepare, "st", "select 1", 32768),
             "pack_stmt_prepare 超 INT16_MAX 被拒")

        -- 时间相关
        local b3 = pbind.new(3)
        b3:timestamp(os.time())
        b3:date(20260522)
        b3:uuid("0123456789abcdef0123456789abcdef")   -- 32 字符
        b3 = nil
        collectgarbage()
        t:check(true, "pgsql.bind GC roundtrip ok")
    end

    -- ── bind 显式析构后再复用 ──────────────────────────────────────────
    -- REG_MTABLE 把 __gc 和 __index 放在同一张表里，脚本能直接调 b:__gc()。
    -- 旧实现只把缓冲指针置空却留着 size/offset，下一次写入会以为"还写得下"从而
    -- 跳过扩容、往空指针上 memset —— 段错误，且是脚本一行就能触发的自伤路径
    do
        local b = mbind.new()
        b:string("k", "v")
        b:__gc()
        b:__gc()  -- 重复析构幂等
        -- 析构后再绑定：等同刚 new 出来的空上下文，不炸也不留旧数据
        b:integer("k2", 1)
        b:string("k3", "after-free")
        b:null()
        b:clear()
        t:check(true, "mysql.bind __gc 后复用不崩")

        local p = pbind.new(2)
        p:int32(1)
        p:__gc()
        p:__gc()
        -- 析构后 nparam 归零，后续绑定被统一早退挡住，静默无视
        p:int32(2)
        p:text("after-free")
        p:null()
        p:clear()
        t:check(true, "pgsql.bind __gc 后复用不崩")

        -- 组包侧同样按 nparam 早退，不会发出半截 Bind 消息
        local pack, size = pgsql.pack_stmt_execute("st_af", p)
        t:check(pack ~= nil and size > 0, "已析构的 bind 交给 pack_stmt_execute 仍得到完整报文")
        utils.ud_free(pack)

        b = nil
        p = nil
        collectgarbage()
    end

    -- ── full userdata 当数据缓冲：只认 nil / string / lightuserdata ────
    -- 四个入口曾把 LUA_TUSERDATA 与 lightuserdata 并到同一个 case，对句柄对象
    -- 不查元表就取载荷首址当字节缓冲，配无上界的长度即堆越界读，内容还随报文
    -- 发往数据库。现统一走 lpub_opt_buf：nil 仍绑 NULL，其余类型一律报错
    do
        local handle = mbind.new()  -- 一个真实句柄对象（full userdata）
        local pb = pbind.new(1)
        local mb = mbind.new()

        t:eq(false, pcall(pb.text, pb, handle, 4096), "pgsql bind:text 收 full userdata 报错")
        t:eq(false, pcall(pb.bytea, pb, handle, 4096), "pgsql bind:bytea 收 full userdata 报错")
        t:eq(false, pcall(pgsql.pack_copy_data, handle, 4096), "pgsql.pack_copy_data 收 full userdata 报错")
        t:eq(false, pcall(mb.string, mb, "k", handle, 4096), "mysql bind:string 收 full userdata 报错")

        -- 其余非 string/lightuserdata 同样报错；nil 仍是 NULL 绑定
        t:eq(false, pcall(pb.text, pb, 42), "pgsql bind:text 收 number 报错")
        t:eq(false, pcall(pb.text, pb, {}), "pgsql bind:text 收 table 报错")
        t:eq(true, pcall(pb.text, pb, nil), "pgsql bind:text 收 nil 仍绑 NULL")
        local pnull, snull = pgsql.pack_stmt_execute("st_ud", pb)
        t:check(pnull ~= nil and snull > 0, "nil 绑 NULL 后组包正常")
        utils.ud_free(pnull)

        -- lightuserdata 仍照收，长度上界统一收到 INT32_MAX
        local ptr, plen = pgsql.pack_copy_data("payload")
        t:check(ptr ~= nil and plen > 0, "pack_copy_data 的 lightuserdata 仍正常")
        t:eq(false, pcall(pgsql.pack_copy_data, ptr, 2147483648), "长度超 INT32_MAX 被拒")
        local bt = pbind.new(1)
        t:eq(false, pcall(bt.text, bt, ptr, 2147483648), "bind:text 长度超 INT32_MAX 被拒")
        t:eq(true, pcall(bt.text, bt, ptr, plen), "bind:text 合法 lightuserdata+长度照常接受")
        utils.ud_free(ptr)

        handle = nil
        collectgarbage()
    end

    -- ── mongo.session ──────────────────────────────────────────────────
    do
        -- 不连接，仅构造 mongo ctx；session.new 需要 16 字节 uuid
        local mg = mongo.new("127.0.0.1", 27017, nil, "testdb")
        t:check(mg ~= nil, "mongo.new (无连接)")

        -- uuid 长度校验：!= 16 字节返回 nil
        local bad = mgsess.new(mg, "short", 10)
        t:eq(nil, bad, "session.new uuid 短长度拒绝")

        -- 合法 16 字节 uuid 创建 session
        local uuid = string.rep("\xab", 16)
        local sess = mgsess.new(mg, uuid, 30)
        t:check(sess ~= nil, "session.new 16 字节 uuid")

        -- pack_endsession：无需 begin，直接打包 endSessions 命令
        local pack, size = sess:pack_endsession()
        t:check(pack ~= nil and size > 0, "session pack_endsession")
        -- wire 上含 endSessions 字段名（mongo OP_MSG body 一般为 BSON 字符串）
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("endSessions", 1, true) ~= nil, "endSessions in wire")
        utils.ud_free(pack)

        -- pack_refresh
        pack, size = sess:pack_refresh()
        t:check(pack ~= nil and size > 0, "session pack_refresh")
        txt = srey.ud_str(pack, size)
        t:check(txt:find("refreshSessions", 1, true) ~= nil, "refreshSessions in wire")
        utils.ud_free(pack)

        -- set_flag 收得下 clear_flag 的返回值：没有标志时 clear_flag 返 0，
        -- 而 0 曾被当成非法值 WARN 掉，"存档-还原"惯用法每轮都要吵一条日志
        local mg2 = mgolib.new("127.0.0.1", 27017, SSL_NAME.NONE, "d")
        local old = mg2:clear_flag()
        t:eq(0, old, "没有标志时 clear_flag 返 0")
        t:eq(true, pcall(mg2.set_flag, mg2, old), "set_flag(0) 是空操作，不报错")

        -- ctor 的 authmod 自查：不挡的话非字符串要到第一次 _connect 的 pack_auth_first
        -- 才抛，而那一抛正落在 clear_flag 与 set_flag 之间，MORETOCOME 被永久摘掉、fd 也漏
        t:eq(false, pcall(mgolib.new, "127.0.0.1", 27017, SSL_NAME.NONE, "d", "u", "p", nil, {}),
            "mongo ctor: authmod 传 table 被拒")
        t:eq(false, pcall(mgolib.new, "127.0.0.1", 27017, SSL_NAME.NONE, "d", "u", "p", nil, true),
            "mongo ctor: authmod 传 boolean 被拒")
        t:eq(true, pcall(mgolib.new, "127.0.0.1", 27017, SSL_NAME.NONE, "d", "u", "p", nil, "SCRAM-SHA-1"),
            "mongo ctor: authmod 传字符串照常接受")
        -- 同族的 password 自查（既有行为，一并钉住）
        t:eq(false, pcall(mgolib.new, "127.0.0.1", 27017, SSL_NAME.NONE, "d", "u"),
            "mongo ctor: 给了 user 不给 password 被拒")

        -- 超时判定：timeoutmin=30 建出来的会话剩余约 1800 秒（留出跑测试的余量）
        local left = sess:expires_in()
        t:check(left > 1790 and left <= 1800, "session expires_in 约 30 分钟，实际 " .. tostring(left))
        -- 服务端没给出 timeoutmin 时按"未知"处理，恒返 0 促使调用方主动 refresh
        local nosess = mgsess.new(mg, uuid, 0)
        t:eq(0, nosess:expires_in(), "timeoutmin=0 时 expires_in 恒为 0")
        -- 分钟数来自服务端应答，负值曾经过 uint64 提升回绕成一个已过期的时刻
        t:eq(false, pcall(mgsess.new, mg, uuid, -1), "session.new 负超时分钟数被拒")
        -- 连接没绑会话时 touch 是空操作，不该报错
        t:eq(true, pcall(mg.session_touch, mg), "未绑会话时 session_touch 空操作")

        -- begin → pack_commit / pack_abort：commit/abort 依赖 begin 构造 options
        t:check(sess:begin(), "session begin (no active txn on conn)")
        pack, size = sess:pack_commit()
        t:check(pack ~= nil and size > 0, "session pack_commit (after begin)")
        txt = srey.ud_str(pack, size)
        t:check(txt:find("commitTransaction", 1, true) ~= nil, "commitTransaction in wire")
        utils.ud_free(pack)
        sess:done()

        t:check(sess:begin(), "session begin again (after done)")
        pack, size = sess:pack_abort()
        t:check(pack ~= nil and size > 0, "session pack_abort (after begin)")
        txt = srey.ud_str(pack, size)
        t:check(txt:find("abortTransaction", 1, true) ~= nil, "abortTransaction in wire")
        utils.ud_free(pack)
        sess:done()

        -- free + 重复 free 幂等
        sess:free()
        sess:free()
        sess = nil
        mg = nil
        collectgarbage()
        t:check(true, "mongo.session GC roundtrip ok")
    end

    -- ── 宿主被 m:__gc() 提前释放后，session 的每个入口都必须报可捕获的错 ──
    -- session 里存的是宿主的裸指针，宿主一释放读它就是 use-after-free；
    -- begin 更是往里写（mongo->session = session），只判自身非空拦不住
    do
        local mg = mongo.new("127.0.0.1", 27017, nil, "testdb")
        local sess = mgsess.new(mg, string.rep("\xcd", 16), 30)
        t:check(sess ~= nil, "session.new before owner gc")
        mg:__gc()
        t:eq(false, pcall(function() sess:begin() end), "owner 释放后 begin 被拒")
        t:eq(false, pcall(function() sess:pack_refresh() end), "owner 释放后 pack_refresh 被拒")
        t:eq(false, pcall(function() sess:pack_endsession() end), "owner 释放后 pack_endsession 被拒")
        t:eq(false, pcall(function() sess:pack_commit() end), "owner 释放后 pack_commit 被拒")
        t:eq(false, pcall(function() sess:pack_abort() end), "owner 释放后 pack_abort 被拒")
        -- done / free 不抛错，但内部那步"解宿主的 session 绑定"必须跳过。
        -- 注意：守卫真被删掉时那步是读悬垂指针，release 下全程静默，这里只能守住
        -- "不抛错 / 幂等 / 之后仍按已释放报错"这几条可观测的，UAF 本身要靠 ASan
        t:eq(true, pcall(function() sess:done() end), "owner 释放后 done 不抛错")
        t:eq(true, pcall(function() sess:free() end), "owner 释放后 free 不抛错")
        t:eq(true, pcall(function() sess:free() end), "owner 释放后 free 幂等")
        t:eq(false, pcall(function() sess:begin() end), "free 之后 begin 仍被拒")
        sess = nil
        mg = nil
        collectgarbage()
    end

    -- ── mysql packer 系列（不连接，仅 buffer 构造）────────────────────
    do
        local m = mysql.new("127.0.0.1", 3306, nil, "admin", "x", "testdb", "utf8mb4")
        t:check(m ~= nil, "mysql.new (无连接)")
        t:eq("_mysql_ctx", getmetatable(m), "REG_MTABLE 置 __metatable:普通 getmetatable 只得类型名")
        t:check(debug.getmetatable(m) ~= nil, "debug.getmetatable 仍可取真元表(__metatable 不挡它)")

        -- 这四个入口原来用 luaL_checktype(LUA_TLIGHTUSERDATA)，只判类型不判空——空指针照过，
        -- 而 pack_type / reader_new 的下游是裸解引用（mysql_reader_init 不像 mysql_stmt_init 那样判空）
        t:eq(false, pcall(mysql.pack_type, yyjson.null), "mysql.pack_type NULL 指针被拒")
        t:eq(false, pcall(mysql.has_more, yyjson.null), "mysql.has_more NULL 指针被拒")
        t:eq(false, pcall(mreader.new, yyjson.null), "mysql.reader.new NULL 指针被拒")
        t:eq(false, pcall(mstmt.new, m, yyjson.null), "mysql.stmt.new NULL 指针被拒")

        local pack, size = m:pack_ping()
        t:check(pack ~= nil and size > 0, "mysql pack_ping non-empty")
        utils.ud_free(pack)

        pack, size = m:pack_quit()
        t:check(pack ~= nil and size > 0, "mysql pack_quit non-empty")
        utils.ud_free(pack)

        pack, size = m:pack_selectdb("newdb")
        t:check(pack ~= nil and size > 0, "mysql pack_selectdb non-empty")
        t:check(srey.ud_str(pack, size):find("newdb", 1, true) ~= nil, "selectdb wire 含库名")
        utils.ud_free(pack)

        pack, size = m:pack_query("SELECT 1")
        t:check(pack ~= nil and size > 0, "mysql pack_query non-empty")
        t:check(srey.ud_str(pack, size):find("SELECT 1", 1, true) ~= nil, "query wire 含 SQL")
        utils.ud_free(pack)

        -- 带 bind 的 query
        local b = mbind.new()
        b:integer("k", 42)
        pack, size = m:pack_query("SELECT ? AS v", b)
        t:check(pack ~= nil and size > 0, "mysql pack_query with bind non-empty")
        utils.ud_free(pack)

        pack, size = m:pack_stmt_prepare("SELECT * FROM t WHERE id=?")
        t:check(pack ~= nil and size > 0, "mysql pack_stmt_prepare non-empty")
        t:check(srey.ud_str(pack, size):find("WHERE id=?", 1, true) ~= nil, "prepare wire 含 SQL")
        utils.ud_free(pack)

        m = nil
        collectgarbage()
        t:check(true, "mysql packer GC roundtrip ok")
    end

    -- ── pgsql packer 系列（不连接，仅 buffer 构造）────────────────────
    do
        local p = pgsql.new("127.0.0.1", 5432, nil, "admin", "x", "testdb")
        t:check(p ~= nil, "pgsql.new (无连接)")

        -- setter 长度边界：字段是 char[64]，超 63 字节须返 false 且保持原值不变，
        -- 否则 selectdb 会用旧库名重连成功、把切库失败报成功
        local toolong = string.rep("d", 64)
        t:check(p:set_db("okdb"), "pgsql set_db (63 字节内)")
        t:check(p:get_db() == "okdb", "set_db 生效")
        t:check(not p:set_db(toolong), "pgsql set_db 超长返 false")
        t:check(p:get_db() == "okdb", "set_db 超长不改动原库名")
        t:check(p:set_userpwd("u2", "p2"), "pgsql set_userpwd (63 字节内)")
        t:check(not p:set_userpwd(toolong, "p2"), "set_userpwd 用户名超长返 false")
        t:check(not p:set_userpwd("u2", toolong), "set_userpwd 密码超长返 false")

        -- pack_cancel 要用握手带回的 BackendKeyData（pid + key）。这个 p 从未连接，pid 恒为 0，
        -- 组出来的 CancelRequest 匹配不到任何后端，发出去只被服务端静默丢弃却让调用方以为取消成功，
        -- 故绑定层须直接拒掉；pgsql.lua 的 ctx:cancel 靠这个 nil 返 false（C 侧同款守卫在 pgsql_cancel）
        t:eq(nil, p:pack_cancel(), "pgsql pack_cancel 未握手时返回 nil")

        local pack, size = pgsql.pack_query("SELECT 1")
        t:check(pack ~= nil and size > 0, "pgsql pack_query non-empty")
        t:check(srey.ud_str(pack, size):find("SELECT 1", 1, true) ~= nil, "pg query wire 含 SQL")
        utils.ud_free(pack)

        pack, size = pgsql.pack_terminate()
        t:check(pack ~= nil and size > 0, "pgsql pack_terminate non-empty")
        utils.ud_free(pack)

        pack, size = pgsql.pack_stmt_prepare("st1", "SELECT $1::int", 1, { 23 })
        t:check(pack ~= nil and size > 0, "pgsql pack_stmt_prepare non-empty")
        t:check(srey.ud_str(pack, size):find("st1", 1, true) ~= nil, "prepare wire 含 stmt name")
        utils.ud_free(pack)

        pack, size = pgsql.pack_stmt_close("st1")
        t:check(pack ~= nil and size > 0, "pgsql pack_stmt_close non-empty")
        utils.ud_free(pack)

        -- pack_stmt_execute 不带 bind
        pack, size = pgsql.pack_stmt_execute("st1")
        t:check(pack ~= nil and size > 0, "pgsql pack_stmt_execute (no bind) non-empty")
        utils.ud_free(pack)

        -- pack_stmt_execute 带 bind
        local b = pbind.new(1)
        b:int32(42)
        pack, size = pgsql.pack_stmt_execute("st1", b)
        t:check(pack ~= nil and size > 0, "pgsql pack_stmt_execute with bind non-empty")
        utils.ud_free(pack)

        -- 结果列格式只有 0/1：曾用裸转换，非法值让 reader 各取值器走错解码分支
        -- （文本行 "f" 被二进制分支当成非零字节，FALSE 到 Lua 侧变成 true 而 err 仍是 ERR_OK）
        t:eq(false, pcall(pgsql.pack_stmt_execute, "st1", nil, 2), "pack_stmt_execute: format 2 被拒")
        t:eq(false, pcall(pgsql.pack_stmt_execute, "st1", nil, -1), "pack_stmt_execute: format 负值被拒")
        -- 放行的这条会真的组出包，返回的指针归调用方，丢掉就是泄漏
        local okfmt, pkfmt = pcall(pgsql.pack_stmt_execute, "st1", nil, 1)
        t:eq(true, okfmt, "pack_stmt_execute: format 1 照常接受")
        if okfmt then utils.ud_free(pkfmt) end

        -- COPY 流操作
        pack, size = pgsql.pack_copy_data("1,2,3\n")
        t:check(pack ~= nil and size > 0, "pgsql pack_copy_data non-empty")
        utils.ud_free(pack)

        pack, size = pgsql.pack_copy_done()
        t:check(pack ~= nil and size > 0, "pgsql pack_copy_done non-empty")
        utils.ud_free(pack)

        pack, size = pgsql.pack_copy_fail("oops")
        t:check(pack ~= nil and size > 0, "pgsql pack_copy_fail non-empty")
        t:check(srey.ud_str(pack, size):find("oops", 1, true) ~= nil, "copy_fail wire 含 reason")
        utils.ud_free(pack)

        p = nil
        collectgarbage()
        t:check(true, "pgsql packer GC roundtrip ok")
    end

    -- ── mongo packer 系列（不连接，仅 buffer 构造）────────────────────
    do
        local mg = mongo.new("127.0.0.1", 27017, nil, "testdb")
        t:check(mg ~= nil, "mongo.new (无连接) for packer")
        mg:collection("coll1")

        local pack, size = mg:pack_hello()
        t:check(pack ~= nil and size > 0, "mongo pack_hello non-empty")
        t:check(srey.ud_str(pack, size):find("hello", 1, true) ~= nil, "hello wire 含 hello")
        utils.ud_free(pack)

        pack, size = mg:pack_ping()
        t:check(pack ~= nil and size > 0, "mongo pack_ping non-empty")
        t:check(srey.ud_str(pack, size):find("ping", 1, true) ~= nil, "ping wire 含 ping")
        utils.ud_free(pack)

        pack, size = mg:pack_drop()
        t:check(pack ~= nil and size > 0, "mongo pack_drop non-empty")
        t:check(srey.ud_str(pack, size):find("drop", 1, true) ~= nil, "drop wire 含 drop")
        utils.ud_free(pack)

        -- 构造 docs/updates/deletes BSON 数组（单元素：含一个嵌套 doc）
        local docs = bson.encode({ { a = 1 } })
        local docs_ptr, docs_sz = docs:data()

        pack, size = mg:pack_insert(docs_ptr, docs_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_insert non-empty")
        t:check(srey.ud_str(pack, size):find("insert", 1, true) ~= nil, "insert wire 含 insert")
        utils.ud_free(pack)

        -- updates 数组：含 q (filter) + u (update doc)
        local updates = bson.encode({ { q = { a = 1 }, u = { ["$set"] = { b = 2 } } } })
        local upd_ptr, upd_sz = updates:data()
        pack, size = mg:pack_update(upd_ptr, upd_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_update non-empty")
        t:check(srey.ud_str(pack, size):find("update", 1, true) ~= nil, "update wire 含 update")
        utils.ud_free(pack)

        -- deletes 数组：含 q (filter) + limit
        local deletes = bson.encode({ { q = { a = 1 }, limit = 0 } })
        local del_ptr, del_sz = deletes:data()
        pack, size = mg:pack_delete(del_ptr, del_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_delete non-empty")
        t:check(srey.ud_str(pack, size):find("delete", 1, true) ~= nil, "delete wire 含 delete")
        utils.ud_free(pack)

        -- find 无 filter
        pack, size = mg:pack_find()
        t:check(pack ~= nil and size > 0, "mongo pack_find (no filter) non-empty")
        t:check(srey.ud_str(pack, size):find("find", 1, true) ~= nil, "find wire 含 find")
        utils.ud_free(pack)

        -- find 带 filter
        local filter = bson.encode({ a = 1 })
        local f_ptr, f_sz = filter:data()
        pack, size = mg:pack_find(f_ptr, f_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_find with filter non-empty")
        utils.ud_free(pack)

        -- aggregate pipeline
        local pipeline = bson.encode({ { ["$match"] = { a = 1 } }, { ["$count"] = "n" } })
        local pl_ptr, pl_sz = pipeline:data()
        pack, size = mg:pack_aggregate(pl_ptr, pl_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_aggregate non-empty")
        t:check(srey.ud_str(pack, size):find("aggregate", 1, true) ~= nil, "aggregate wire 含 aggregate")
        utils.ud_free(pack)

        pack, size = mg:pack_getmore(123456789)
        t:check(pack ~= nil and size > 0, "mongo pack_getmore non-empty")
        t:check(srey.ud_str(pack, size):find("getMore", 1, true) ~= nil, "getmore wire 含 getMore")
        utils.ud_free(pack)

        -- killcursors 数组
        local cursorids = bson.encode({ bson.mkint64(123), bson.mkint64(456) })
        local c_ptr, c_sz = cursorids:data()
        pack, size = mg:pack_killcursors(c_ptr, c_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_killcursors non-empty")
        t:check(srey.ud_str(pack, size):find("killCursors", 1, true) ~= nil, "killcursors wire 含 killCursors")
        utils.ud_free(pack)

        -- distinct 无 query
        pack, size = mg:pack_distinct("fieldA")
        t:check(pack ~= nil and size > 0, "mongo pack_distinct (no query) non-empty")
        t:check(srey.ud_str(pack, size):find("distinct", 1, true) ~= nil, "distinct wire 含 distinct")
        utils.ud_free(pack)

        -- findandmodify (remove=1)
        pack, size = mg:pack_findandmodify(f_ptr, f_sz, 1, 0, nil, 0)
        t:check(pack ~= nil and size > 0, "mongo pack_findandmodify (remove) non-empty")
        t:check(srey.ud_str(pack, size):find("findAndModify", 1, true) ~= nil, "findandmodify wire 含 findAndModify")
        utils.ud_free(pack)

        -- findandmodify (update doc)
        local update_doc = bson.encode({ ["$set"] = { b = 99 } })
        local u_ptr, u_sz = update_doc:data()
        pack, size = mg:pack_findandmodify(f_ptr, f_sz, 0, 0, u_ptr, u_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_findandmodify (update) non-empty")
        utils.ud_free(pack)

        -- remove=0 时 update 必填：漏传的话 mongo_pack_findandmodify 会写出一个声明了
        -- ulens 字节却一字节没有的 update 元素，整条命令从这里错位发上线且无报错
        t:eq(false, pcall(mg.pack_findandmodify, mg, f_ptr, f_sz, 0, 0, nil, 0),
             "findandmodify: remove=0 缺 update 报错")
        t:eq(false, pcall(mg.pack_findandmodify, mg, f_ptr, f_sz, 0, 0, yyjson.null, 16),
             "findandmodify: remove=0 传 NULL 指针报错")
        -- 空缓冲与 NULL 同罪：非空指针 + 长度 0 一样会写出有键无体的 update 元素。
        -- 只判指针非空的守卫在这里会放行，必须连长度一起判
        t:eq(false, pcall(mg.pack_findandmodify, mg, f_ptr, f_sz, 0, 0, "", 0),
             "findandmodify: remove=0 传空字符串报错")
        t:eq(false, pcall(mg.pack_findandmodify, mg, f_ptr, f_sz, 0, 0, u_ptr, 0),
             "findandmodify: remove=0 传零长度报错")

        -- 可选文档收到空缓冲时应当"当没给"，而不是写下键再跳过文档体：
        -- 后者产出的元素声明了子文档却零字节，服务端会把下一个元素的头 4 字节当成它的长度
        local nofilter, nf_sz = mg:pack_find()
        local emptyfilter, ef_sz = mg:pack_find("", 0)
        t:eq(nf_sz, ef_sz, "pack_find 空 filter 与不传 filter 组出同样长度")
        t:check(nil == srey.ud_str(emptyfilter, ef_sz):find("filter", 1, true),
                "pack_find 空 filter 不写 filter 键")
        utils.ud_free(nofilter)
        utils.ud_free(emptyfilter)

        -- count 无 query
        pack, size = mg:pack_count()
        t:check(pack ~= nil and size > 0, "mongo pack_count (no query) non-empty")
        t:check(srey.ud_str(pack, size):find("count", 1, true) ~= nil, "count wire 含 count")
        utils.ud_free(pack)

        -- createindexes 数组：{ key={a=1}, name="a_1" }
        local indexes = bson.encode({ { key = { a = 1 }, name = "a_1" } })
        local i_ptr, i_sz = indexes:data()
        pack, size = mg:pack_createindexes(i_ptr, i_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_createindexes non-empty")
        t:check(srey.ud_str(pack, size):find("createIndexes", 1, true) ~= nil, "createindexes wire 含 createIndexes")
        utils.ud_free(pack)

        -- dropindexes：index 名数组
        local dropidx = bson.encode({ "a_1" })
        local d_ptr, d_sz = dropidx:data()
        pack, size = mg:pack_dropindexes(d_ptr, d_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_dropindexes non-empty")
        t:check(srey.ud_str(pack, size):find("dropIndexes", 1, true) ~= nil, "dropindexes wire 含 dropIndexes")
        utils.ud_free(pack)

        pack, size = mg:pack_startsession()
        t:check(pack ~= nil and size > 0, "mongo pack_startsession non-empty")
        t:check(srey.ud_str(pack, size):find("startSession", 1, true) ~= nil, "startsession wire 含 startSession")
        utils.ud_free(pack)

        -- bulkwrite：ops 数组 + nsinfo 数组
        local ops = bson.encode({ { insert = 0, document = { a = 1 } } })
        local op_ptr, op_sz = ops:data()
        local nsinfo = bson.encode({ { ns = "testdb.coll1" } })
        local n_ptr, n_sz = nsinfo:data()
        pack, size = mg:pack_bulkwrite(op_ptr, op_sz, n_ptr, n_sz)
        t:check(pack ~= nil and size > 0, "mongo pack_bulkwrite non-empty")
        t:check(srey.ud_str(pack, size):find("bulkWrite", 1, true) ~= nil, "bulkwrite wire 含 bulkWrite")
        utils.ud_free(pack)

        -- 必填的数组参数长度为 0 时整条命令作废（MONGO_PACK_ARR）。放行的话
        -- bson_append_array 只写 type+key 不写数组体，整篇 BSON 从这个元素起就解不开，
        -- 而这种包发出去服务端多半照收，错位要到后面某个字段才暴露。
        -- 断言的是"返 nil"而非"报错"：与 opts 畸形时的既有行为同口径
        t:eq(nil, mg:pack_insert(docs_ptr, 0), "pack_insert 空 documents 返 nil")
        t:eq(nil, mg:pack_update(upd_ptr, 0), "pack_update 空 updates 返 nil")
        t:eq(nil, mg:pack_delete(del_ptr, 0), "pack_delete 空 deletes 返 nil")
        t:eq(nil, mg:pack_aggregate(pl_ptr, 0), "pack_aggregate 空 pipeline 返 nil")
        t:eq(nil, mg:pack_killcursors(c_ptr, 0), "pack_killcursors 空 cursors 返 nil")
        t:eq(nil, mg:pack_createindexes(i_ptr, 0), "pack_createindexes 空 indexes 返 nil")
        t:eq(nil, mg:pack_dropindexes(d_ptr, 0), "pack_dropindexes 空 index 返 nil")
        t:eq(nil, mg:pack_bulkwrite(op_ptr, 0, n_ptr, n_sz), "pack_bulkwrite 空 ops 返 nil")
        t:eq(nil, mg:pack_bulkwrite(op_ptr, op_sz, n_ptr, 0), "pack_bulkwrite 空 nsInfo 返 nil")

        mg = nil
        collectgarbage()
        t:check(true, "mongo packer GC roundtrip ok")
    end

    -- ── mongo opts 参数校验：组包侧 bson_cat 按文档自身的 4 字节头决定拷贝多少，
    --    没有外部长度就判断不出那个自声明值有没有超出缓冲。
    --    绑定层只验长度本身合法（报 Lua 错），"是不是完整文档"由 bson_cat 判（返 nil）──
    do
        local mg = mongo.new("127.0.0.1", 27017, nil, "testdb")
        mg:collection("coll1")
        local opts = bson.encode({ comment = "hi" })
        local o_ptr, o_sz = opts:data()

        -- 合法三形态：nil / (指针,长度) / string
        local pack, size = mg:pack_find()
        t:check(pack ~= nil and size > 0, "opts 省略：组包正常")
        utils.ud_free(pack)
        pack, size = mg:pack_find(nil, nil, o_ptr, o_sz)
        t:check(pack ~= nil and size > 0, "opts 为 (指针,长度)：组包正常")
        t:check(srey.ud_str(pack, size):find("comment", 1, true) ~= nil, "opts 内容确实拼进了 wire")
        utils.ud_free(pack)
        pack, size = mg:pack_find(nil, nil, srey.ud_str(o_ptr, o_sz))
        t:check(pack ~= nil and size > 0, "opts 为 string：组包正常")
        utils.ud_free(pack)

        -- 长度本身非法在绑定层就报 Lua 错：缺长度、负长度
        t:eq(false, pcall(function() return mg:pack_find(nil, nil, o_ptr) end), "opts 为 lightuserdata 却不给长度被拒")
        t:eq(false, pcall(function() return mg:pack_find(nil, nil, o_ptr, -1) end), "opts 负长度被拒")

        -- 关键用例：缓冲比文档头声明的长度短。旧实现会照着头里的长度往后读，
        -- 把相邻堆内存拼进随后发出的 OP_MSG。这道判定在组包侧的 bson_cat，
        -- 故不抛错而是整条命令作废返 nil（*size 一并置 0）
        t:eq(nil, mg:pack_find(nil, nil, o_ptr, 5), "opts 文档头声明长度超出缓冲：组包返 nil")
        t:eq(nil, mg:pack_find(nil, nil, srey.ud_str(o_ptr, o_sz):sub(1, 8)), "string 形态的截断 opts 同样返 nil")
        -- 不足最小文档（4 字节长度头 + EOD）
        t:eq(nil, mg:pack_find(nil, nil, "abc"), "opts 不足 5 字节：组包返 nil")

        -- opts 位置在各 pack_* 上不同，抽样确认走的是同一条组包路径
        t:eq(nil, mg:pack_drop(o_ptr, 5), "pack_drop 的 opts 同样返 nil")
        t:eq(nil, mg:pack_count(nil, nil, o_ptr, 5), "pack_count 的 opts 同样返 nil")

        mg = nil
        collectgarbage()
    end
end)
end)
