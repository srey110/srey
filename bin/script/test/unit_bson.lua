-- bson 绑定层单元测试：encode/decode + wrappers (mkoid/mkdate/mkbinary/mkint64) + iter

local srey   = require("lib.srey")
local runner = require("test.runner")
local bson   = require("lib.bson")
local yyjson = require("yyjson")-- yyjson.null 是一个 NULL lightuserdata，用来测空指针拒收

srey.startup(function()
runner.run(function(t)
    -- 1. 基本类型 round-trip
    local b = bson.encode({ i32=100, i64=3000000000, dbl=3.14, str="hello", flag=true })
    t:check(b ~= nil, "encode basic")
    local tb = bson.decode(b)
    t:eq(100,   tb.i32,  "decode i32")
    t:eq("hello", tb.str, "decode str")
    t:eq(true,  tb.flag, "decode bool")
    t:check(tb.dbl > 3.13 and tb.dbl < 3.15, "decode double")
    t:check(tb.i64 ~= nil and tb.i64:val() == 3000000000, "decode i64 wrapper")

    -- 1b. 结构非法的文档：报错而不是静默交出前缀
    do
        local full = bson.encode({ a = 1, b = "xyz", c = true })
        local ptr, sz = full:data()
        local raw = srey.ud_str(ptr, sz)
        -- 头 4 字节仍声明 sz，缓冲却少 3 字节：遍历会在耗尽前撞不到 EOD
        local cut = raw:sub(1, sz - 3)
        local ok, err = pcall(bson.decode, cut)
        t:eq(false, ok, "截断文档 decode 报错而不是返回半截表")
        t:check(type(err) == "string" and nil ~= err:find("malformed", 1, true),
                "报错文案点明文档非法")
        -- iter 侧同一份缓冲：next() 走完后 error() 认得出，用来区分"读完"和"文档坏了"。
        -- bson.new(ptr, lens) 是只读模式，头里仍声明 sz 而实际只给 sz-3
        local it = bson.iter.new(bson.new(ptr, sz - 3))
        while it:next() do end
        t:eq(true, it:error(), "iter:error() 对截断文档返回 true")
        -- 阳性对照：完整文档遍历完 error() 为 false
        local it2 = bson.iter.new(bson.new(ptr, sz))
        while it2:next() do end
        t:eq(false, it2:error(), "iter:error() 对完整文档返回 false")
        -- 结构完好、只是撞上本实现不认识的类型字节（0x0E symbol，废弃类型不在 bson_type 里）：
        -- BSON 元素不自带长度，认不出类型就算不出边界，后面一律读不到，故与文档非法同样报错。
        -- 前 7 字节是合法的 int32 a=1，证明卡住的确实是后面那个 symbol 元素
        local sym = string.char(0x15, 0, 0, 0,
                                0x10, 0x61, 0, 1, 0, 0, 0,
                                0x0E, 0x73, 0, 2, 0, 0, 0, 0x78, 0,
                                0)
        local ok2, err2 = pcall(bson.decode, sym)
        t:eq(false, ok2, "含未支持类型的文档 decode 报错而不是丢掉后半截")
        t:check(type(err2) == "string" and nil ~= err2:find("unsupported", 1, true),
                "报错文案点出可能是未支持的类型")
    end

    -- 1c. 编码侧的四道拒收：混合表、内嵌 NUL 的 key、非法子文档、NULL 指针
    do
        -- "洞的个数 == 额外命名 key 的个数" 时旧实现把混合表误判成纯序列，
        -- 按下标写出，name 被静默丢掉
        local mixed = {}
        for i = 1, 10 do mixed[i] = i end
        mixed[4] = nil
        mixed.name = "srey"
        local mb = bson.encode(mixed)
        local mt = bson.decode(mb)
        t:eq("srey", mt.name, "混合表按 document 编码，命名 key 不丢")
        t:eq(1, mt["1"], "混合表的整数 key 编成字符串 key")

        -- key 含内嵌 NUL：e_name 是 cstring，装不下就得拒，不能截断成同一个名字
        t:eq(false, pcall(bson.encode, { ["a\0b"] = 1 }), "encode: key 含 NUL 被拒")
        local kb = bson.new()
        t:eq(false, pcall(kb.utf8, kb, "a\0b", "v"), "b:utf8: key 含 NUL 被拒")

        -- append_doc / append_arr 的子文档必须完整，否则写出只有 type+key 的坏元素
        local sub = bson.encode({ x = 1 })
        local sptr, ssz = sub:data()
        local sraw = srey.ud_str(sptr, ssz)
        local ab = bson.new()
        t:eq(true, pcall(ab.append_doc, ab, "d", sraw), "append_doc: 完整子文档照常接受")
        t:eq(false, pcall(ab.append_doc, ab, "d", ""), "append_doc: 空缓冲被拒")
        t:eq(false, pcall(ab.append_doc, ab, "d", sraw:sub(1, ssz - 1)), "append_doc: 截断子文档被拒")
        t:eq(false, pcall(ab.append_arr, ab, "a", "abc"), "append_arr: 不足 5 字节被拒")

        -- NULL lightuserdata：bson.new 会造出永远闭合不了的对象，oid 会写出无 OID 体的元素
        t:eq(false, pcall(bson.new, yyjson.null, 1024), "bson.new: NULL 指针被拒")
        local ob = bson.new()
        t:eq(false, pcall(ob.oid, ob, "k", yyjson.null), "b:oid: NULL 指针被拒")
        -- binary/cat/mkbinary 走 lpub_check_buf：长度非 0 时拒收空指针（否则 binary 会写出
        -- 声明了 N 字节却一字节没有的元素，整篇文档从这里错位），长度 0 视作空缓冲放行
        t:eq(false, pcall(ob.binary, ob, "k", 0, yyjson.null, 16), "b:binary: NULL 指针被拒")
        t:eq(false, pcall(ob.cat, ob, yyjson.null, 16), "b:cat: NULL 指针被拒")
        t:eq(false, pcall(bson.mkbinary, 0, yyjson.null, 16), "mkbinary: NULL 指针被拒")
        local okempty = pcall(bson.mkbinary, 0, yyjson.null, 0)
        t:eq(true, okempty, "mkbinary: NULL 指针 + 0 长度放行")
    end

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
        -- lightuserdata 分支早先只挡 NULL，短缓冲会被固定读满 12 字节，越界字节直接进文档
        local eptr, esz = bson.empty()
        t:eq(false, pcall(function() b:oid("k", eptr) end), "lightuserdata oid 不带长度被拒")
        t:eq(false, pcall(function() b:oid("k", eptr, esz) end), "5 字节 lightuserdata oid 被拒")
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

        -- 类型不符时失败与成功的返回值个数必须一致（各 2 个），否则按元数取值的调用方拿到的
        -- 不是 nil 而是"没有这个返回值"
        local b2 = bson.encode({ n = 1 })
        local it2 = bson.iter.new(b2)
        t:eq(true, it2:find("n"), "iter find n")
        t:eq(2, select("#", it2:array()), "iter:array 类型不符时也返 2 个值")
        t:eq(nil, (it2:array()), "iter:array 类型不符时首值为 nil")
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

    -- 19a. 解码深度超 BSON_MAX_DEPTH(18) 要报错而不是交出空表：
    -- 空表让调用方分不清"这个子文档本来就是空的"和"从这层起全丢了"，
    -- 与同函数畸形文档路径同处置。encode 侧撞 ASSERTAB 造不出这种输入，只能手拼 wire
    do
        -- 一篇 BSON = int32 总长 + 元素 + 0x00；一层嵌套的元素是 0x03 + "a\0" + 子文档
        local function nest(n)
            local doc = string.pack("<i4", 5) .. "\0"-- 空文档 {}
            local body
            for _ = 1, n do
                body = "\3a\0" .. doc
                doc = string.pack("<i4", 4 + #body + 1) .. body .. "\0"
            end
            return doc
        end
        local shallow = nest(3)
        local ok, tb = pcall(bson.decode, shallow, #shallow)
        t:eq(true, ok, "手拼的 3 层文档解得开(证明 wire 拼法没错)")
        t:check(ok and "table" == type(tb.a) and "table" == type(tb.a.a), "3 层嵌套结构正确")
        local deep = nest(25)
        t:eq(false, pcall(bson.decode, deep, #deep), "25 层嵌套被拒而不是静默返空表")
    end

    -- 20a. regex 的 pattern / options 是 BSON cstring，内嵌 NUL 只能拒不能截
    -- （同 key，截断会让两个不同模式塌缩成一个且无报错）
    do
        local b = bson.new()
        t:eq(false, pcall(function() b:regex("k", "^a\0b", "i") end), "regex pattern 含 NUL 被拒")
        t:eq(false, pcall(function() b:regex("k", "^ab", "i\0m") end), "regex options 含 NUL 被拒")
        t:eq(true, pcall(function() b:regex("k", "^ab", "im") end), "正常 regex 接受")
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

        -- jscode 与 utf8 一样按长度取：Lua 字符串能装 NUL，用 lua_pushstring 会截在 NUL 处
        local nb = bson.new()
        nb:jscode("j", "a\0b")
        nb:utf8("s", "x\0y")
        nb["end"](nb)
        local nit = bson.iter.new(nb)
        t:eq(true, nit:find("j"), "iter find 含 NUL 的 js")
        t:eq("a\0b", nit:jscode(), "iter:jscode 内嵌 NUL 不截断")
        nit = bson.iter.new(nb)
        t:eq(true, nit:find("s"), "iter find 含 NUL 的 utf8")
        t:eq("x\0y", nit:utf8(), "iter:utf8 内嵌 NUL 不截断")

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

    -- 23. bson.new(data, size) 只读对象：写入方法被拒而非 abort 进程，读取 / 迭代 / decode 照常
    do
        local w = bson.encode({ a = 1, s = "x" })
        local ptr, sz = w:data()
        t:eq("_bson_reader", getmetatable(bson.new(ptr, sz)), "只读对象挂独立元表且 __metatable 已保护")
        t:eq("_bson_ctx",    getmetatable(w),                 "可写对象仍挂 MT_BSON")

        -- 三类写入方法各取一个：标量 / 嵌套 / 拼接
        local r = bson.new(ptr, sz)
        local ok, err = pcall(function() r:int32("k", 1) end)
        t:eq(false, ok, "只读对象 :int32 被拒")
        t:check(type(err) == "string" and nil ~= err:find("read%-only"), "错误信息点明只读")
        t:eq(false, pcall(function() r:doc_begin("d") end), "只读对象 :doc_begin 被拒")
        t:eq(false, pcall(function() r:cat(ptr, sz) end),   "只读对象 :cat 被拒")
        t:eq(false, pcall(function() r:complete() end),     "只读对象无 :complete（写入方专属概念）")

        -- :data 取长走 doc.size 而非恒为 0 的 doc.offset
        local rptr, rsz = r:data()
        t:check(rptr ~= nil and rsz == sz, "只读对象 :data 返回原缓冲与真实长度")
        t:check(type(r:tostring()) == "string", "只读对象 :tostring 可用")

        local iter = bson.iter.new(r)
        t:eq(true, iter:find("s"), "只读对象可交给 bson.iter.new")
        t:eq("x",  iter:utf8(),    "只读对象经 iter 读值正确")
        local rptr2, rsz2 = r:data()
        t:check(rptr2 == rptr and rsz2 == sz, "iter 推进 offset 后 :data 长度不变")

        t:eq(1, bson.decode(bson.new(ptr, sz)).a, "只读对象可交给 bson.decode")
    end

    -- 24. binary 长度越界：lightuserdata 分支的 lens 直接参与 userdata 尺寸计算，
    --     负数转 size_t 后会让加法回绕出一个装不下头部的小块，须在入口就拒掉
    do
        local src = bson.encode({ a = 1 })
        local ptr, sz = src:data()

        t:eq(false, pcall(function() return bson.mkbinary(0, ptr, -1) end),         "mkbinary 负长度被拒")
        t:eq(false, pcall(function() return bson.mkbinary(0, ptr, 0x80000000) end), "mkbinary 超 INT32_MAX 被拒")
        local ok, err = pcall(function() return bson.mkbinary(0, ptr, -1) end)
        t:check(type(err) == "string" and nil ~= err:find("out of range"), "错误信息点明长度越界")

        -- 同形状的写入方法一并挡住，不再落到 bson_append_binary 的断言上 abort 进程
        local w = bson.new()
        t:eq(false, pcall(function() w:binary("k", 0, ptr, -1) end), "b:binary 负长度被拒")
        t:eq(false, pcall(function() w:binary("k", 0, ptr, 0x80000000) end), "b:binary 超 INT32_MAX 被拒")

        -- 合法路径不受影响：字符串分支自带长度，lightuserdata 分支按给定长度截取
        local bin = bson.mkbinary(bson.SUBTYPE.BINARY, "abc")
        t:eq("abc", bin:data(), "mkbinary 字符串分支照常")
        t:eq(0, #bson.mkbinary(0, ptr, 0):data(), "mkbinary 零长度合法")
        t:eq(sz, #bson.mkbinary(0, ptr, sz):data(), "mkbinary lightuserdata 分支照常")
        ok = pcall(function() w:binary("k", 0, ptr, sz) end)
        t:eq(true, ok, "b:binary 合法长度照常")
    end

    -- 25. iter 与源 bson 的生命周期：iter 的 val / doc / nested_doc 全是指向源缓冲的裸指针，
    --     源对象 :free() 后必须报错，不能继续读已释放的堆
    do
        local b = bson.encode({ s = "hello", n = 42, sub = { x = 7 } })
        local it = bson.iter.new(b)
        t:eq(true, it:next(), "free 前 iter:next 正常")

        -- find 走点分路径会把 iter->doc 指向 nested_doc（源缓冲的别名视图），
        -- 源缓冲释放后它仍是非 NULL 的悬垂指针，故判活只能查源对象自己
        local it2 = bson.iter.new(bson.encode({ sub = { x = 7 } }))
        t:eq(true, it2:find("sub.x"), "free 前点分 find 正常")

        b:free()
        t:eq(false, pcall(function() return it:next() end),     "free 后 iter:next 被拒")
        t:eq(false, pcall(function() return it:key() end),      "free 后 iter:key 被拒")
        t:eq(false, pcall(function() return it:utf8() end),     "free 后 iter:utf8 被拒")
        t:eq(false, pcall(function() return it:type() end),     "free 后 iter:type 被拒")
        t:eq(false, pcall(function() return it:document() end), "free 后 iter:document 被拒")
        t:eq(false, pcall(function() return it:find("s") end),  "free 后 iter:find 被拒")
        t:eq(false, pcall(function() it:reset() end),           "free 后 iter:reset 被拒")
        local ok, err = pcall(function() return it:utf8() end)
        t:check(type(err) == "string" and nil ~= err:find("freed"), "错误信息点明源已释放")

        -- 已 free 的对象不能再造 iter
        t:eq(false, pcall(function() return bson.iter.new(b) end), "free 后 iter.new 被拒")
        -- 另一个 iter 的源没被释放，不受牵连
        t:eq(7, it2:int32(), "未释放的源上 iter 照常可读")
    end

    -- 26. 未闭合文档不得交出数据：bson.new() 建的是隐式顶层文档(depth=1)，首 4 字节长度前缀
    --     要等 end() 才写；此前那 4 字节是 MALLOC 来的未初始化堆
    do
        local b = bson.new()
        b:int32("a", 1)
        t:eq(false, b:complete(), "未 end() 时 complete 为假")

        local ok, err = pcall(function() return b:data() end)
        t:eq(false, ok, "未闭合时 :data 被拒")
        t:check(type(err) == "string" and nil ~= err:find("not complete"), "错误信息点明未闭合")
        t:eq(false, pcall(function() return b:tostring() end),   "未闭合时 :tostring 被拒")
        t:eq(false, pcall(function() return bson.decode(b) end), "未闭合时 bson.decode 被拒")
        t:eq(false, pcall(function() return bson.iter.new(b) end), "未闭合时 iter.new 被拒")

        -- 嵌套只配平一层仍算未闭合
        local n = bson.new()
        n:doc_begin("m")
        n:int32("v", 1)
        n["end"](n)
        t:eq(false, pcall(function() return n:data() end), "顶层未配平时 :data 仍被拒")
        n["end"](n)
        t:eq(true, n:complete(), "顶层配平后 complete")
        t:eq(1, bson.decode(n).m.v, "配平后 decode 正常")

        -- 配平后原对象照常可用
        b["end"](b)
        local ptr, sz = b:data()
        t:check(ptr ~= nil and sz > 0, "配平后 :data 正常")
        t:eq(1, bson.decode(b).a, "配平后 decode 正常")
    end

    -- 27. 已释放对象不得再交出数据：free 后 doc.data 为 NULL，:data 原先照样返回一个
    --     NULL lightuserdata（在 Lua 里是真值，`if not p` 拦不住），喂给 decode 会让内部
    --     bson_init 走分配分支泄漏 256 字节并解析未初始化堆
    do
        local b = bson.encode({ a = 1 })
        b:free()
        local ok, err = pcall(function() return b:data() end)
        t:eq(false, ok, "free 后 :data 被拒")
        t:check(type(err) == "string" and nil ~= err:find("freed"), "错误信息点明已释放")
        t:eq(false, pcall(function() return b:tostring() end),   "free 后 :tostring 被拒")
        t:eq(false, pcall(function() return bson.decode(b) end), "free 后 bson.decode 被拒")
        t:eq(false, pcall(function() return bson.iter.new(b) end), "free 后 iter.new 被拒")
        t:eq(true, pcall(function() b:free() end), "重复 free 安全")
    end

    -- 28. (指针, 长度) 形式的长度必须在 [0, INT32_MAX]：负数转成 size_t 是 SIZE_MAX，
    --     而 bson_iter_init 唯一的边界就是拿文档头声明的长度跟 doc.size 比，doc.size 成了
    --     SIZE_MAX 那道判定永不触发，文档头写多长就往堆里读多长；cat 的
    --     "内嵌长度 > buffer 长度" 校验同样被 (uint32_t)SIZE_MAX 架空
    do
        local src = bson.encode({ a = 1 })
        local ptr, sz = src:data()
        local big = 2147483648  -- INT32_MAX + 1

        t:eq(false, pcall(function() return bson.new(ptr, -1) end),  "bson.new 负长度被拒")
        t:eq(false, pcall(function() return bson.new(ptr, big) end), "bson.new 超 INT32_MAX 被拒")
        t:eq(true,  pcall(function() return bson.new(ptr, sz) end),  "bson.new 真实长度正常")

        t:eq(false, pcall(function() return bson.tostring2(ptr, -1) end),  "tostring2 负长度被拒")
        t:eq(false, pcall(function() return bson.tostring2(ptr, big) end), "tostring2 超 INT32_MAX 被拒")
        t:check(nil ~= bson.tostring2(ptr, sz), "tostring2 真实长度正常")
        t:check(nil ~= bson.tostring2(srey.ud_str(ptr, sz)), "tostring2 string 形式正常")

        t:eq(false, pcall(function() return bson.decode(ptr, -1) end),  "decode 负长度被拒")
        -- 取值折到 lpub_check_buf 之后，报错文案仍须点明是长度越界而不是别的参数问题
        local _, derr = pcall(function() return bson.decode(ptr, -1) end)
        t:check(type(derr) == "string" and nil ~= derr:find("out of range"), "decode 负长度报错点明越界")
        t:eq(false, pcall(function() return bson.decode(ptr, big) end), "decode 超 INT32_MAX 被拒")

        local b = bson.new()
        t:eq(false, pcall(function() b:append_doc("d", ptr, -1) end), "append_doc 负长度被拒")
        t:eq(false, pcall(function() b:append_arr("r", ptr, -1) end), "append_arr 负长度被拒")
        t:eq(false, pcall(function() b:cat(ptr, -1) end),             "cat 负长度被拒")
        -- 被拒的调用不得写进 builder：长度校验发生在任何 append 之前
        b["end"](b)
        t:eq(nil, bson.decode(b).d, "被拒的 append_doc 未落盘")
        t:eq(nil, bson.decode(b).r, "被拒的 append_arr 未落盘")
        t:eq(nil, bson.decode(b).a, "被拒的 cat 未落盘")

        t:eq(true, pcall(function() bson.new():cat(ptr, sz) end), "cat 真实长度正常")

        -- cat 的拷贝长度取自文档自身的 4 字节头,故长度合法还不够,还得确认那个自声明值没超出缓冲
        t:eq(false, pcall(function() bson.new():cat(ptr, 5) end),   "cat 文档头声明长度超出缓冲被拒")
        t:eq(false, pcall(function() bson.new():cat("abcd") end),   "cat 不足 5 字节被拒")
        -- 零长在 C 层是有意的空操作，但对绑定调用方是错：不拒的话会拼出个格式合法却
        -- 一个字段都没有的文档，直到服务端才发现
        t:eq(false, pcall(function() bson.new():cat("") end),       "cat 零长被拒")
        t:eq(false, pcall(function() bson.new():cat(ptr, 0) end),   "cat 零长(lightuserdata)被拒")
        local _, cerr = pcall(function() bson.new():cat(ptr, 5) end)
        t:check(type(cerr) == "string" and nil ~= cerr:find("document rejected", 1, true), "cat 越界报错点明整篇被拒")

        -- 六处取值都折到 lpub_check_buf 上了，非 string/lightuserdata 仍须被拒
        t:eq(false, pcall(function() return bson.decode(42) end),           "decode 类型错被拒")
        t:eq(false, pcall(function() return bson.tostring2(42) end),        "tostring2 类型错被拒")
        t:eq(false, pcall(function() bson.new():cat(42) end),               "cat 类型错被拒")
        t:eq(false, pcall(function() bson.new():append_doc("d", 42) end),   "append_doc 类型错被拒")
        t:eq(false, pcall(function() bson.new():append_arr("r", 42) end),   "append_arr 类型错被拒")
    end

    -- 29. 建 iter 后往源对象写入撑破容量：REALLOC 搬走缓冲，iter 的 key/val 裸指针集体悬垂。
    --     iter.new 已把 doc.offset 重置为 0，所以写入从头开始，得写够超出容量才会触发扩容
    do
        local b = bson.encode({ a = 1 })
        local before = b:data()
        local it = bson.iter.new(b)
        t:eq(true, it:next(), "搬移前 iter 正常")
        b:utf8("z", string.rep("x", 1 << 20))
        local after = b:data()
        if after ~= before then
            local ok, err = pcall(function() return it:next() end)
            t:eq(false, ok, "缓冲被搬走后 iter 拒绝访问")
            t:check(type(err) == "string" and nil ~= err:find("realloc"), "错误信息点明缓冲被搬移")
            t:eq(false, pcall(function() return it:key() end), "取 key 同样被拒")
        else
            -- REALLOC 原地扩容返回同一地址：内存没被释放，不构成 UAF，本轮无从验证
            t:check(true, "本次扩容未搬移地址，跳过")
        end
    end

    -- 29. :free() 之后的写入方法必须报错。binary_free 只清 data/size/offset 却留着 inc，
    --     所以下一次写入照样能扩容出一块新缓冲、一声不吭地写进去；而这块新缓冲从没经过
    --     bson.new() 那次 append_start，缺开头 4 字节长度前缀与结尾 EOD，:data() 的两道
    --     守卫(非空 + depth==0)又恰好都能过 —— 交出去的是一份结构非法、原有字段也没了的文档，
    --     要等对端解析失败才暴露
    do
        local b = bson.new()
        b:utf8("k", "v")
        b["end"](b)
        b:free()

        t:eq(false, pcall(function() b:utf8("k2", "v2") end), "free 后 :utf8 被拒")
        t:eq(false, pcall(function() b:int32("k2", 1) end),   "free 后 :int32 被拒")
        t:eq(false, pcall(function() b:doc_begin("d") end),   "free 后 :doc_begin 被拒")
        t:eq(false, pcall(function() b:arr_begin("a") end),   "free 后 :arr_begin 被拒")
        t:eq(false, pcall(function() b["end"](b) end),        "free 后 :end 被拒")
        t:eq(false, pcall(function() b:cat("\5\0\0\0\0") end), "free 后 :cat 被拒")
        local ok, err = pcall(function() b:utf8("k2", "v2") end)
        t:eq(false, ok, "free 后写入返回失败")
        t:check(type(err) == "string" and nil ~= err:find("freed"), "错误信息点明已释放")

        -- 写入既然全被挡住，:data() 也就不会交出那份半成品
        t:eq(false, pcall(function() return b:data() end), "free 后 :data 仍被拒")
        b:free()  -- 重复 free 幂等
        t:check(true, "bson 重复 free 幂等")
    end

    -- ── binary 子类型的截断回归 ───────────────────────────────────────
    -- 子类型直接写进一个字节，260 静默截成 4 就是 BSON_SUBTYPE_UUID，业务照 UUID 解释任意二进制
    do
        local b = bson.new()
        t:eq(false, pcall(b.binary, b, "k", 260, "ab"), "b:binary: subtype 260 被拒")
        t:eq(false, pcall(b.binary, b, "k", -1, "ab"), "b:binary: subtype -1 被拒")
        t:eq(true, pcall(b.binary, b, "k", 4, "ab"), "b:binary: 合法子类型照常接受")
        t:eq(false, pcall(bson.mkbinary, 260, "ab"), "bson.mkbinary: subtype 260 被拒")
        t:eq(true, pcall(bson.mkbinary, 128, "ab"), "bson.mkbinary: 自定义区间 0x80 放行")
    end
end)
end)
