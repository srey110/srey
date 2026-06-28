-- Lua 层单元测试：lib/utils.lua（split/host_type/table_size/randstr/class/dump 等）
--                  + lib/log.lua（_curlv 缓存 / log_setlv round-trip）

local srey   = require("lib.srey")
local runner = require("test.runner")
local utils  = require("srey.utils")

srey.startup(function()
runner.run("lua_layer", function(t)
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
    end

    -- ── lib/log.lua: _curlv 缓存与 log_setlv 同步 ──────────────────────
    do
        local saved = utils.log_getlv()
        -- 调 log_setlv（lib/log.lua 中函数）应同步更新 C 层
        log_setlv(0)
        t:eq(0, utils.log_getlv(), "log_setlv sync to C 层 (FATAL only)")
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
end)
end)
