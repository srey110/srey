-- Lua 层单元测试：lib/utils.lua（split/host_type/table_size/randstr/class/dump 等）
--                  + lib/log.lua（级别短路现读 C 层 / log_setlv round-trip）
--                  + srey.MSG_TYPE 与 C 侧 msg_type 枚举对齐
--                  + core.wait_skid 的入参类型检查（经 wait_connect / sync_close / syn_recv）
--                  + task.msg_release 对各种入参的处理

local srey   = require("lib.srey")
local runner = require("test.runner")
local utils  = require("srey.utils")
local core   = require("srey.core")
local task   = require("srey.task")

local UDP_PORT = 15073 -- msg_release 用例 UDP 自发自收

srey.startup(function()
runner.run(function(t)
    -- ── MSG_TYPE 与 C 侧 msg_type 对齐 ─────────────────────────────────
    -- 这张表是按 C 枚举手抄的字面量。C 侧往中间插一个成员，后面的取值整体后移，
    -- 而 Lua 这边毫无察觉：_dispatchers[msg.mtype] 静默错投（未知值被 if dispatcher then
    -- 吞掉）、message_may_keep 答的是别的类型、_msg_clean 挑错释放方式。
    -- C 的名字表是权威（protocol/prots.c 的 _mtype_names），逐项对名字钉住
    do
        local n = 0
        for name, v in pairs(srey.MSG_TYPE) do
            t:eq(name, core.message_str(v), "MSG_TYPE." .. name .. " 对上 C 侧同名")
            n = n + 1
        end
        t:eq(13, n, "MSG_TYPE 成员数")
        -- 紧邻最大值的那个必须越界。C 侧往末尾追加新 mtype 时这条会红，
        -- 提醒把新成员补进 Lua 表（追加不移动既有取值，逐项对名字那圈查不出来）
        t:eq(false, pcall(core.message_str, 14), "C 侧未追加新 mtype")
    end

    -- ── wait_skid：传的不是连接标识当场报错，失效连接仍按"已断"返回 ──────
    -- 各 wait_* / syn_* 与 sync_close 都经它取 skid；类型错若也当已断处理，
    -- 误用看起来就像网络故障
    do
        t:eq(false, pcall(srey.wait_connect, {}), "wait_connect 传表立即报错")
        t:eq(false, pcall(srey.wait_connect, nil), "wait_connect 传 nil 立即报错")
        t:eq(false, pcall(srey.sync_close, nil), "sync_close 传 nil 立即报错")
        t:eq(false, pcall(core.wait_skid, 1), "wait_skid 传整数立即报错")
        local bad = srey.sock_invalid()
        t:eq(nil, core.wait_skid(bad), "失效连接 wait_skid 返回 nil")
        t:eq(false, srey.wait_connect(bad), "失效连接 wait_connect 返回 false")
        t:eq(nil, srey.syn_recv(bad), "失效连接 syn_recv 返回 nil")
    end

    -- ── host_type ──────────────────────────────────────────────────────
    t:eq("ipv4",     host_type("127.0.0.1"),     "host_type ipv4")
    t:eq("ipv4",     host_type("192.168.1.1"),   "host_type ipv4 lan")
    t:eq("ipv6",     host_type("::1"),           "host_type ipv6 loopback")
    t:eq("ipv6",     host_type("fe80::1"),       "host_type ipv6 linklocal")
    t:eq("hostname", host_type("example.com"),   "host_type hostname")
    t:eq("hostname", host_type("a.b.c.test"),    "host_type hostname tld")

    -- ── split ─────────────────────────────────────────────────────────
    do
        local r = split("a,b,c,d", ",")
        t:eq(4,   #r,    "split count")
        t:eq("a", r[1],  "split [1]")
        t:eq("d", r[4],  "split [4]")
        -- 空分隔符返回原串
        r = split("abc", "")
        t:eq(1,     #r,    "split empty delim count")
        t:eq("abc", r[1],  "split empty delim val")
        -- 末尾空字段
        r = split("a,b,", ",")
        t:eq(3,  #r,   "split trailing empty count")
        t:eq("", r[3], "split trailing empty val")
        -- 无分隔符出现
        r = split("abc", ",")
        t:eq(1,     #r,   "split no match count")
        t:eq("abc", r[1], "split no match val")
    end

    -- ── str_nullorempty ───────────────────────────────────────────────
    t:eq(true,  str_nullorempty(nil),   "str_nullorempty nil")
    t:eq(true,  str_nullorempty(""),    "str_nullorempty empty")
    t:eq(false, str_nullorempty(" "),   "str_nullorempty space")
    t:eq(false, str_nullorempty("abc"), "str_nullorempty non-empty")

    -- ── table_size / table_nullorempty ─────────────────────────────────
    t:eq(0, table_size({}),                       "table_size empty")
    t:eq(3, table_size({1,2,3}),                  "table_size seq")
    t:eq(2, table_size({a=1, b=2}),               "table_size map")
    t:eq(3, table_size({1, x="y", [10]=true}),    "table_size mixed")
    t:eq(true,  table_nullorempty(nil),           "table_nullorempty nil")
    t:eq(true,  table_nullorempty({}),            "table_nullorempty empty")
    t:eq(false, table_nullorempty({1}),           "table_nullorempty seq")
    t:eq(false, table_nullorempty({a=1}),         "table_nullorempty map")

    -- ── randstr ───────────────────────────────────────────────────────
    do
        local s = randstr(32)
        t:check(#s == 32, "randstr 32 length")
        t:check(s:match("^[0-9a-zA-Z]+$") ~= nil, "randstr charset")
        t:check(randstr(16) ~= randstr(16), "randstr random")
        t:eq("", randstr(0), "randstr 0")
    end

    -- ── class（OOP） ──────────────────────────────────────────────────
    do
        local Animal = class("Animal")
        function Animal:ctor(name) self.name = name end
        function Animal:say() return "hi " .. self.name end
        local a = Animal.new("dog")
        t:eq("dog",    a.name,   "class ctor field")
        t:eq("hi dog", a:say(),  "class method")
        t:eq("Animal", a.class.__cname, "class __cname")
    end
    do
        -- 单继承
        local A = class("A")
        function A:ctor() self.x = 1 end
        function A:foo() return "A.foo" end
        local B = class("B", A)
        function B:ctor()
            A.ctor(self)
            self.y = 2
        end
        function B:bar() return "B.bar" end
        local b = B.new()
        t:eq(1, b.x, "inherit field x")
        t:eq(2, b.y, "subclass field y")
        t:eq("A.foo", b:foo(), "inherit method foo")
        t:eq("B.bar", b:bar(), "subclass method bar")
        t:eq(A, b.super, "super points to A")
    end
    do
        -- 变参里夹 nil：ipairs 会停在它上面，后面的父类被静默丢掉，
        -- 而那句 assert 也永远看不到 nil，产出一个"什么都继承不到"的类
        local A = class("A")
        function A:foo() return "A.foo" end
        t:eq(false, pcall(class, "X", nil, A), "父类列表夹 nil 报错而非静默丢弃")
        local Y = class("Y", A)
        t:eq("A.foo", Y.new():foo(), "正常单继承不受影响")
    end

    -- ── dump ──────────────────────────────────────────────────────────
    do
        local s = dump({ a=1, b="hi", c={x=10} })
        t:check(s:find('["a"]', 1, true) ~= nil, "dump key a brackets")
        t:check(s:find('["b"]', 1, true) ~= nil, "dump key b brackets")
        t:check(s:find('"hi"', 1, true) ~= nil, "dump string value quoted")
        -- 序列 table
        s = dump({ 10, 20, 30 })
        t:check(s:find("10", 1, true) ~= nil and s:find("30", 1, true) ~= nil,
                "dump array elements")
        -- 循环引用安全
        local t1 = { x=1 }
        t1.self = t1
        s = dump(t1)
        t:check(s:find("<circular>", 1, true) ~= nil, "dump circular safe")
        -- %c 匹配全部控制符而转义表只有五个：查不到时 gsub 原样保留，
        -- NUL / ESC 就直接落进引号里，输出既 load 不回来也能往日志注入终端转义序列
        s = dump({ k = "a\0b\27c" })
        t:check(nil == s:find("\0", 1, true), "dump 不把 NUL 原样吐出")
        t:check(nil == s:find("\27", 1, true), "dump 不把 ESC 原样吐出")
        t:check(s:find("\\000", 1, true) ~= nil, "NUL 转成 \\ddd")
        t:check(s:find("\\027", 1, true) ~= nil, "ESC 转成 \\ddd")
        -- 五个惯用转义仍走原来的写法
        s = dump({ k = "a\tb\nc\"d\\e" })
        t:check(s:find("\\t", 1, true) ~= nil and s:find("\\n", 1, true) ~= nil,
                "惯用转义不变")
    end

    -- ── lib/log.lua: 级别读取与 log_setlv 同步 ──────────────────────
    do
        local saved = utils.log_getlv()
        -- Lua 侧短路必须现问 C 层，不能缓存：级别是进程级一份，而每个 task 一个 lua_State，
        -- 缓存就成了每 VM 一份，调试台调高只对收到命令的那个 task 生效。
        -- 这里绕过 log_setlv 直接改 C 层（模拟"命令打给了别的 task / C task"），
        -- 再用一个会让 string.format 抛错的调用探测短路有没有放行：
        -- 被短路则 format 根本不执行、不抛；放行则抛
        -- 只降到 WARN 不降到 FATAL：级别是进程级一份，而 runner/reporter 的 FAIL 走 WARN 输出，
        -- 压到 FATAL 会把这几行之间其他 task 的失败明细一起吞掉。WARN 已低于 DEBUG，短路照样验得了
        utils.log_setlv(2)-- WARN
        t:eq(true, pcall(DEBUG, "%d", {}), "低级别下 DEBUG 被短路(未走到 string.format)")
        utils.log_setlv(4)-- 绕过 log_setlv 调到 DEBUG
        t:eq(false, pcall(DEBUG, "%d", {}), "绕过 log_setlv 调高级别后 Lua 侧立刻生效")
        utils.log_setlv(saved)
        -- 调 log_setlv（lib/log.lua 中函数）应同步更新 C 层
        log_setlv(2)
        t:eq(2, utils.log_getlv(), "log_setlv sync to C 层")
        log_setlv(saved)
        t:eq(saved, utils.log_getlv(), "log_setlv restore")
        -- 非法级别（非整数 / 越界）返回 false 且不改变当前级别
        t:eq(false, log_setlv("abc"), "log_setlv reject non-integer")
        t:eq(false, log_setlv(99),    "log_setlv reject out-of-range")
        t:eq(false, log_setlv(-1),    "log_setlv reject negative")
        t:eq(saved, utils.log_getlv(), "log_setlv invalid keeps level")
        t:eq(true,  log_setlv(saved),  "log_setlv valid returns true")
        -- FATAL/ERROR/WARN/INFO/DEBUG 五个全局函数都存在
        t:eq("function", type(FATAL), "FATAL exists")
        t:eq("function", type(ERROR), "ERROR exists")
        t:eq("function", type(WARN),  "WARN exists")
        t:eq("function", type(INFO),  "INFO exists")
        t:eq("function", type(DEBUG), "DEBUG exists")
    end

    -- ── task.msg_release：只认带载荷的消息对象，别的入参什么都不做 ──────────
    do
        local release = task.msg_release
        t:eq(true, pcall(release), "msg_release 无参不报错")
        t:eq(true, pcall(release, nil), "msg_release(nil) 不报错")
        t:eq(true, pcall(release, {}), "msg_release(表) 不报错")
        t:eq(true, pcall(release, 42), "msg_release(数字) 不报错")
        -- 带载荷的用 UDP 自发自收的 RECVFROM：它的释放函数不顺手把 msg.data 置空，重复调全靠 msg_release 自己挡
        local sk = srey.udp(PACK_TYPE.NONE, "127.0.0.1", UDP_PORT)
        if t:check(sk and sk.valid and srey.sock_session(sk), "udp 建 socket 并设会话") then
            local p = "release-me"
            srey.sendto(sk, "127.0.0.1", UDP_PORT, p, #p, 1)
            local msg = srey._coro_wait(sk.skid, srey.MSG_TYPE.RECVFROM, 2000)
            t:eq(srey.MSG_TYPE.RECVFROM, msg.mtype, "等到 RECVFROM")
            local lud = msg.data
            if t:check(nil ~= lud, "RECVFROM 带载荷") then
                -- msg.data 是 light userdata：没有消息元表，不当消息对象
                t:eq(true, pcall(release, lud), "msg_release(light userdata) 不报错")
                t:eq(p, srey.ud_str(msg.udata, msg.size), "传 light userdata 不释放载荷")
                -- 下次挂起、分发返回时 C 侧还会再放一次，已放过的必须是空操作（否则 ASan 报双重释放）
                release(msg)
                t:eq(nil, msg.data, "释放后 msg.data 为 nil")
                t:eq(true, pcall(release, msg), "同一消息再释放一次不报错")
                t:eq(nil, msg.data, "重复释放后 msg.data 仍为 nil")
                t:eq(sk.skid, msg.sess, "释放后其余字段照读")
            end
            srey.close(sk)
        end
        -- 无载荷消息：data 本就为空，调了也不动它
        local sess = srey.id()
        srey.response(srey.task_handle(), 0, sess, ERR_OK)
        local m0 = srey._coro_wait(sess, srey.MSG_TYPE.RESPONSE, 2000)
        t:eq(srey.MSG_TYPE.RESPONSE, m0.mtype, "等到无载荷的 RESPONSE")
        t:eq(true, pcall(release, m0), "msg_release(无载荷消息) 不报错")
        t:eq(nil, m0.data, "无载荷消息 data 仍为 nil")
        t:eq(sess, m0.sess, "无载荷消息其余字段不变")
    end
end)
end)
