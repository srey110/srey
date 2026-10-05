-- protocol 绑定层单元测试（除 redis.unpack 差分外不走网络）：
-- websock pack_*, smtp pack_*, mail pack, redis.pack/value/next/node/unpack, harbor.pack, http.code_status

local srey    = require("lib.srey")
local runner  = require("test.runner")
local utils   = require("srey.utils")
local websock = require("srey.websock")
local http    = require("srey.http")
local redis   = require("lib.redis")
local sredis  = require("srey.redis")-- node 只在 C 绑定层，lib.redis 不导出
local harbor  = require("srey.harbor")
local smtp    = require("srey.smtp")
local mail    = require("srey.smtp.mail")
local yyjson  = require("yyjson")-- yyjson.null 是一个 NULL lightuserdata，用来测空指针拒收
local base64  = require("srey.base64")

local rand  = math.random
local rnode = sredis.node-- 参照实现逐节点取值用
-- redis.unpack 差分：本 task 监听 PACK_TYPE.REDIS 再用 PACK_TYPE.NONE 自连，灌进去的线上字节
-- 经协议层解成链表后，在 RECV 回调里分别交 C 版与参照实现转换
local REDIS_PORT = 15074
local REDIS_MAXD = 17-- 协议层最多同时打开 REDIS_MAX_DEPTH-1 层聚合（见 lib/protocol/redis.h），attr 也算一层
local RESP_DOUBLES = { "nan", "-nan", "inf", "-inf", "1.5", "-0", "0", "2", "1e3" }
-- map 键位偏向这些：nil / NaN 键整对丢弃，整数键会与 attr 追加的 #t + 1 交叠
local RESP_KEYS = { ",nan\r\n", "_\r\n", "$-1\r\n", "*-1\r\n", "%-1\r\n", ":1\r\n", ":2\r\n", ",2\r\n", "#f\r\n" }
local RESP3_AGGS = { "*", "~", ">", "%" }

-- ── redis.unpack 的参照实现：C 版下沉前 lib/redis.lua 里的 Lua 版，原样搬来 ──────

-- 能不能当表键。nil 与 NaN 都不行（t[NaN] 抛错），而 RESP3 的 ,nan 解出来就是 NaN
local function _key_ok(key)
    return nil ~= key and key == key
end
---是否为 map 或 attr（键值对聚合）
---@param kind string? rnode 回带的聚合类型名
---@return boolean ok
local function _is_map(kind)
    return "map" == kind or "attr" == kind
end

---是否为 attr（属性前置聚合，RESP3 特有）
---@param kind string? rnode 回带的聚合类型名
---@return boolean ok
local function _is_attr(kind)
    return "attr" == kind
end

---是否为任意聚合类型（array / set / push / map / attr）
---@param kind string? rnode 回带的聚合类型名
---@return boolean ok
local function _is_agg(kind)
    return "array" == kind or "set" == kind or "push" == kind or
           "map" == kind or "attr" == kind
end

---@class RedisParseMark
---@field status 0|1           0=期望 key，1=期望 val（仅 map/attr 使用）
---@field nelem  integer       剩余待处理元素个数（map/attr 已 ×2）
---@field agg    RedisAggPayload 所属聚合节点（只装载荷）
---@field ismap  boolean       压栈时按 kind 算好的"是否键值对聚合"，替代读表判定
---@field isattr boolean       压栈时按 kind 算好的"是否 attr"
---@field key    any?          map/attr 解析到 key 时暂存，读到 val 时配对写进 agg

