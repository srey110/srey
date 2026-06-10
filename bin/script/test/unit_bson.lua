-- bson 绑定层单元测试：encode/decode + wrappers (mkoid/mkdate/mkbinary/mkint64) + iter

local srey   = require("lib.srey")
local runner = require("test.runner")
local bson   = require("lib.bson")

srey.startup(function()
runner.run("bson", function(t)
    -- 1. 基本类型 round-trip
    local b = bson.encode({ i32=100, i64=3000000000, dbl=3.14, str="hello", flag=true })
    t:check(b ~= nil, "encode basic")
    local tb = bson.decode(b)
    t:eq(100,   tb.i32,  "decode i32")
    t:eq("hello", tb.str, "decode str")
    t:eq(true,  tb.flag, "decode bool")
    t:check(tb.dbl > 3.13 and tb.dbl < 3.15, "decode double")
    t:check(tb.i64 ~= nil and tb.i64:val() == 3000000000, "decode i64 wrapper")

    -- 2. INT32 / INT64 边界
    b = bson.encode({ max32=2147483647, min32=-2147483648, over=bson.mkint64(2147483648) })
    tb = bson.decode(b)
    t:eq(2147483647,  tb.max32, "INT32 max")
    t:eq(-2147483648, tb.min32, "INT32 min")
    t:eq("number",    type(tb.max32), "INT32 decoded as number")
    t:check(tb.over ~= nil and tb.over:val() == 2147483648, "mkint64 forced INT64")

    -- 3. nil 字段不写入
    b = bson.encode({ a=1, b=nil, c=2 })
    tb = bson.decode(b)
    t:eq(1, tb.a, "null skip a")
    t:eq(nil, tb.b, "null skip b absent")
    t:eq(2, tb.c, "null skip c")

    -- 4. mkoid round-trip
    local raw = bson.oid()
    b = bson.encode({ _id = bson.mkoid(raw) })
    tb = bson.decode(b)
    t:check(tb._id ~= nil and tb._id:data() == raw, "mkoid round-trip")

    -- 5. mkdate round-trip
    b = bson.encode({ created = bson.mkdate(1700000000000) })
    tb = bson.decode(b)
    t:check(tb.created ~= nil and tb.created:ms() == 1700000000000, "mkdate round-trip")

    -- 6. mkbinary round-trip
    local payload = "\x01\x02\x03\x04"
    b = bson.encode({ blob = bson.mkbinary(bson.SUBTYPE.BINARY, payload) })
    tb = bson.decode(b)
    t:check(tb.blob ~= nil, "mkbinary decoded")
    t:eq(bson.SUBTYPE.BINARY, tb.blob:subtype(), "mkbinary subtype")
    t:eq(payload, tb.blob:data(), "mkbinary data")

    -- 7. 嵌套 document（字符串 key）
    b = bson.encode({ meta = { x=10, y=20 } })
    tb = bson.decode(b)
    t:eq("table", type(tb.meta), "nested doc type")
    t:eq(10, tb.meta.x, "nested doc x")
    t:eq(20, tb.meta.y, "nested doc y")

    -- 8. 嵌套 array（序列 table）
    b = bson.encode({ tags = { "a", "b", "c" } })
    tb = bson.decode(b)
    t:eq("a", tb.tags[1], "nested array [1]")
    t:eq("b", tb.tags[2], "nested array [2]")
    t:eq("c", tb.tags[3], "nested array [3]")

    -- 9. decode 接受 lightuserdata + size
    b = bson.encode({ n = 7 })
    local ptr, sz = b:data()
    tb = bson.decode(ptr, sz)
    t:eq(7, tb.n, "decode from lightuserdata")

    -- 10. 空表
    b = bson.encode({})
    t:check(b:complete(), "empty table bson_ctx complete")
    tb = bson.decode(b)
    local empty = true
    for _ in pairs(tb) do empty = false end
    t:check(empty, "empty table decoded empty")

    -- 11. 深层嵌套
    b = bson.encode({
        outer = { list = { { id=1, name="foo" }, { id=2, name="bar" } } }
    })
    tb = bson.decode(b)
    t:eq(1,     tb.outer.list[1].id,   "deep nested [1].id")
    t:eq("foo", tb.outer.list[1].name, "deep nested [1].name")
    t:eq("bar", tb.outer.list[2].name, "deep nested [2].name")

    -- 12. iter 一次性消费契约：iter.new 内部强制 reset doc.offset=0 让 init 能正确读 doclens
    -- 但 iter:next 会推进底层 offset，用完后原 bson 的 :data() 失效——所以要么先 :data() 后 iter，
    -- 要么干脆用 bson.decode 转 table
    do
        local b1 = bson.encode({ k1 = "v1", k2 = 42 })
        local iter = bson.iter.new(b1)
        local nkeys = 0
        local seen_keys, seen_types, seen_vals = {}, {}, {}
        while iter:next() do
            nkeys = nkeys + 1
            local key = iter:key()
            local ty = iter:type()
            seen_keys[key]  = true
            seen_types[ty]  = true
            if bson.TYPE.UTF8 == ty then
                seen_vals[key] = iter:utf8()
            elseif bson.TYPE.INT32 == ty then
                seen_vals[key] = iter:int32()
            end
        end
        t:eq(2, nkeys, "iter visits 2 fields")
        t:eq(true, seen_keys.k1, "iter key k1")
        t:eq(true, seen_keys.k2, "iter key k2")
        t:eq(true, seen_types[bson.TYPE.UTF8],  "iter type UTF8 visited")
        t:eq(true, seen_types[bson.TYPE.INT32], "iter type INT32 visited")
        t:eq("v1", seen_vals.k1, "iter utf8 value")
        t:eq(42,   seen_vals.k2, "iter int32 value")
    end
    do
        -- reset 后可重新遍历同一 iter
        local b2 = bson.encode({ x = 1, y = 2, z = 3 })
        local iter = bson.iter.new(b2)
        local first = 0
        while iter:next() do first = first + 1 end
        t:eq(3, first, "iter first pass count")
        iter:reset()
        local second = 0
        while iter:next() do second = second + 1 end
        t:eq(3, second, "iter reset re-iterate count")
    end
    do
        -- find: 跳到指定 key，可读 type/key/value
        local b3 = bson.encode({ name = "alice", age = 30 })
        local iter = bson.iter.new(b3)
        t:eq(true, iter:find("name"), "iter find name")
        t:eq(bson.TYPE.UTF8, iter:type(), "iter find name type")
        t:eq("alice", iter:utf8(), "iter find name value")

        iter = bson.iter.new(b3)
        t:eq(true, iter:find("age"), "iter find age")
        t:eq(30, iter:int32(), "iter find age value")

        iter = bson.iter.new(b3)
        t:eq(false, iter:find("missing"), "iter find missing returns false")
    end

    -- 13. 低阶 builder：double / utf8 / int32 / int64 / bool / null / date / minkey / maxkey
    do
        local b = bson.new()
        b:double("d", 1.5)
        b:utf8("s", "world")
        b:int32("i32", -42)
        b:int64("i64", 9000000000)
        b:bool("ok", true)
        b:null("nul")
        b:date("dt", 1700000000123)
        b:minkey("mn")
        b:maxkey("mx")
        b["end"](b)
        t:check(b:complete(), "builder complete after end")
        local tb = bson.decode(b)
        t:check(tb.d > 1.49 and tb.d < 1.51, "builder double round-trip")
        t:eq("world", tb.s, "builder utf8 round-trip")
        t:eq(-42, tb.i32, "builder int32 round-trip")
        t:check(tb.i64 ~= nil and tb.i64:val() == 9000000000, "builder int64 round-trip")
        t:eq(true, tb.ok, "builder bool true round-trip")
        t:eq(nil, tb.nul, "builder null decoded as nil")
        t:check(tb.dt ~= nil and tb.dt:ms() == 1700000000123, "builder date round-trip")
        local s = b:tostring()
        t:check(type(s) == "string" and #s > 0, "builder :tostring non-empty")
    end

    -- 14. doc_begin / arr_begin / end 嵌套
    do
        local b = bson.new()
        b:doc_begin("meta")
            b:int32("v", 7)
            b:utf8("name", "alice")
            b["end"](b)
        b:arr_begin("tags")
            b:utf8("0", "x")
            b:utf8("1", "y")
            b:utf8("2", "z")
            b["end"](b)
        b["end"](b)
        t:check(b:complete(), "nested builder complete")
        local tb = bson.decode(b)
        t:eq(7, tb.meta.v, "nested doc int32")
        t:eq("alice", tb.meta.name, "nested doc utf8")
        t:eq("x", tb.tags[1], "nested arr [1]")
        t:eq("z", tb.tags[3], "nested arr [3]")
    end

    -- 15. append_doc / append_arr：把已序列化子 bson 内嵌
    do
        local sub = bson.encode({ a = 1, b = "two" })
        local subptr, subsz = sub:data()
        local arr = bson.encode({ "p", "q" })
        local arrptr, arrsz = arr:data()

        -- string 形式
        local b1 = bson.new()
        local ssub = srey.ud_str(subptr, subsz)
        local sarr = srey.ud_str(arrptr, arrsz)
        b1:append_doc("sub", ssub)
        b1:append_arr("arr", sarr)
        b1["end"](b1)
        t:check(b1:complete(), "append_doc/arr string form complete")
        local tb1 = bson.decode(b1)
        t:eq(1, tb1.sub.a, "append_doc string sub.a")
        t:eq("two", tb1.sub.b, "append_doc string sub.b")
        t:eq("p", tb1.arr[1], "append_arr string arr[1]")
        t:eq("q", tb1.arr[2], "append_arr string arr[2]")

        -- lightuserdata 形式
        local b2 = bson.new()
        b2:append_doc("sub", subptr, subsz)
        b2:append_arr("arr", arrptr, arrsz)
        b2["end"](b2)
        local tb2 = bson.decode(b2)
        t:eq(1, tb2.sub.a, "append_doc lud sub.a")
        t:eq("p", tb2.arr[1], "append_arr lud arr[1]")
    end

    -- 16. cat：把另一已完成 bson 内容拼到本 builder
    do
        local src = bson.encode({ k1 = 100, k2 = "src" })
        local srcptr, srcsz = src:data()

        -- string 形式
        local s = srey.ud_str(srcptr, srcsz)
        local b1 = bson.new()
        b1:int32("pre", 1)
        b1:cat(s)
        b1["end"](b1)
        local tb1 = bson.decode(b1)
        t:eq(1, tb1.pre, "cat string preserves pre")
        t:eq(100, tb1.k1, "cat string merges k1")
        t:eq("src", tb1.k2, "cat string merges k2")

        -- lightuserdata 形式
        local b2 = bson.new()
        b2:cat(srcptr, srcsz)
        b2["end"](b2)
        local tb2 = bson.decode(b2)
        t:eq(100, tb2.k1, "cat lud k1")
    end

    -- 17. binary builder + iter:binary
    do
        local payload = "\xde\xad\xbe\xef"
        local b = bson.new()
        b:binary("bin", bson.SUBTYPE.UUID, payload)
        b["end"](b)
        local iter = bson.iter.new(b)
        t:eq(true, iter:find("bin"), "iter find bin")
        local subtype, ptr, sz = iter:binary()
        t:eq(bson.SUBTYPE.UUID, subtype, "iter:binary subtype")
        t:eq(#payload, sz, "iter:binary size")
        t:eq(payload, srey.ud_str(ptr, sz), "iter:binary data")
    end

    -- 18. oid builder + iter:oid
    do
        local raw = bson.oid()
        local b = bson.new()
        b:oid("_id", raw)
        b["end"](b)
        local iter = bson.iter.new(b)
        t:eq(true, iter:find("_id"), "iter find _id")
        t:eq(raw, iter:oid(), "iter:oid round-trip")
    end

    -- 18a. bson:oid 长度校验：非 12 字节字符串必须 argcheck 拒绝（防 OOB 读 12 字节）
    do
        local b = bson.new()
        local ok = pcall(function() b:oid("k", "short") end)
        t:eq(false, ok, "短 oid 字符串被 argcheck 拒绝")
        local ok2 = pcall(function() b:oid("k", string.rep("x", 13)) end)
        t:eq(false, ok2, "长 oid 字符串被 argcheck 拒绝")
        local ok3 = pcall(function() b:oid("k", "") end)
        t:eq(false, ok3, "空 oid 字符串被 argcheck 拒绝")
        local ok4 = pcall(function() b:oid("k", string.rep("x", 12)) end)
        t:eq(true, ok4, "12 字节 oid 字符串接受")
    end

    -- 19. iter:document / iter:array
    do
        local b = bson.encode({ doc = { x = 1 }, arr = { 10, 20 } })
        local iter = bson.iter.new(b)
        t:eq(true, iter:find("doc"), "iter find doc")
        local dptr, dsz = iter:document()
        t:check(dptr ~= nil and dsz > 5, "iter:document returns data")
        local sub = bson.decode(dptr, dsz)
        t:eq(1, sub.x, "iter:document decoded sub")

        iter = bson.iter.new(b)
        t:eq(true, iter:find("arr"), "iter find arr")
        local aptr, asz = iter:array()
        t:check(aptr ~= nil and asz > 5, "iter:array returns data")
        -- iter:array 返回的是 BSON 数组 wire（key="0","1"...）；顶层 bson.decode 按 doc 解读保留字符串 key
        local arr = bson.decode(aptr, asz)
        t:eq(10, arr["0"], "iter:array decoded [0]")
        t:eq(20, arr["1"], "iter:array decoded [1]")
    end

    -- 20. iter:bool / iter:date / iter:int64 / iter:isnull
    do
        local b = bson.new()
        b:bool("flag", false)
        b:date("when", 1700000000456)
        b:int64("big", 9000000001)
        b:null("z")
        b["end"](b)
        local iter = bson.iter.new(b)
        t:eq(true, iter:find("flag"), "iter find flag")
        t:eq(false, iter:bool(), "iter:bool false")
        iter = bson.iter.new(b)
        t:eq(true, iter:find("when"), "iter find when")
        t:eq(1700000000456, iter:date(), "iter:date ms")
        iter = bson.iter.new(b)
        t:eq(true, iter:find("big"), "iter find big")
        t:eq(9000000001, iter:int64(), "iter:int64 value")
        iter = bson.iter.new(b)
        t:eq(true, iter:find("z"), "iter find z")
        t:eq(true, iter:isnull(), "iter:isnull true on BSON_NULL")
    end

    -- 21. iter:regex / iter:jscode / iter:timestamp
    do
        local b = bson.new()
        b:regex("re", "^hello.*$", "im")
        b:jscode("js", "function(){return 1;}")
        b:timestamp("ts", 1700000000, 7)
        b["end"](b)

        local iter = bson.iter.new(b)
        t:eq(true, iter:find("re"), "iter find re")
        local pat, opt = iter:regex()
        t:eq("^hello.*$", pat, "iter:regex pattern")
        t:eq("im", opt, "iter:regex options")

        iter = bson.iter.new(b)
        t:eq(true, iter:find("js"), "iter find js")
        t:eq("function(){return 1;}", iter:jscode(), "iter:jscode code")

        iter = bson.iter.new(b)
        t:eq(true, iter:find("ts"), "iter find ts")
        local ts, inc = iter:timestamp()
        t:eq(1700000000, ts, "iter:timestamp ts")
        t:eq(7, inc, "iter:timestamp inc")
    end

    -- 22. 顶层辅助：empty / tostring2 / type_tostring / subtype_tostring
    do
        local eptr, esz = bson.empty()
        t:check(eptr ~= nil and esz == 5, "bson.empty returns 5-byte empty doc")
        local etxt = bson.tostring2(eptr, esz)
        t:check(type(etxt) == "string", "tostring2 from lightuserdata")

        local b = bson.encode({ k = 1 })
        local bptr, bsz = b:data()
        local s = srey.ud_str(bptr, bsz)
        local stxt = bson.tostring2(s)
        t:check(type(stxt) == "string" and #stxt > 0, "tostring2 from string")

        t:eq("double",   bson.type_tostring(bson.TYPE.DOUBLE),     "type_tostring DOUBLE")
        t:eq("string",   bson.type_tostring(bson.TYPE.UTF8),       "type_tostring UTF8")
        t:eq("int",      bson.type_tostring(bson.TYPE.INT32),      "type_tostring INT32")
        t:eq("uuid",     bson.subtype_tostring(bson.SUBTYPE.UUID), "subtype_tostring UUID")
    end
end)
end)
