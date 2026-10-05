-- yyjson 绑定层单元测试：encode/decode 往返 + 数组/对象判定 + null + 错误路径
-- 行为基线：空表编成 {}、稀疏数组报错、深度上限 LYYJSON_MAX_DEPTH、整数精确写出

local srey   = require("lib.srey")
local runner = require("test.runner")
local yyjson = require("yyjson")

srey.startup(function()
runner.run(function(t)
    -- ── 标量往返 ────────────────────────────────────────────────
    do
        t:eq('"hi"', yyjson.encode("hi"), "encode string")
        t:eq("hi", yyjson.decode('"hi"'), "decode string")
        t:eq("true", yyjson.encode(true), "encode true")
        t:eq(true, yyjson.decode("true"), "decode true")
        t:eq(false, yyjson.decode("false"), "decode false")
        t:eq("42", yyjson.encode(42), "encode integer")
        t:eq(42, yyjson.decode("42"), "decode integer")
        t:eq(-7, yyjson.decode("-7"), "decode negative integer")
        t:eq(1.5, yyjson.decode("1.5"), "decode real")
        t:check(math.type(yyjson.decode("42")) == "integer", "整数解成 integer 子类型")
        t:check(math.type(yyjson.decode("1.5")) == "float", "小数解成 float 子类型")
    end
    -- ── 整数精确：不退化成浮点 ─────────────────────────────────
    do
        local big = 9007199254740993 -- 2^53 + 1，双精度表示不了
        t:eq("9007199254740993", yyjson.encode(big), "大整数精确编码，不退化成 9.007199254741e+15")
        t:eq(big, yyjson.decode("9007199254740993"), "大整数精确解码")
        t:eq(math.maxinteger, yyjson.decode(tostring(math.maxinteger)), "maxinteger 往返")
        t:eq(math.mininteger, yyjson.decode(tostring(math.mininteger)), "mininteger 往返")
        -- 超出 lua_Integer 的无符号数退化成浮点，而不是整数回绕成负值
        local over = yyjson.decode("18446744073709551615")
        t:check(math.type(over) == "float" and over > 0, "超界 uint64 退化成正浮点")
    end
    -- ── 空表 / 数组 / 对象判定 ──────────────────────────────────
    do
        t:eq("{}", yyjson.encode({}), "空表编成 {}")
        t:eq("[1,2,3]", yyjson.encode({ 1, 2, 3 }), "连续整数键编成数组")
        t:eq('{"a":1}', yyjson.encode({ a = 1 }), "字符串键编成对象")
        local arr = yyjson.decode("[10,20,30]")
        t:eq(3, #arr, "数组解出长度")
        t:eq(20, arr[2], "数组下标 1 基")
        local obj = yyjson.decode('{"k":"v","n":5}')
        t:eq("v", obj.k, "对象字符串字段")
        t:eq(5, obj.n, "对象数值字段")
        -- 数字键混字符串键：整体当对象，数字键转成字符串键
        local mixed = yyjson.decode(yyjson.encode({ [1] = "a", x = "b" }))
        t:eq("a", mixed["1"], "数字键转成字符串键")
        t:eq("b", mixed.x, "同表内的字符串键保留")
    end
    -- ── 嵌套往返 ────────────────────────────────────────────────
    do
        local src = { id = 1, name = "srey", tags = { "a", "b" },
                      dev = { os = "linux", tz = -8 }, ok = true }
        local got = yyjson.decode(yyjson.encode(src))
        t:eq(1, got.id, "嵌套：顶层整数")
        t:eq("srey", got.name, "嵌套：顶层字符串")
        t:eq(2, #got.tags, "嵌套：数组长度")
        t:eq("b", got.tags[2], "嵌套：数组元素")
        t:eq("linux", got.dev.os, "嵌套：子对象字符串")
        t:eq(-8, got.dev.tz, "嵌套：子对象负数")
        t:eq(true, got.ok, "嵌套：布尔")
    end
    -- ── null 双向 ───────────────────────────────────────────────
    do
        t:eq("null", yyjson.encode(yyjson.null), "yyjson.null 编成 null")
        t:eq("null", yyjson.encode(nil), "nil 编成 null")
        t:eq(yyjson.null, yyjson.decode("null"), "null 解成 yyjson.null")
        local arr = yyjson.decode("[1,null,3]")
        t:eq(3, #arr, "含 null 的数组长度")
        t:eq(yyjson.null, arr[2], "数组中的 null")
        t:eq(yyjson.null, yyjson.decode('{"a":null}').a, "对象中的 null")
        t:eq("[1,null,3]", yyjson.encode({ [1] = 1, [3] = 3 }), "数组空洞编成 null")
    end
    -- ── 稀疏数组：过度稀疏报错，不静默转对象 ────────────────────
    do
        -- 判定：max > items*2 且 max > 10 即过度稀疏
        t:eq(false, pcall(yyjson.encode, { [1] = 1, [100] = 1 }), "过度稀疏数组报错")
        t:eq(true, pcall(yyjson.encode, { [1] = 1, [2] = 2, [3] = 3 }), "连续数组不报错")
        -- max=10 未超过 safe 阈值 10，仍当数组
        t:eq(true, pcall(yyjson.encode, { [1] = 1, [10] = 1 }), "max 等于 safe 阈值不算稀疏")
    end
    -- ── 深度上限 LYYJSON_MAX_DEPTH ──────────────────────────────
    do
        local function nest(n)
            local root = {}
            local cur = root
            for _ = 1, n do
                cur.x = {}
                cur = cur.x
            end
            return root
        end
        -- 贴边取值：_lyyjson_pack_tbl 在 depth >= LYYJSON_MAX_DEPTH(18) 时拒，
        -- 而 nest(n) 的最内层正好落在 depth n，故 17 放行、18 被拒。
        -- 上限挪到任何一个中间值都会被这两条抓到
        t:eq(true, pcall(yyjson.encode, nest(16)), "上限内正常")
        t:eq(true, pcall(yyjson.encode, nest(17)), "最深一层恰好 depth 17，放行")
        t:eq(false, pcall(yyjson.encode, nest(18)), "depth 到 18 即达上限，被拒")
        t:eq(false, pcall(yyjson.encode, nest(19)), "超过 LYYJSON_MAX_DEPTH 层报错")
        t:eq(false, pcall(yyjson.decode, string.rep("[", 1200) .. string.rep("]", 1200)),
             "解码超深嵌套报错")
        -- 解码同 encode 只数容器层：最内层表带标量叶子，编得出就得解得回；再包一层容器才拒
        local leaf = nest(17)
        local cur = leaf
        while cur.x do
            cur = cur.x
        end
        cur.v = 1
        local okenc, s17 = pcall(yyjson.encode, leaf)
        t:eq(true, okenc, "18 层容器带标量叶子可编码")
        t:eq(true, (pcall(yyjson.decode, s17)), "18 层容器带标量叶子可解码")
        t:eq(false, (pcall(yyjson.decode, '{"y":' .. tostring(s17) .. '}')), "19 层容器解码报错")
    end
    -- ── 错误路径 ────────────────────────────────────────────────
    do
        t:eq(false, pcall(yyjson.encode, print), "function 类型报错")
        t:eq(false, pcall(yyjson.encode, { [print] = 1 }), "function 作键报错")
        t:eq(false, pcall(yyjson.decode, "{"), "非法 JSON 报错")
        t:eq(false, pcall(yyjson.decode, ""), "空串报错")
        t:eq(false, pcall(yyjson.decode, "{bad}"), "语法错报错")
        t:eq(false, pcall(yyjson.decode), "decode 缺参数报错")
        t:eq(false, pcall(yyjson.encode), "encode 缺参数报错")
        -- 循环引用：靠深度上限兜住，不能爆栈
        local cyc = {}
        cyc.self = cyc
        t:eq(false, pcall(yyjson.encode, cyc), "循环引用报错而不是爆栈")
    end
    -- ── decode 收 lightuserdata ─────────────────────────────────
    do
        -- yyjson.null 是 NULL light userdata：走的是 lightuserdata 分支（不是
        -- "string expected" 参数类型错），由 lpub_check_buf 在长度非 0 时拒收
        local ok, err = pcall(yyjson.decode, yyjson.null, 4)
        t:eq(false, ok, "NULL lightuserdata 被拒")
        t:check(type(err) == "string" and err:find("non%-null") ~= nil,
                "报的是空指针参数错")
        -- (NULL, 0) 等价空缓冲，放行到 yyjson 自己报解析错
        t:eq(false, pcall(yyjson.decode, yyjson.null, 0), "NULL lud + 0 长度是空输入")
        -- lightuserdata 必须带 size，缺了走 lpub_check_lens 报错
        t:eq(false, pcall(yyjson.decode, yyjson.null), "lightuserdata 缺 size 报错")
        -- 负 size 被 lpub_check_lens 挡下（转 size_t 会变天文数字）
        t:eq(false, pcall(yyjson.decode, yyjson.null, -1), "负 size 报错")
    end

    -- ── 非法 UTF-8 原样透传 ─────────────────────────────────────
    -- 旧的 lua_cjson 明确声明"不检测非法 UTF-8，原样放行"，迁到 yyjson 时 flags 传 0
    -- 把这条行为丢了：encode 抛错让请求一个字节都发不出去，decode 把第三方回的
    -- 未转码 Latin-1 JSON 当解析失败整个丢掉。Lua 字符串本就是字节串
    do
        local blob = "a\xff\xfeb"
        local ok, js = pcall(yyjson.encode, { k = blob })
        t:eq(true, ok, "encode 含非 UTF-8 字节的字符串不报错")
        if ok then
            local ok2, tb = pcall(yyjson.decode, js)
            t:eq(true, ok2, "decode 自己写出的那份不报错")
            if ok2 then
                t:eq(blob, tb.k, "非法 UTF-8 字节往返后逐字节相同")
            end
        end
        -- 对端直接甩过来的 Latin-1（未转码的 "café"）也要收得下
        local ok3, tb3 = pcall(yyjson.decode, '{"name":"caf\xe9"}')
        t:eq(true, ok3, "decode 未转码的 Latin-1 JSON 不报错")
        if ok3 then
            t:eq("caf\xe9", tb3.name, "Latin-1 字节原样收下")
        end
    end

    -- ── 数字键的键名文本：与 tostring（即 lua_tolstring 的十进制）逐字相同 ──
    -- 整数键改走手写十进制，负数、0 与两端极值最容易写错；1.5 这类浮点键仍走 Lua 格式化
    do
        -- 单键对象：输出唯一，直接整串比对
        t:eq('{"0":1}', yyjson.encode({ [0] = 1 }), "键 0 编成对象键 \"0\"")
        t:eq('{"-1":1}', yyjson.encode({ [-1] = 1 }), "键 -1")
        t:eq('{"-5":1}', yyjson.encode({ [-5] = 1 }), "键 -5")
        t:eq('{"-9223372036854775808":1}', yyjson.encode({ [math.mininteger] = 1 }), "键 mininteger")
        t:eq('{"1.5":1}', yyjson.encode({ [1.5] = 1 }), "浮点键 1.5")
        t:eq('{"-0.5":1}', yyjson.encode({ [-0.5] = 1 }), "浮点键 -0.5")
        -- 混合表：键序由 lua_next 决定，逐个找片段
        local src = { [-5] = "n5", [0] = "z", [1] = "p1", [2] = "p2",
                      [math.maxinteger] = "max", [math.mininteger] = "min", [1.5] = "f" }
        local js = yyjson.encode(src)
        local n = 0
        for k, v in pairs(src) do
            n = n + 1
            t:check(nil ~= js:find(string.format('"%s":"%s"', tostring(k), v), 1, true),
                    "混合表键 " .. tostring(k) .. " 的键名文本与 tostring 一致")
        end
        t:eq(7, n, "混合表源 7 个键")
        -- 极值另写死期望文本，不只靠 tostring 对照
        t:check(nil ~= js:find('"9223372036854775807":"max"', 1, true), "混合表 maxinteger 键名")
        t:check(nil ~= js:find('"-9223372036854775808":"min"', 1, true), "混合表 mininteger 键名")
        t:check(nil ~= js:find('"1.5":"f"', 1, true), "混合表浮点键名")
        local dec = yyjson.decode(js)
        local cnt = 0
        for _ in pairs(dec) do cnt = cnt + 1 end
        t:eq(7, cnt, "混合表 decode 回来键数不变")
        for k, v in pairs(src) do
            t:eq(v, dec[tostring(k)], "混合表 decode 回来键 " .. tostring(k) .. " 在")
        end
    end

    -- ── 纯整数数组与稀疏判定：整数键快路径不改变判定结果 ──────
    do
        local rev = {}
        for i = 5, 1, -1 do rev[i] = i * 10 end
        t:eq("[10,20,30,40,50]", yyjson.encode(rev), "倒序插入的整数键编成数组")
        t:eq('["x","y","z"]', yyjson.encode({ "x", "y", "z" }), "字符串元素数组")
        t:eq("[[1,2],[3]]", yyjson.encode({ { 1, 2 }, { 3 } }), "嵌套数组")
        t:eq("[" .. math.maxinteger .. "," .. math.mininteger .. "]",
             yyjson.encode({ math.maxinteger, math.mininteger }), "数组里的整数极值精确写出")
        -- 0 不是数组下标：整表当对象
        local o = yyjson.decode(yyjson.encode({ [0] = "a", [1] = "b" }))
        t:eq("a", o["0"], "含 0 键的表当对象，0 键在")
        t:eq("b", o["1"], "含 0 键的表当对象，1 键在")
        -- 判定式 max > items*2 且 max > 10：items=7 max=12 不稀疏，空位补 null
        t:eq("[1,2,3,4,5,6,null,null,null,null,null,12]",
             yyjson.encode({ 1, 2, 3, 4, 5, 6, [12] = 12 }), "max=12 items=7 仍编成数组")
        local ok, err = pcall(yyjson.encode, { 1, 2, 3, 4, 5, [13] = 13 })
        t:eq(false, ok, "max=13 items=6 过度稀疏报错")
        t:check(type(err) == "string" and nil ~= err:find("sparse", 1, true), "报错点明稀疏")
        t:eq(false, pcall(yyjson.encode, { [1] = 1, [11] = 1 }), "max=11 刚过 safe 阈值即报错")
        t:eq(false, pcall(yyjson.encode, { [math.maxinteger] = 1 }), "单个 maxinteger 键按过度稀疏报错")
        t:eq(false, pcall(yyjson.encode, { a = { [1] = 1, [50] = 1 } }), "嵌套层过度稀疏同样报错")
    end

    -- ── 字符串值 / 字符串键：按长度写全，NUL 与需转义字符都对 ──
    do
        t:eq('{"key":"val"}', yyjson.encode({ key = "val" }), "字符串键 + 字符串值")
        t:eq('{"same":"same"}', yyjson.encode({ same = "same" }), "键与值是同一个字符串")
        t:eq('""', yyjson.encode(""), "空串")
        t:eq('{"":""}', yyjson.encode({ [""] = "" }), "空串键与空串值")
        t:eq('"\\u0000"', yyjson.encode("\0"), "只有一个 NUL")
        t:eq('"a\\u0000b"', yyjson.encode("a\0b"), "顶层字符串含 NUL 不截断")
        t:eq('{"s":"x\\u0000y"}', yyjson.encode({ s = "x\0y" }), "字符串值含 NUL")
        t:eq('{"k\\u0000z":1}', yyjson.encode({ ["k\0z"] = 1 }), "字符串键含 NUL")
        t:eq('"q\\"b\\\\s\\n"', yyjson.encode('q"b\\s\n'), "引号 / 反斜杠 / 换行转义")
        local rt = yyjson.decode(yyjson.encode({ s = "x\0y", ["k\0z"] = "a\0\0b" }))
        t:eq("x\0y", rt.s, "含 NUL 的值往返")
        t:eq("a\0\0b", rt["k\0z"], "含 NUL 的键与值往返")
        -- 运行期拼出的短串与长串（长串不进字符串池）作值和键
        local long = string.rep("L", 1000)
        t:eq('"' .. long .. '"', yyjson.encode(long), "长串顶层值原样写出")
        local arr = {}
        for i = 1, 200 do arr[i] = "v" .. i .. long:sub(1, i) end
        local back = yyjson.decode(yyjson.encode(arr))
        local same = #back == 200
        for i = 1, 200 do
            if back[i] ~= arr[i] then same = false end
        end
        t:check(same, "200 个运行期字符串值往返逐个相同")
        local obj = {}
        for i = 1, 50 do obj["k" .. i .. long:sub(1, i * 10)] = "v" .. i end
        local bo = yyjson.decode(yyjson.encode(obj))
        local nk = 0
        local kok = true
        for _ in pairs(bo) do nk = nk + 1 end
        for k, v in pairs(obj) do
            if bo[k] ~= v then kok = false end
        end
        t:check(kok and nk == 50, "50 个长字符串键往返逐个相同")
    end
end)
end)