---更新栈顶计数器；计数归零时弹出并归还对象池；attr 完成后立即 break，
---因为 attr 之后跟随被修饰的真实数据，需由上层继续处理，不能连续弹出
---@param mark RedisParseMark[] 解析栈
local function _update_mark(mark)
    local mk
    while true do
        mk = mark[#mark]
        if not mk then
            break
        end
        mk.nelem = mk.nelem - 1
        if mk.nelem > 0 then
            break
        end
        mark[#mark] = nil          -- 弹出栈顶
        if mk.isattr then
            break
        end
    end
end

---聚合节点按 nelem 三态取值:>0 取 val,==0 取空表,<0(RESP3 的 nil 聚合)取 neg。
---neg 由调用点显式给:父为 map 暂存 key 时取 nil(让 _key_ok 拦掉整对),其余三处取 false。
---@param val any 聚合值
---@param nelem integer 元素计数
---@param neg any nelem<0 时的取值
---@return any
local function _aggval(val, nelem, neg)
    if nelem > 0 then
        return val
    end
    if 0 == nelem then
        return {}
    end
    return neg
end

---将聚合节点压入解析栈；map/attr 的元素个数需乘以 2（每元素占 key+val 两节点）
---@param mark RedisParseMark[] 解析栈
---@param val RedisAggPayload 聚合容器表
---@param kind string 聚合类型名
---@param nelem integer 元素计数
local function _add_mark(mark, val, kind, nelem)
    local ismap = _is_map(kind)
    mark[#mark + 1] = {
        status = 0,    -- 0=期望 key，1=期望 val（仅 map/attr 使用）
        nelem  = ismap and nelem * 2 or nelem,
        agg    = val,
        ismap  = ismap,
        isattr = _is_attr(kind),
    }
end

---处理单一节点：标量直接返回；聚合 nelem=0 返回 {}，nelem=-1 返回 nil
---@param val any rnode 回带的节点值
---@param kind string? 聚合类型名
---@param nelem integer? 元素计数
---@return any value 解包后的值
local function _single_node(val, kind, nelem)
    if _is_agg(kind) then
        if -1 == nelem then
            return nil
        elseif 0 == nelem then
            return {}
        else
            WARN("resp message error.")
            return nil
        end
    end
    return val
end

---处理多节点响应的第一个节点（必须为聚合类型）；attr 类型用 {val} 包装以区分属性与数据节点
---@param mark RedisParseMark[] 解析栈
---@param val any rnode 回带的首节点值
---@param kind string? 聚合类型名
---@param nelem integer? 元素计数
---@return RedisAggPayload|nil rtn 容器表；首节点非聚合或为 nil 聚合返回 nil
local function _first_nodes(mark, val, kind, nelem)
    if not _is_agg(kind) then
        WARN("resp message error.")
        return nil
    end
    if nelem > 0  then
        _add_mark(mark, val, kind, nelem)
        if _is_attr(kind) then
            return {val}
        else
            return val
        end
    elseif 0 == nelem then
        if _is_attr(kind) then
            return {{}}
        end
        return {}
    else
        if _is_attr(kind) then
            return {}
        end
        return nil
    end
end

---将 C 层 RESP3 响应链表解包为 Lua 值；单节点直接返回，多节点用显式栈组装嵌套聚合（array/set/push/map/attr）
---@param pk lightuserdata redis_pack_ctx 首节点指针
---@return any value 解包后的 Lua 值
local function _ref_unpack(pk)
    local val, kind, nelem, nxt = rnode(pk)
    -- 单一节点：无嵌套，直接返回
    if not nxt then
        return _single_node(val, kind, nelem)
    end
    -- 多节点：第一个节点必须是 aggregate data
    local mark = {}
    local rtn = _first_nodes(mark, val, kind, nelem)
    if not rtn then
        return nil
    end
    local parent, agg, isattr
    pk = nxt
    while pk do
        parent = mark[#mark]
        val, kind, nelem, pk = rnode(pk)
        -- kind 非 nil 即聚合（C 侧只给聚合节点回带类型名），等价于 _is_agg(kind)。
        -- 追加一律 t[#t + 1]：与 table.insert 落点相同，少一次 C 调用
        if not parent then
            -- 无父节点（顶层多值响应，如 pipeline）
            if kind then
                rtn[#rtn + 1] = _aggval(val, nelem, false)
                if nelem > 0 then
                    _add_mark(mark, val, kind, nelem)
                end
            else
                rtn[#rtn + 1] = val ~= nil and val or false
            end
        elseif kind then
            -- 有父节点、当前为聚合节点
            isattr = "attr" == kind
            if parent.ismap and not isattr then
                -- 父为 map/attr，当前为非 attr 聚合节点
                if 0 == parent.status then
                    -- 作为 key 暂存
                    parent.status = 1
                    parent.key = _aggval(val, nelem, nil)
                else
                    -- 作为 val 写入父 map
                    parent.status = 0
                    if _key_ok(parent.key) then
                        parent.agg[parent.key] = _aggval(val, nelem, false)
                    end
                end
            else
                -- 父为 array/set/push 或当前节点为 attr：顺序追加
                agg = parent.agg
                agg[#agg + 1] = _aggval(val, nelem, false)
            end
            if nelem > 0 then
                _add_mark(mark, val, kind, nelem)
            elseif not isattr then
                _update_mark(mark)
            end
        else
            -- 有父节点、当前为标量节点
            if parent.ismap then
                -- 父为 map/attr：交替填充 key/val
                if 0 == parent.status then
                    parent.status = 1
                    parent.key = val
                else
                    parent.status = 0
                    if _key_ok(parent.key) then
                        parent.agg[parent.key] = val ~= nil and val or false
                    end
                end
            else
                -- 父为 array/set/push：顺序追加
                agg = parent.agg
                agg[#agg + 1] = val ~= nil and val or false
            end
            -- 快路径：栈顶计数减一后仍 > 0 就不用进 _update_mark（parent 即栈顶）
            if parent.nelem > 1 then
                parent.nelem = parent.nelem - 1
            else
                _update_mark(mark)
            end
        end
    end
    return rtn
end

-- ── 差分比较与随机 RESP 生成 ───────────────────────────────────────────

-- 解包结果规范成可比较的串：表按 键=值 排序拼接（键也可能是表），数字带 integer/float 标记，NaN 统一成 nan
local function _canon(v)
    local tv = type(v)
    if "table" == tv then
        local parts = {}
        for k, x in pairs(v) do
            parts[#parts + 1] = _canon(k) .. "=" .. _canon(x)
        end
        table.sort(parts)
        return "{" .. table.concat(parts, ",") .. "}"
    end
    if "number" == tv then
        if v ~= v then
            return "nan"
        end
        return math.type(v) .. ":" .. string.format("%.17g", v)
    end
    if "string" == tv then
        return string.format("%q", v)
    end
    return tostring(v)
end

-- 短串取自 a~c 的小字母表，让 map 键经常撞车
local function _gen_word()
    local s = ""
    for _ = 1, rand(0, 4) do
        s = s .. string.char(rand(97, 99))
    end
    return s
end

-- bulk 类（$ ! =）：偶尔带 NUL 与 CRLF；= 要 3 字节编码名加冒号打头
local function _gen_bulk(ty)
    local s = _gen_word()
    if 1 == rand(1, 6) then
        s = s .. "\0\r\n"
    end
    if "=" == ty then
        s = "txt:" .. s
    end
    return ty .. #s .. "\r\n" .. s .. "\r\n"
end

-- 随机标量；resp2 时只出 RESP2 的 + - : $ 四类（含 $-1）
local function _gen_scalar(resp2)
    local k = rand(1, resp2 and 5 or 12)
    if 1 == k then
        return "+" .. _gen_word() .. "\r\n"
    elseif 2 == k then
        return "-ERR " .. _gen_word() .. "\r\n"
    elseif 3 == k then
        return ":" .. rand(-2, 3) .. "\r\n"
    elseif 4 == k then
        return _gen_bulk("$")
    elseif 5 == k then
        return "$-1\r\n"
    elseif 6 == k then
        return "_\r\n"
    elseif 7 == k then
        return 1 == rand(1, 2) and "#t\r\n" or "#f\r\n"
    elseif 8 == k then
        return "," .. RESP_DOUBLES[rand(1, #RESP_DOUBLES)] .. "\r\n"
    elseif 9 == k then
        return "(12345678901234567890123\r\n"
    elseif 10 == k then
        return _gen_bulk("!")
    elseif 11 == k then
        return _gen_bulk("=")
    end
    return ":" .. math.maxinteger .. "\r\n"
end

-- st：budget 剩余节点预算（耗尽后只出标量，保证收敛）、pagg 出聚合的概率、maxn 聚合元素数上限。
-- depth 是外面已打开的聚合层数，聚合（含 attr）声明了元素就多占一层
local _gen_elem
local function _gen_agg(ty, depth, st, resp2)
    st.budget = st.budget - 1
    local r = rand(1, 8)
    if 1 == r then
        return ty .. "-1\r\n"
    elseif 2 == r then
        return ty .. "0\r\n"
    end
    local n = rand(1, st.maxn)
    local ismap = "%" == ty or "|" == ty
    local parts = { ty .. n .. "\r\n" }
    for i = 1, ismap and n * 2 or n do
        if ismap and 1 == i % 2 and 1 == rand(1, 3) then
            parts[#parts + 1] = RESP_KEYS[rand(1, #RESP_KEYS)]
        else
            parts[#parts + 1] = _gen_elem(depth + 1, st, resp2)
        end
    end
    return table.concat(parts)
end
_gen_elem = function(depth, st, resp2)
    local pre = ""
    -- RESP3 的 attr 可挂在任何元素前，不计入父层元素数
    if not resp2 and st.budget > 0 and depth < REDIS_MAXD and 1 == rand(1, 6) then
        pre = _gen_agg("|", depth, st, resp2)
    end
    if st.budget <= 0 or depth >= REDIS_MAXD or rand() >= st.pagg then
        st.budget = st.budget - 1
        return pre .. _gen_scalar(resp2)
    end
    return pre .. _gen_agg(resp2 and "*" or RESP3_AGGS[rand(1, #RESP3_AGGS)], depth, st, resp2)
end

-- 一条完整回复：RESP3 时顶层先挂 0~2 个 attr（解出来是顶层多值），再跟一个元素
local function _gen_reply(st, resp2)
    local parts = {}
    if not resp2 then
        for _ = 1, math.max(0, rand(-2, 2)) do
            parts[#parts + 1] = _gen_agg("|", 0, st, resp2)
        end
    end
    parts[#parts + 1] = _gen_elem(0, st, resp2)
    return table.concat(parts)
end

srey.startup(function()
runner.run(function(t)
    -- ── http.code_status ───────────────────────────────────────────────
    t:eq("OK",                    http.code_status(200), "http 200")
    t:eq("Not Found",             http.code_status(404), "http 404")
    t:eq("Internal Server Error", http.code_status(500), "http 500")

    -- ── 封包访问器拒收空指针 ───────────────────────────────────────────
    do
        -- 这一族拿到 pack 就直接解引用，空指针由 LUACHECK_LUDATA 在参数处拦下
        t:eq(false, pcall(websock.unpack, yyjson.null), "websock.unpack NULL 指针被拒")
        t:eq(false, pcall(http.chunked, yyjson.null), "http.chunked NULL 指针被拒")
        t:eq(false, pcall(http.status, yyjson.null), "http.status NULL 指针被拒")
    end

    -- ── redis.pack ─────────────────────────────────────────────────────
    -- RESP 协议格式：*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n
    do
        local req = redis.pack("SET", "key", "hello")
        t:eq("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n", req, "redis.pack SET")
        local req2 = redis.pack("GET", "key")
        t:eq("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n", req2, "redis.pack GET")
        local req3 = redis.pack("DEL", "k1", "k2", "k3")
        t:check(req3:sub(1, 4) == "*4\r\n", "redis.pack DEL header")
        -- 整数值的浮点要按整数上线：3600000/1000 在 Lua 里是 float，写成 "3600.0"
        -- 会被 Redis 判 not an integer；大数还会退化成 "1e+17"
        t:eq("*4\r\n$5\r\nSETEX\r\n$1\r\nk\r\n$4\r\n3600\r\n$1\r\nv\r\n",
             redis.pack("SETEX", "k", 3600000 / 1000, "v"), "整数值浮点按整数编码")
        t:check(nil ~= redis.pack("ZADD", "k", 1e17, "m"):find("100000000000000000", 1, true),
                "大整数值浮点不退化成科学计数法")
        -- 真正的小数原样保留
        t:check(nil ~= redis.pack("SET", "k", 3.25):find("3.25", 1, true), "小数原样编码")
    end
    do
        -- nil 编成空 bulk(并告警)，末尾的 nil 也算一个参数：个数按栈顶算，不按 # 算
        t:eq("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n", redis.pack("SET", "k", nil), "末尾 nil 编成空 bulk")
        t:eq("*3\r\n$1\r\nA\r\n$0\r\n\r\n$1\r\nb\r\n", redis.pack("A", nil, "b"), "中间 nil 编成空 bulk")
        t:eq("*1\r\n$0\r\n\r\n", redis.pack(nil), "只有一个 nil")
        t:eq("*0\r\n", redis.pack(), "零参数")
        -- 字符串与数字之外的参数按 tostring 转：boolean、带 __tostring 的表
        local ts = setmetatable({}, { __tostring = function() return "TS-OBJ" end })
        t:eq("*4\r\n$1\r\nA\r\n$4\r\ntrue\r\n$5\r\nfalse\r\n$6\r\nTS-OBJ\r\n", redis.pack("A", true, false, ts),
             "boolean 与 __tostring 按 tostring 编码")
        t:eq(false, pcall(redis.pack, "A", setmetatable({}, { __tostring = function() return {} end })),
             "__tostring 返回非字符串抛错")
        -- 整数值浮点各种取值：2^53、-0.0 都按整数写，2^63 超出整数范围按 tostring
        local f63 = tostring(2 ^ 63)
        t:eq("*5\r\n$1\r\nA\r\n$1\r\n3\r\n$16\r\n9007199254740992\r\n$1\r\n0\r\n$" .. #f63 .. "\r\n" .. f63 .. "\r\n",
             redis.pack("A", 3.0, 2 ^ 53, -0.0, 2 ^ 63), "整数值浮点按整数写，超范围按 tostring")
        t:eq("*3\r\n$1\r\nA\r\n$20\r\n-9223372036854775808\r\n$2\r\n-1\r\n", redis.pack("A", math.mininteger, -1),
             "整数原样")
        -- 转换要在缓冲写入之前做完：长参数把缓冲撑到栈上之后再遇到要转换的参数，字节不能错位
        local big = string.rep("r", 70000)
        t:eq("*4\r\n$1\r\nA\r\n$70000\r\n" .. big .. "\r\n$4\r\ntrue\r\n$6\r\nTS-OBJ\r\n", redis.pack("A", big, true, ts),
             "长参数之后的 boolean / __tostring 不错位")
        -- node(nil) 恒返回 4 个 nil：解析循环按 4 个值多重赋值
        t:eq(4, select("#", sredis.node(nil)), "redis.node(nil) 返回 4 个值")
        t:eq(4, select("#", sredis.node()), "redis.node() 返回 4 个值")
        local v, kind, nelem, nxt = sredis.node(nil)
        t:check(nil == v and nil == kind and nil == nelem and nil == nxt, "redis.node(nil) 4 个都是 nil")
    end

    -- ── redis.value / redis.next 的 nil 语义 ───────────────────────────
    -- 链表遍历配对：next 走到尾返回的是 Lua nil,而两者的文档都承诺 nil 时返回 nil。
    -- 少了那道 nil 闸,LUACHECK_LUDATA_OPT 会抛 "light userdata expected",
    -- redis.value(redis.next(node)) 这个惯用法会把整个协程记成错误
    do
        t:eq(nil, redis.value(nil), "redis.value(nil) 返回 nil 而不抛")
        t:eq(nil, redis.next(nil), "redis.next(nil) 返回 nil 而不抛")
        t:eq(1, select("#", redis.value(nil)), "nil 路径也只返 1 个值")
        t:eq(1, select("#", redis.next(nil)), "next 的 nil 路径同样返 1 个值")
        -- 别的类型仍算误用,照抛
        t:eq(false, pcall(redis.value, 42), "redis.value 收数字仍被拒")
        t:eq(false, pcall(redis.next, {}), "redis.next 收 table 仍被拒")
        -- unpack 同口径：nil 与 NULL 指针返回 1 个 nil，别的类型照抛
        t:eq(nil, redis.unpack(nil), "redis.unpack(nil) 返回 nil 而不抛")
        t:eq(1, select("#", redis.unpack(nil)), "unpack 的 nil 路径只返 1 个值")
        t:eq(1, select("#", redis.unpack()), "unpack 不带参数同 nil")
        t:eq(nil, redis.unpack(yyjson.null), "redis.unpack(NULL 指针) 返回 nil")
        t:eq(false, pcall(redis.unpack, 42), "redis.unpack 收数字被拒")
    end

    -- ── harbor.pack ────────────────────────────────────────────────────
    do
        -- 组出来的是一条 HTTP POST：call=0 走 /request，call=1 走 /call，
        -- dst / type 进查询串，payload 进 body。两次只判"非空"的话，
        -- call 标志接反、dst 没写进 url 都发现不了
        local data, size = harbor.pack(0x10, 0, 0, "hello", 5)
        t:check(data ~= nil and size > 0, "harbor.pack returns data")
        local w = srey.ud_str(data, size)
        t:check(nil ~= w:find("POST /request?dst=16&type=0 ", 1, true), "call=0 走 /request，dst/type 入查询串")
        t:check(nil ~= w:find("Content-Length: 5", 1, true), "Content-Length 按 payload 长度写")
        t:check(w:sub(-5) == "hello", "payload 落在报文末尾")
        utils.ud_free(data)
        -- 无 payload
        data, size = harbor.pack(0x10, 1, 0)
        t:check(data ~= nil and size > 0, "harbor.pack no payload")
        w = srey.ud_str(data, size)
        t:check(nil ~= w:find("POST /call?dst=16&type=0 ", 1, true), "call=1 走 /call")
        t:check(nil ~= w:find("Content-Length: 0", 1, true), "无 payload 时 Content-Length 为 0")
        utils.ud_free(data)
    end

    -- ── websock pack 系列（验证返回非空，记得 ud_free） ────────────────
    do
        -- handshake：返回 (pack, size, hsctx)，hsctx 也需要释放
        local pack, size, hsctx = websock.pack_handshake("example.com", nil, "chat")
        t:check(pack ~= nil and size > 0, "websock.pack_handshake")
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("GET ", 1, true) ~= nil, "handshake has GET")
        t:check(txt:find("Upgrade: websocket", 1, true) ~= nil, "handshake has Upgrade")
        t:check(txt:find("Host: example.com", 1, true) ~= nil, "handshake has Host")
        t:check(txt:find("Sec%-WebSocket%-Protocol: chat") ~= nil, "handshake has Sec-WebSocket-Protocol")
        utils.ud_free(pack)
        utils.ud_free(hsctx)
    end
    do
        -- secprots：nil/非 lightuserdata 安全返回 nil（真实协商结果由 C 集成测试覆盖）
        t:check(nil == websock.secprots(nil), "websock.secprots(nil) 返回 nil")
    end
    do
        -- 只判长度分不开这几个包：ping / pong / close 三种帧的差别全在首字节的 opcode 上，
        -- 长度一模一样。绑定层把 ping 接到 pong 上，"size >= 2" 一个都发现不了。
        -- 首字节 = FIN(0x80) | opcode，opcode 取自 lib/protocol/websock.h 的 ws_prot
        local function _head(pack, size, n)
            return { srey.ud_str(pack, size):byte(1, n) }
        end
        local pack, size = websock.pack_ping(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_ping")
        t:eq(0x89, pack and _head(pack, size, 1)[1], "ping 首字节 FIN|WS_PING")
        utils.ud_free(pack)
        pack, size = websock.pack_pong(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_pong")
        t:eq(0x8A, pack and _head(pack, size, 1)[1], "pong 首字节 FIN|WS_PONG")
        utils.ud_free(pack)
        pack, size = websock.pack_close(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_close")
        t:eq(0x88, pack and _head(pack, size, 1)[1], "close 首字节 FIN|WS_CLOSE")
        utils.ud_free(pack)

        -- text/binary 服务端帧（mask=0），fin=1：payload "hi" → 2+2 字节。
        -- text 与 binary 同样只差 opcode，长度还各不相同，于是长度对了更容易让人以为测过了
        pack, size = websock.pack_text(0, 1, "hi")
        t:check(pack ~= nil and size == 4, "websock.pack_text small frame")
        local h = pack and _head(pack, size, 2)
        t:eq(0x81, h and h[1], "text 首字节 FIN|WS_TEXT")
        t:eq(0x02, h and h[2], "text 次字节：无掩码 + 长度 2")
        utils.ud_free(pack)
        pack, size = websock.pack_binary(0, 1, "\x01\x02\x03")
        t:check(pack ~= nil and size == 5, "websock.pack_binary small frame")
        h = pack and _head(pack, size, 2)
        t:eq(0x82, h and h[1], "binary 首字节 FIN|WS_BINARY")
        t:eq(0x03, h and h[2], "binary 次字节：无掩码 + 长度 3")
        utils.ud_free(pack)
        -- 客户端帧带 4 字节 mask，长度加 4；掩码位在次字节的最高位
        pack, size = websock.pack_text(1, 1, "hi")
        t:check(pack ~= nil and size == 8, "websock.pack_text client mask")
        h = pack and _head(pack, size, 2)
        t:eq(0x81, h and h[1], "客户端 text 首字节不变")
        t:eq(0x82, h and h[2], "客户端帧次字节置掩码位(0x80) + 长度 2")
        utils.ud_free(pack)
        -- continua fin=1 + 空 payload（终止帧）：opcode 必须是 WS_CONTINUE(0)，
        -- 写成 WS_TEXT 的话对端会把它当一条新消息的开头
        pack, size = websock.pack_continua(0, 1, "")
        t:check(pack ~= nil and size >= 2, "websock.pack_continua end")
        t:eq(0x80, pack and _head(pack, size, 1)[1], "continua 首字节 FIN|WS_CONTINUE")
        utils.ud_free(pack)
    end

    -- ── smtp pack 系列 ─────────────────────────────────────────────────
    do
        local s = smtp.new("127.0.0.1", 25, nil, "user", "psw")
        t:check(s ~= nil, "smtp.new 合法参数")
        -- ip / user / psw 超长返回 nil 而不是静默截断：截断后的密码拿去认证只换回
        -- 服务端一句 535，调用方看不出是自己传长了（同 mysql.new / pgsql.new / mongo.new）
        local toolong = string.rep("x", 96)
        t:eq(nil, smtp.new("127.0.0.1", 25, nil, "user", toolong), "smtp.new 密码超长返 nil")
        t:eq(nil, smtp.new("127.0.0.1", 25, nil, toolong, "psw"),  "smtp.new 用户名超长返 nil")
        t:eq(nil, smtp.new(toolong, 25, nil, "user", "psw"),       "smtp.new ip 超长返 nil")
        -- 贴边各一条：只测 63 合法 + 96 被拒的话，上限放宽到 95 也抓不到
        t:check(smtp.new("127.0.0.1", 25, nil, "user", string.rep("y", 63)) ~= nil,
                "smtp.new 正好 63 字节仍合法")
        t:eq(nil, smtp.new("127.0.0.1", 25, nil, "user", string.rep("y", 64)),
             "smtp.new 64 字节即超限(缓冲 64 含结尾 NUL)")
        -- 正常地址
        local pack, size = s:pack_from("alice@example.com")
        t:check(pack ~= nil and size > 0, "smtp pack_from")
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("MAIL FROM:", 1, true) ~= nil, "smtp pack_from has prefix")
        t:check(txt:find("<alice@example.com>", 1, true) ~= nil, "smtp pack_from has addr")
        t:check(txt:sub(-2) == "\r\n", "smtp pack_from ends CRLF")
        utils.ud_free(pack)

        pack, size = s:pack_rcpt("bob@example.com")
        t:check(pack ~= nil and size > 0, "smtp pack_rcpt")
        txt = srey.ud_str(pack, size)
        t:check(txt:find("RCPT TO:", 1, true) ~= nil, "smtp pack_rcpt has prefix")
        utils.ud_free(pack)

        -- CRLF 注入防御
        pack, size = s:pack_from("evil@example.com\r\nINJECT")
        t:eq(nil, pack, "smtp pack_from CRLF injection rejected")
        pack, size = s:pack_rcpt("evil@example.com\nINJECT")
        t:eq(nil, pack, "smtp pack_rcpt LF injection rejected")

        -- 固定命令
        pack, size = s:pack_reset()
        t:check(pack ~= nil and srey.ud_str(pack, size):find("RSET", 1, true) ~= nil, "smtp pack_reset")
        utils.ud_free(pack)
        pack, size = s:pack_quit()
        t:check(pack ~= nil and srey.ud_str(pack, size):find("QUIT", 1, true) ~= nil, "smtp pack_quit")
        utils.ud_free(pack)
        pack, size = s:pack_data()
        t:check(pack ~= nil and srey.ud_str(pack, size):find("DATA", 1, true) ~= nil, "smtp pack_data")
        utils.ud_free(pack)
        pack, size = s:pack_ping()
        t:check(pack ~= nil and srey.ud_str(pack, size):find("NOOP", 1, true) ~= nil, "smtp pack_ping (NOOP)")
        utils.ud_free(pack)
    end

    -- ── mail pack（MIME 输出） ─────────────────────────────────────────
    do
        -- reply 的入参形态单独拿个对象验，免得改动下面 pack 断言所依赖的状态。
        -- 标注是 integer?、文档写"nil 视为 0"，那不带参数（LUA_TNONE）也得照收：
        -- 只判 LUA_TNIL 的话会落到 luaL_checkinteger 上报 "number expected, got no value"
        local rm = mail.new()
        t:eq(true, pcall(rm.reply, rm), "mail:reply() 不带参数等同 0")
        t:eq(true, pcall(rm.reply, rm, nil), "mail:reply(nil) 等同 0")
        t:eq(true, pcall(rm.reply, rm, 1), "mail:reply(1) 请求回执")
        -- 契约是"非 0 即真"而不是只收 0/1，别顺手收紧成 lpub_opt_flag
        t:eq(true, pcall(rm.reply, rm, 2), "mail:reply 非 0 值一律当请求回执")

        local m = mail.new()
        m:from("Srey", "srey@example.com")
        -- 收件人类型裸 cast 会让越界值原样存进去:_mail_pack_addr 只认 TO/CC,
        -- 类型是 9 的地址照发 RCPT TO 却不出现在 To: / Cc: 里,可见收件人静默变密送
        t:eq(false, pcall(function() m:addrs_add("x@example.com", 9) end), "越界收件人类型被拒")
        t:eq(false, pcall(function() m:addrs_add("x@example.com", 0) end), "类型 0 被拒")
        m:addrs_add("alice@example.com", 1) -- TO
        -- 发件人/收件人只存在 C 侧，取值一律回读 mail_ctx。Lua 层再留一份副本的话，
        -- CRLF 净化与定长截断这两道改写就会让两份状态分叉（MIME 头一个样、RCPT TO 另一个样）
        t:eq("srey@example.com", m:from_get(), "from_get 回读 C 侧发件人")
        t:eq("alice@example.com", m:addrs_get()[1], "addrs_get 回读 C 侧收件人")
        m:addrs_add("bob@example.com", 2) -- CC
        m:addrs_add("eve@example.com", 3) -- BCC
        local got = m:addrs_get()
        t:eq(3, #got, "TO/CC/BCC 都在 addrs_get 里")
        t:eq("eve@example.com", got[3], "按加入顺序交出")
        m:addrs_clear()
        t:eq(0, #m:addrs_get(), "addrs_clear 后 addrs_get 为空表")
        -- 这一条是删掉 Lua 镜像的理由：交出来的必须是净化后的存量值，不是传进去的原串
        m:from("Srey", "a\r\nb@example.com")
        t:eq("ab@example.com", m:from_get(), "from_get 交出剔除 CRLF 后的存量值")
        m:from("Srey", "srey@example.com")
        m:addrs_add("alice@example.com", 1)
        m:subject("unit test")
        m:msg("plain text body")
        m:html("<h1>hi</h1>")
        local pack, size = m:pack()
        t:check(pack ~= nil and size > 0, "mail pack returns content")
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("Subject:", 1, true) ~= nil, "mail has Subject")
        t:check(txt:find("From:", 1, true) ~= nil, "mail has From")
        t:check(txt:find("To:", 1, true) ~= nil, "mail has To")
        -- 正文与 html 段都是 base64：8bit 原样写出时一段没换行的长正文会造出超过
        -- RFC 5321 §4.5.3.1.6 那 1000 octet 上限的 DATA 行，纯文本单段还会因为缺
        -- Content-Type 被按 us-ascii 解释
        t:check(txt:find("plain text body", 1, true) == nil, "正文不再以明文出现")
        t:check(txt:find(base64.encode("plain text body"), 1, true) ~= nil, "正文以 base64 出现")
        t:check(txt:find("multipart/alternative", 1, true) ~= nil, "mail has multipart/alternative")
        t:check(txt:find("text/html", 1, true) ~= nil, "mail has text/html header")
        -- RFC 5322 §3.4 的 name-addr 形式
        t:check(txt:find("From: Srey <srey@example.com>", 1, true) ~= nil, "From 用 display-name <addr>")
        utils.ud_free(pack)
    end

    -- ── mail 释放后再调用 ─────────────────────────────────────────────
    -- REG_MTABLE 令 __gc 经 __index 也是个普通方法，业务一行 m:__gc() 就能提前释放；
    -- 此后任何方法都必须报可捕获的 Lua 错，而不是拿着已释放的 mail_ctx 往下走
    do
        local m = mail.new()
        m:from("Srey", "srey@example.com")
        m:addrs_add("alice@example.com", 1)
        m:__gc()

        -- array_free 只置空 ptr 不复位 size/maxsize，无守卫时 array_push_back 跳过扩容分支
        -- 直接往 NULL 基址 memcpy，是空指针写而非断言
        t:eq(false, pcall(function() m:addrs_add("bob@example.com", 1) end), "释放后 addrs_add 被拒")
        t:eq(false, pcall(function() return m:from_get() end),  "释放后 from_get 被拒")
        t:eq(false, pcall(function() return m:addrs_get() end), "释放后 addrs_get 被拒")
        t:eq(false, pcall(function() m:attach_clear() end), "释放后 attach_clear 被拒")
        t:eq(false, pcall(function() m:subject("x") end),   "释放后 subject 被拒")
        t:eq(false, pcall(function() m:clear() end),        "释放后 clear 被拒")
        local ok, err = pcall(function() return m:pack() end)
        t:eq(false, ok, "释放后 pack 被拒")
        t:check(type(err) == "string" and nil ~= err:find("freed"), "错误信息点明已释放")

        -- 显式释放后真正的 __gc 仍会跑一遍，必须幂等
        t:eq(true, pcall(function() m:__gc() end), "重复 __gc 安全")
    end

    -- ── redis.unpack：C 版与参照实现差分 ─────────────────────────────
    -- 线上字节经协议层解成链表，同一个链表分别交两边转换，规范化后逐条比；
    -- 固定用例另与手写期望比，确认参照实现与期望本身没搬错
    do
        local function _wait(cond)
            for _ = 1, 60 do
                if cond() then
                    return true
                end
                srey.sleep(50)
            end
            return cond()
        end
        local got = {}
        srey.on_recved(function(pktype, _, client, _, data)
            if PACK_TYPE.REDIS == pktype and 0 == client then
                -- 两边都不挂起，回调里转完时载荷还有效
                got[#got + 1] = { c = _canon(redis.unpack(data)), r = _canon(_ref_unpack(data)) }
            end
        end)
        -- 固定边角：{ 线上字节, 期望值, 名字 }
        local deep = 1
        for _ = 1, REDIS_MAXD do
            deep = { deep }
        end
        local mixed = { [1] = { a = "b" }, k = 1 }
        for _ = 1, REDIS_MAXD - 2 do
            mixed = { k = mixed }
        end
        local fixed = {
            { "+OK\r\n", "OK", "单节点标量" },
            { "$-1\r\n", nil, "单节点 null bulk" },
            { "_\r\n", nil, "单节点 RESP3 nil" },
            { ",2\r\n", 2.0, "单节点浮点保持 float" },
            { "*-1\r\n", nil, "单节点 nil 聚合" },
            { "%-1\r\n", nil, "单节点 nil map" },
            { "*0\r\n", {}, "单节点空聚合为空表" },
            { "*3\r\n:1\r\n$-1\r\n#f\r\n", { 1, false, false }, "数组里的 nil 写 false" },
            { "~2\r\n+a\r\n+a\r\n", { "a", "a" }, "set 解成数组且不去重" },
            { ">2\r\n+message\r\n+x\r\n", { "message", "x" }, "push 解成数组" },
            { "%2\r\n,nan\r\n:1\r\n+a\r\n_\r\n", { a = false }, "NaN 键整对丢弃、nil 值写 false" },
            { "%2\r\n_\r\n:1\r\n*-1\r\n:2\r\n", {}, "nil 键与 nil 聚合键整对丢弃" },
            { "%1\r\n,2\r\n+v\r\n", { [2] = "v" }, "整数值浮点键归一成整数键" },
            { "%1\r\n+k\r\n*-1\r\n", { k = false }, "map 值为 nil 聚合写 false" },
            { "%1\r\n+k\r\n*0\r\n", { k = {} }, "map 值为空聚合写空表" },
            { "%1\r\n*1\r\n:1\r\n+v\r\n", { [{ 1 }] = "v" }, "聚合当 map 键" },
            { "*1\r\n,nan\r\n", { 0 / 0 }, "NaN 当值照留" },
            { "*2\r\n|1\r\n+a\r\n+b\r\n:1\r\n:2\r\n", { { a = "b" }, 1, 2 }, "数组里的 attr 顺序追加" },
            { "%1\r\n|1\r\n+a\r\n+b\r\n+k\r\n+v\r\n", { [1] = { a = "b" }, k = "v" }, "map 里的 attr 追加到 #t + 1" },
            { "%1\r\n+k\r\n|1\r\n+a\r\n+b\r\n+v\r\n", { [1] = { a = "b" }, k = "v" }, "attr 夹在键值之间不打断配对" },
            { "%1\r\n|-1\r\n+k\r\n+v\r\n", { [1] = false, k = "v" }, "map 里的 nil attr 追加 false" },
            { "|1\r\n+k\r\n+v\r\n+OK\r\n", { { k = "v" }, "OK" }, "首节点 attr 包一层" },
            { "|0\r\n:5\r\n", { {}, 5 }, "首节点空 attr 占一格空表" },
            { "|-1\r\n:5\r\n", { 5 }, "首节点 nil attr 不占位" },
            { "|1\r\n+a\r\n+b\r\n|-1\r\n*-1\r\n", { { a = "b" }, false, false }, "顶层多值里的 nil 写 false" },
            { "|1\r\n+a\r\n+b\r\n*0\r\n", { { a = "b" }, {} }, "顶层多值里的空聚合" },
            { "|1\r\n+a\r\n+b\r\n|1\r\n+c\r\n+d\r\n*1\r\n:1\r\n", { { a = "b" }, { c = "d" }, { 1 } },
              "顶层两个 attr 再跟数组" },
            { string.rep("*1\r\n", REDIS_MAXD) .. ":1\r\n", deep, "数组嵌满深度上限" },
            { string.rep("%1\r\n+k\r\n", REDIS_MAXD - 1) .. "|1\r\n+a\r\n+b\r\n:1\r\n", mixed,
              "map 嵌到上限前一层再挂 attr 顶满" },
        }
        -- 随机用例：普通 RESP3、只用 RESP2 类型、贴着深度上限的高聚合率三组
        local seed = os.time()
        math.randomseed(seed)
        local wires = {}
        for i = 1, 400 do
            if i <= 250 then
                wires[i] = _gen_reply({ budget = 60, pagg = 0.35, maxn = 4 }, false)
            elseif i <= 320 then
                wires[i] = _gen_reply({ budget = 60, pagg = 0.35, maxn = 4 }, true)
            else
                wires[i] = _gen_reply({ budget = 300, pagg = 0.9, maxn = 2 }, false)
            end
        end
        local cases = {}
        for i, f in ipairs(fixed) do
            cases[i] = f[1]
        end
        for _, w in ipairs(wires) do
            cases[#cases + 1] = w
        end
        local lid = srey.listen(PACK_TYPE.REDIS, SSL_NAME.NONE, "127.0.0.1", REDIS_PORT)
        local csk = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", REDIS_PORT)
        if t:check(ERR_FAILED ~= lid and csk.valid, "redis.unpack 差分用例连接建立") then
            -- 分批灌：一批拼成一次发送（同一连接上的多条回复），收齐再发下一批，免得顶爆本 task 队列
            local batch = 25
            local last, chunk
            for first = 1, #cases, batch do
                last = math.min(first + batch - 1, #cases)
                chunk = table.concat(cases, "", first, last)
                srey.send(csk, chunk, #chunk, 1)
                _wait(function() return #got >= last end)
            end
            t:eq(#cases, #got, "每条线上回复都解出一条消息")
            for i, f in ipairs(fixed) do
                if got[i] then
                    t:eq(_canon(f[2]), got[i].c, "redis.unpack " .. f[3])
                    t:eq(got[i].r, got[i].c, "redis.unpack 与参照一致：" .. f[3])
                end
            end
            local nbad = 0
            local g
            for i, w in ipairs(wires) do
                g = got[#fixed + i]
                if g and g.r ~= g.c then
                    nbad = nbad + 1
                    if nbad <= 3 then
                        t:eq(g.r, g.c, string.format("redis.unpack 随机差分 #%d seed=%d wire=%q", i, seed, w))
                    end
                end
            end
            t:eq(0, nbad, string.format("redis.unpack 随机差分 %d 条全部一致 seed=%d", #wires, seed))
        end
        if csk.valid then
            srey.close(csk)
        end
        if ERR_FAILED ~= lid then
            srey.unlisten(lid)
        end
    end
end)
end)
