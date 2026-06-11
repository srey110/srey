-- seri 绑定层单元测试：pack/unpack 各 Lua 类型 + 嵌套 table + 边界 + 拒绝不支持类型

local srey   = require("lib.srey")
local runner = require("test.runner")
local seri   = require("srey.seri")
local utils  = require("srey.utils")

srey.startup(function()
runner.run("seri", function(t)
    -- 1. 基本类型 round-trip
    local buf, size = seri.pack(nil, true, false, 42, -7, 3.14, "hello")
    t:check(buf ~= nil, "pack returns buffer")
    t:check(type(size) == "number" and size > 0, "size > 0")

    local n, b1, b2, i1, i2, r, s = seri.unpack(buf, size)
    t:eq(nil,   n,  "nil round-trip")
    t:eq(true,  b1, "true round-trip")
    t:eq(false, b2, "false round-trip")
    t:eq(42,    i1, "positive int")
    t:eq(-7,    i2, "negative int")
    t:check(r > 3.13 and r < 3.15, "double round-trip")
    t:eq("hello", s, "string round-trip")
    utils.ud_free(buf)

    -- 2. int 各档边界
    buf, size = seri.pack(0, 255, 256, 65535, 65536, 2147483647, 2147483648, -1, -2147483648)
    local v0, v1, v2, v3, v4, v5, v6, v7, v8 = seri.unpack(buf, size)
    t:eq(0,           v0, "int zero")
    t:eq(255,         v1, "int byte upper")
    t:eq(256,         v2, "int word lower")
    t:eq(65535,       v3, "int word upper")
    t:eq(65536,       v4, "int dword lower")
    t:eq(2147483647,  v5, "int dword upper")
    t:eq(2147483648,  v6, "int qword crossover")
    t:eq(-1,          v7, "int neg dword")
    t:eq(-2147483648, v8, "int dword negative bound")
    utils.ud_free(buf)

    -- 3. 数组 table
    do
        local arr = {10, 20, 30, "four", true, 5.5}
        buf, size = seri.pack(arr)
        local arr2 = seri.unpack(buf, size)
        t:eq(6,      #arr2,   "array length")
        t:eq(10,     arr2[1], "array [1]")
        t:eq("four", arr2[4], "array [4] string")
        t:eq(true,   arr2[5], "array [5] bool")
        t:check(arr2[6] > 5.4 and arr2[6] < 5.6, "array [6] double")
        utils.ud_free(buf)
    end

    -- 4. 纯 hash table
    do
        local h = {a=1, b="bee", c=true}
        buf, size = seri.pack(h)
        local h2 = seri.unpack(buf, size)
        t:eq(1,     h2.a, "hash a")
        t:eq("bee", h2.b, "hash b")
        t:eq(true,  h2.c, "hash c")
        utils.ud_free(buf)
    end

    -- 5. 混合（数组段 + hash 段）
    do
        local m = {100, 200, name="kira", flag=true}
        buf, size = seri.pack(m)
        local m2 = seri.unpack(buf, size)
        t:eq(100,    m2[1],    "mixed [1]")
        t:eq(200,    m2[2],    "mixed [2]")
        t:eq("kira", m2.name,  "mixed name")
        t:eq(true,   m2.flag,  "mixed flag")
        utils.ud_free(buf)
    end

    -- 6. 嵌套 table
    do
        local nested = {x=1, sub={y=2, list={9, 8, 7}}}
        buf, size = seri.pack(nested)
        local n2 = seri.unpack(buf, size)
        t:eq(1, n2.x,             "nested x")
        t:eq(2, n2.sub.y,         "nested sub.y")
        t:eq(3, #n2.sub.list,     "nested list length")
        t:eq(8, n2.sub.list[2],   "nested list [2]")
        utils.ud_free(buf)
    end

    -- 7. 长字符串（>=32 字节走 long string 路径）
    do
        local long = string.rep("ABCD", 20)  -- 80 字节
        buf, size = seri.pack(long)
        local long2 = seri.unpack(buf, size)
        t:eq(long, long2, "long string round-trip")
        utils.ud_free(buf)
    end

    -- 8. 长数组（>=31 走 long array cookie 转义）
    do
        local longarr = {}
        local i
        for i = 1, 50 do
            longarr[i] = i * 2
        end
        buf, size = seri.pack(longarr)
        local longarr2 = seri.unpack(buf, size)
        t:eq(50,  #longarr2,    "long array length")
        t:eq(2,   longarr2[1],  "long array [1]")
        t:eq(100, longarr2[50], "long array [50]")
        utils.ud_free(buf)
    end

    -- 9. unpack 支持 string 入参（不传 size）
    do
        local b, sz = seri.pack(99, "two")
        local s_view = utils.ud_str(b, sz)
        utils.ud_free(b)
        local x, y = seri.unpack(s_view)
        t:eq(99,    x, "unpack string arg [1]")
        t:eq("two", y, "unpack string arg [2]")
    end

    -- 10. 空表 round-trip
    do
        buf, size = seri.pack({})
        local empty = seri.unpack(buf, size)
        t:eq(0, #empty, "empty table array len")
        t:eq("table", type(empty), "empty table type")
        utils.ud_free(buf)
    end

    -- 11. 不支持类型 raise error
    do
        local ok, err = pcall(function() seri.pack(function() end) end)
        t:check(not ok, "pack function rejected")
        t:check(type(err) == "string" and err:find("unsupported"),
            "pack function error message")
    end

    -- 12. 大量顶层值 round-trip（顶层 unpack 逐项 checkstack，超 LUA_MINSTACK 不溢出）
    do
        local args = {}
        local i
        for i = 1, 300 do
            args[i] = i
        end
        buf, size = seri.pack(table.unpack(args))
        local rets = { seri.unpack(buf, size) }
        utils.ud_free(buf)
        t:eq(300, #rets,     "many top-level values count")
        t:eq(1,   rets[1],   "many top-level [1]")
        t:eq(300, rets[300], "many top-level [300]")
    end

    -- 13. 顶层多 nil 裸字节流不溢出栈（每个 \0 字节 = 一个 SERI_ITEM_NIL）
    do
        local cnt = select("#", seri.unpack(string.rep("\0", 300)))
        t:eq(300, cnt, "many top-level nils count")
    end

    -- 14. 恶意流谎报超大 array_n：不预分配天量内存，读到流尾即报 malformed（F-SERI-2）
    do
        -- 0xFE=ARRAY+escape，0x22=NUMBER DWORD，后随 u32 小端 0x10000000(2.68 亿) 声称的数组长度，但无元素数据
        local bad = string.char(0xFE, 0x22, 0x00, 0x00, 0x00, 0x10)
        local ok = pcall(seri.unpack, bad)
        t:check(not ok, "malicious huge array_n rejected as malformed")
    end
end)
end)
