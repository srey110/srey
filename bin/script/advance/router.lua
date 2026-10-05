-- HTTP 路由/中间件框架，仿 Laravel 风格。
-- 路径参数使用 {param} 语法，可选参数 {param?}，末尾通配 *。
-- 快速上手:
--   local Route = require("advance.router")
--   -- 具名中间件注册（可选）
--   Route:define("auth", function(ctx, next)
--       if ctx:header("x-api-key") ~= "secret" then ctx:text(401, "Unauthorized\n") return end
--       next()
--   end)
--   -- 全局中间件
--   Route:use("auth")
--   Route:use(function(ctx, next) ... next() ... end)
--   -- 路由注册
--   Route:get("/", handler)
--   Route:get("/user/{id}", handler)
--   Route:get("/file/{path?}", handler)          -- 可选参数
--   Route:post("/user", handler, {"auth"})        -- 路由级中间件（名称或函数）
--   -- 命名路由：登记后可用 Route:url 反向生成 URL
--   Route:get("/user/{id}", handler):name("user.show")
--   Route:url("user.show", { id = 42, page = 2 })     -- "/user/42?page=2"
--   -- 流式分组（仿 Laravel prefix/middleware/group 链）
--   Route:prefix("/api")
--       :middleware("auth")
--       :group(function()
--           Route:get("/users", list)
--           Route:post("/users", create)
--           Route:prefix("/admin"):group(function()
--               Route:get("/stats", stats)
--           end)
--       end)
--   -- on_recved 中分发（client 是 on_recved 传下来的 1=客户端/0=服务端 标志，不是地址；
--   -- 要对端 IP 用 utils.remote_addr(sk)）
--   Route:dispatch(sk, data)
--   Route:dispatch(sk, data, client)
--   -- 流式路由（chunked 请求体逐块到，router 不缓存）；用它就得把两个回调都接上
--   Route:post_stream("/upload", function(ctx, slice, data)
--       if 0 == slice then ctx:text(200, data or "")                        -- 非 chunked，一次到齐
--       elseif SLICE_TYPE.START == slice then ctx.buf = {}                  -- 首帧，头部/参数已可读
--       elseif SLICE_TYPE.SLICE == slice then ctx.buf[#ctx.buf + 1] = data  -- 数据块
--       elseif SLICE_TYPE.END == slice then ctx:text(200, table.concat(ctx.buf))
--       else ctx.buf = nil end                                              -- STREAM_ABORT，只清理
--   end)
--   srey.on_recved(function(...) Route:net_recv(...) end)
--   -- 不必接 on_closed：流式路由的 CLOSE 清理已由 Route 自己经 srey.watch_closed 订阅，
--   -- 而 on_closed 是单槽、后注册者静默覆盖前者，接了反而会顶掉业务自己的回调
-- ctx 字段:
--   ctx.sk / ctx.client
--   ctx.method  -- "GET" "POST" ...
--   ctx.version -- "HTTP/1.1"
--   ctx.path    -- "/user/42"
--   ctx.params  -- 路由参数 {id="42"}
--   ctx.query   -- 查询参数 {page="1"}
--   ctx.body    -- 请求体（可能为 nil）；惰性取，见下
--   ctx.headers -- 请求头 table（键为线格式原样大小写）；惰性取，见下
--   ctx:header(name) -- 按名字取单个头，大小写无关；不物化头表
-- ctx 的生命周期只在本次 dispatch 内（与 C 侧 router_req 同口径）：body / headers 首次访问
-- 时才从底层 pack 物化，而 pack 出了 dispatch 就没了。要带进 srey.fork / timer 延迟用，
-- 得在 handler 里先读一次（读过即固化在 ctx 上），或者自己把需要的值拷走
-- ctx 响应方法:
--   ctx:text(code, body)
--   ctx:json(code, tbl)
--   ctx:html(code, body)
--   ctx:respond(code, headers, body)
-- 创建独立实例（多路由器场景）:
--   local r = require("advance.router").new()
-- 中间件执行顺序:
--   dispatch 匹配路由后，将三层中间件拼成一条 chain 数组依次执行：
--     全局中间件（use 注册顺序） → 路由级中间件（mws 数组顺序） → handler
--   每个中间件必须主动调 next() 才会继续往后执行，不调则链路在此截断：
--     function(ctx, next)
--         if not auth(ctx) then ctx:text(401, "Unauthorized\n") return end
--         next()           -- 继续执行后续中间件和 handler
--         -- next() 返回后可做后置处理
--     end
--   链内异常不在中间件内捕获，由 dispatch 的 srey.xpcall 统一兜底（自动 ERROR + traceback），响应 500。
--   prefix/middleware/group 分组的中间件在注册阶段静态合并进路由条目，

local srey        = require("lib.srey")
local http        = require("lib.http")
local _srey_router_new = require("srey.router").new
local url         = require("srey.url")
local SLICE_TYPE = SLICE_TYPE
-- 流式路由的中止通知，与 C 侧 ROUTER_STREAM_ABORT 同值。协议层不会产生这个 slice，
-- 由 router 自造：流没收齐就没了（连接断 / 同连接又来一个流式首帧 / 回调自己抛异常）
local STREAM_ABORT = 0x80
local type     = type
local pcall    = pcall
local tostring = tostring
local assert   = assert
local select   = select
local pairs    = pairs
local getmetatable = getmetatable
local rawget   = rawget
local setmetatable = setmetatable
-- 固定的 Content-Type 预渲染成头部块，原样拼进报文，不再每次过一遍头校验
local _PLAIN_BLOCK = "Content-Type: text/plain; charset=utf-8\r\n"
local _HTML_BLOCK  = "Content-Type: text/html; charset=utf-8\r\n"
local _http_respond = http._respond
local _http_response_lua = http._response_lua
-- 匹配失败（400/404/405）与条目缺失（500）的响应正文，按码预先拼好
local _CODE_BODY = {}
for _, c in ipairs({ 400, 404, 405, 500 }) do
    _CODE_BODY[c] = http.code_status(c) .. "\n"
end
-- 注册过流式路由的 router 集合。弱键：业务丢掉一个 router 后它就可回收，不被下面那个
-- 常驻闭包钉住。srey.fork 只往队列追加、本条消息末尾才起，故遍历期间没有用户代码插键
local stream_routers = setmetatable({}, { __mode = "k" })
local st_watching = false
-- 注册失败的条目也要能链式调 :name()，返回这个只告警不做事的哑条目
local _bad_entry = {}
_bad_entry.name = function(_, n) WARN("router: :name(%s) on rejected entry.", tostring(n)) return _bad_entry end

-- 中间件得能被 chain[i](ctx, nxt) 调起来：函数，或带 __call 的表/userdata。
-- 元表被 __metatable 藏起来时查不出真相，放行交给调用点——把真能调的东西拒掉更糟
local function _assert_callable(v, what)
    if "function" == type(v) then
        return v
    end
    local mt = getmetatable(v)
    assert(nil ~= mt and ("table" ~= type(mt) or nil ~= rawget(mt, "__call")),
           what .. " must be callable, got " .. type(v))
    return v
end

-- ── GroupBuilder ──────────────────────────────────────────────────────────

---@class GroupBuilder
local GroupBuilder = {}
GroupBuilder.__index = GroupBuilder

local function _gb_new(router, prefix, mws)
    return setmetatable({ _router = router, _prefix = prefix, _mws = mws }, GroupBuilder)
end

---在当前分组基础上追加路径前缀，返回新 GroupBuilder（不修改原对象）
---@param p string 追加的路径前缀，如 "/api"
---@return GroupBuilder
function GroupBuilder:prefix(p)
    return _gb_new(self._router, self._prefix .. p, self._mws)
end

---在当前分组基础上追加中间件，返回新 GroupBuilder（不修改原对象）
---@param ... string|fun(ctx:Ctx, next:fun()) 中间件名称或函数
---@return GroupBuilder
function GroupBuilder:middleware(...)
    local mws = {}
    local args = { ... }
    for _, mw in ipairs(self._mws) do
        mws[#mws + 1] = mw
    end
    -- 用 select("#") 而不是 #args：中间夹一个 nil 时 #args 可能只到 nil 之前，后面的中间件被一并丢掉
    for i = 1, select("#", ...) do
        mws[#mws + 1] = self._router:_resolve(args[i])
    end
    return _gb_new(self._router, self._prefix, mws)
end

---在当前前缀和中间件上下文中执行路由注册闭包
---@param fn fun() 路由注册闭包
function GroupBuilder:group(fn)
    local stack = self._router._stack
    stack[#stack + 1] = { prefix = self._prefix, mws = self._mws }
    local ok, err = pcall(fn)
    stack[#stack] = nil
    if not ok then error(err, 0) end
end

-- ── 内部辅助 ──────────────────────────────────────────────────────────────

-- 本模块所有响应的唯一出口。HEAD 只发头：头与同一资源的 GET 一致（含真实
-- Content-Length）但不发报文体，多发的字节会被对端当成下一条响应的开头。
-- 匹配失败的 4xx、兜底 500 一样要过这里 —— 那两条路径同样可能是 HEAD 请求打进来的。
-- block 与 headers 二选一：模块常量走 block，业务给的走 headers。
-- 先试 C 组包直发，不接的形状再走 lib.http 的 Lua 原路，两条字节相同
local function _send(method, sk, code, block, headers, body)
    local headonly = "HEAD" == method
    if not _http_respond(sk, code, headers, body, headonly, block) then
        _http_response_lua(headonly, sk, code, block, headers, body)
    end
end

-- 拒绝 chunked：回 411 后关连接。对齐 C 侧 router_reject_chunked。
-- 不分流 HEAD（method 给 nil）：HEAD 请求没有报文体，不可能是 chunked
local function _reject_chunked(sk)
    _send(nil, sk, 411, _PLAIN_BLOCK, nil, "chunked request not supported\n")
    srey.close(sk)
end

-- 兜底 500：中间件与 handler 都没写响应时补一发。正文与 C 侧 ROUTER_BODY_500 一字不差；
-- 异常原文只进日志（srey.xpcall 已打 ERROR + traceback），发给远端等于泄露脚本路径与行号。
-- dispatch 与三个流式入口共用，改文案 / 头 / 是否 pcall 只此一处
local function _fallback_500(ctx)
    if ctx.responded then
        return
    end
    pcall(_send, ctx.method, ctx.sk, 500, _PLAIN_BLOCK, nil, "Internal Server Error\n")
    -- 发完置位，口径同 C 侧 router_req_respond：不置的话流式路由的 STREAM_ABORT 回调
    -- 看到的仍是"没人应答过"，按契约再写一次就成了双响应
    ctx.responded = true
end

---@class Ctx
---@field sk      userdata           连接标识
---@field client  integer?           连接方向标志，1=客户端 0=服务端（由 dispatch 透传 on_recved 的同名参数）。
---                                  **不是地址**；取对端 IP 用 utils.remote_addr(ctx.sk)
---@field method  string             HTTP 方法，如 "GET"、"POST"
---@field version string?            HTTP 版本，如 "HTTP/1.1"
---@field path    string             请求路径，如 "/user/42"
---@field params  table<string,string>  路由路径参数，dispatch 匹配后填充
---@field query   table<string,string>  URL 查询参数
---@field body    string?            请求体，无则为 nil。惰性物化：首次访问才从 pack 取，
---                                  出了 dispatch 再读一律为 nil（详见模块头）
---@field headers table<string,string>  请求头，键为线格式原样大小写，取值须大小写一致；
---                                  按名字取单个头用 ctx:header。惰性物化同 body，但出了
---                                  dispatch 再读拿到的是空表而不是 nil，分不清"请求没带
---                                  这个头"和"读晚了"
---@field responded boolean            已响应标志;ctx:* 方法自动置位,dispatch 据此补兜底 500;延迟/手动响应须手动置 true 且勿直接调 http.response(否则与兜底叠成双响应)
---@field _admitted boolean?           流式路由准入标志，router 内部填写
---@field text    fun(self:Ctx, code:integer, body:string?)        纯文本响应
---@field json    fun(self:Ctx, code:integer, tbl:table<any,any>)  JSON 响应，自动附加 Content-Type
---@field html    fun(self:Ctx, code:integer, body:string?)        HTML 响应，自动附加 Content-Type
---@field respond fun(self:Ctx, code:integer, headers:table<string,any>?, body:string?)  自定义响应
---@field header  fun(self:Ctx, name:string):string?  按名字取单个请求头，大小写无关

-- 值转文本。nil 保持 nil：非 nil 统一转 string 是因为数字等类型直接往下传会被
-- http.response 的类型分派当"无 body"丢掉。整数值的浮点走 num_str——Lua 的 / 恒出浮点,
-- tostring(84/2) 是 "42.0",生成的 URL 打回来后 params 里就是字符串 "42.0"。只对真数字转,
-- num_str 会把字符串 "42.0" 也算成 42
local function _val_str(v)
    if nil == v then
        return nil
    end
    if "number" ~= type(v) then
        return tostring(v)
    end
    return num_str(v)
end

-- ctx 响应方法：共享一份挂在 CtxMeta.__index，避免每请求重建 4 个闭包。
-- 四个方法发完都置位 responded（dispatch 据此决定补不补兜底 500），HEAD 分流见 _send
local CtxMethods = {}
function CtxMethods:text(code, body)
    -- 带 Content-Type：不带的话浏览器与代理会按内容嗅探，回显用户输入的纯文本能被当成
    -- text/html 执行。C 侧 router_req_text 写的就是 text/plain，本模块的 4xx/5xx 也走
    -- _PLAIN_BLOCK，只有这里漏了。已是字符串（最常见）就不过 _val_str
    if "string" ~= type(body) then
        body = _val_str(body)
    end
    _send(self.method, self.sk, code, _PLAIN_BLOCK, nil, body)
    self.responded = true
end
function CtxMethods:json(code, tbl)
    _send(self.method, self.sk, code, nil, nil, tbl)
    self.responded = true
end
function CtxMethods:html(code, body)
    if "string" ~= type(body) then
        body = _val_str(body)
    end
    _send(self.method, self.sk, code, _HTML_BLOCK, nil, body)
    self.responded = true
end
function CtxMethods:respond(code, headers, body)
    _send(self.method, self.sk, code, nil, headers, body)
    self.responded = true
end
-- 按名字取单个请求头，大小写无关：比较走 C 的 buf_icompare，不物化头表也不分配。
-- 口径同 C 侧 router_req_header——流式过了首帧 _pack 已摘，那之后恒返 nil，改读 ctx.headers
function CtxMethods:header(name)
    return self._pack and http.head(self._pack, name)
end
-- 下面 query / params 的惰性构造：给一张空表
local function _new_tbl()
    return {}
end
-- headers / body 惰性物化：绝大多数 handler 只 ctx:text 响应，从不读它们，而急切物化每请求
-- 要造一张头表加 20~30 次短字符串分配。首次访问时才建，建好 rawset 进 ctx，之后走 rawget。
-- 取值全靠 _pack，而 pack 是 dispatch 作用域的（与 C 侧 router_req 同口径），故 dispatch
-- 末尾会把 _pack 置 nil：延迟到 fork/timer 里再读，拿到的是 nil 而不是悬空指针
local _ctx_lazy = {
    headers = function(ctx)
        return ctx._pack and http.heads(ctx._pack) or {}
    end,
    body = function(ctx)
        return ctx._pack and http.datastr(ctx._pack) or nil
    end,
    -- match_pack 对空的查询串 / 路径参数给 nil 不建表，读到时才给空表；不依赖 _pack
    query = _new_tbl,
    params = _new_tbl,
}
local CtxMeta = {
    __index = function(ctx, key)
        local m = CtxMethods[key]
        if nil ~= m then
            return m
        end
        local build = _ctx_lazy[key]
        if nil == build then
            return nil
        end
        local v = build(ctx)
        if nil ~= v then
            rawset(ctx, key, v)-- 物化一次即固定下来，之后不再依赖 _pack
        end
        return v
    end,
}
-- 链末位的 next：后面没有东西可跑，全模块共用这一个，不必每请求现建闭包
local function _nop()
end
-- 一层中间件 f 接上它后面已编好的 after，得到这一层起跑的 step(ctx)。每层的 next 绑死自己的下一位，
-- 不能全链共用一个游标：重试型中间件 pcall(next) 后再调一次会跳层。next 每请求现建，只捕获 ctx
local function _wrap_step(f, after)
    return function(ctx)
        f(ctx, function()
            after(ctx)
        end)
    end
end
-- 把执行链从末位往前编成一个 step(ctx)，调它即从第一层跑起；异常向上抛出由 dispatch 统一捕获。
-- 不调 next 即在此截断，next 返回后仍可做后置处理
local function _compile_chain(chain)
    local n = #chain
    local last = chain[n]
    local step = function(ctx)
        last(ctx, _nop)
    end
    for i = n - 1, 1, -1 do
        step = _wrap_step(chain[i], step)
    end
    return step
end

-- 流式路由的链尾哨兵：跑到这里说明每个中间件都调了 next。中间件在哪层截断从链外看不出来，
-- 只能靠它留个记号
local function _admit(ctx)
    ctx._admitted = true
end

-- ── Router ────────────────────────────────────────────────────────────────

---@class Router
local Router = {}
Router.__index = Router

---创建独立路由器实例
---@return Router
function Router.new()
    local r = setmetatable({
        _c_router  = _srey_router_new(), -- C 路由器（持有路径段数组，匹配在 C 侧完成）
        -- [C索引] = {handler, mws, raw}。C 侧索引只增不回收也没有删除接口，add 成功即在此写入，
        -- 中间无失败点，所以 match 命中的 idx 必然取得到 entry
        _routes    = {},
        _named     = {},    -- 命名路由索引：name → entry，由 :name("key") 写入，Router:url 反查
        _global_mw = {},    -- 全局中间件列表，对所有路由生效
        _mw_reg    = {},    -- 具名中间件注册表：name → fun，由 :define() 写入
        _stack     = {},    -- 分组上下文栈，group() 进入时压栈、退出时弹栈
        _mw_version = 0,    -- 全局中间件版本号，use() 追加时自增，route chain 缓存据此失效重建
        -- 正在接收的流式请求：[skid] = {sk, route, ctx}。skid 进程内唯一且不复用，见 _st_drop
        _streams   = {},
    }, Router)
    return r
end

-- 自订阅连接关闭：流式请求上下文只有 _st_drop 能回收，而 srey.on_closed 是单槽、库抢不到，
-- 漏接就是每条没收齐的 chunked 请求常驻约 5KB 直到进程退出。业务仍可照常用 on_closed。
-- watch_closed 没有反注册，故整个模块只订一次：没有流式路由的 router 不占位，有的也只进弱键表
function Router:_watch_closed_once()
    stream_routers[self] = true
    if st_watching then
        return
    end
    st_watching = true
    srey.watch_closed(function(_, sk)
        local skid = sk.skid
        for r in pairs(stream_routers) do
            if nil ~= r._streams[skid] then
                -- 观察者由 _net_close_dispatch 在主线程直接调，不在协程里，而 _st_drop 要回调用户
                -- 的 on_chunk(ABORT)——它在其余三条路径上都是可挂起的，这里 fork 出协程保持一致
                srey.fork(function()
                    r:_st_drop(skid)
                end)
            end
        end
    end)
end

---注册具名中间件，后续可通过名称字符串在 use / middleware / 路由 mws 中引用
---@param name string 中间件名称
---@param fn fun(ctx:Ctx, next:fun()) 中间件函数
function Router:define(name, fn)
    self._mw_reg[name] = _assert_callable(fn, "middleware '" .. tostring(name) .. "'")
end

-- 解析中间件：字符串 → 注册表查找，其余 → 原样透传。两条路都得过 _assert_callable，
-- 具名的那条尤其不能漏：注册时不校验，就要拖到每个请求才炸成 500
function Router:_resolve(mw)
    if type(mw) == "string" then
        local fn = self._mw_reg[mw]
        assert(fn, "middleware not defined: " .. mw)
        return _assert_callable(fn, "middleware '" .. mw .. "'")
    end
    return _assert_callable(mw, "middleware")
end

-- 合并当前 group 栈的前缀与中间件
function Router:_ctx()
    local prefix = ""
    local mws = {}
    for _, frame in ipairs(self._stack) do
        prefix = prefix .. frame.prefix
        for _, mw in ipairs(frame.mws) do
            mws[#mws + 1] = mw
        end
    end
    return prefix, mws
end

---@class RouterSeg
---@field key string  占位符名字；末尾通配为 "*"（占位符名字只认 \w，与它撞不上）
---@field opt boolean 缺参时可否整段丢弃（{x?} 与末尾通配为 true）

---@class RouteEntry
---@field raw     string                          注册时的完整路径（含 prefix），调试用
---@field handler fun(ctx:Ctx)                    路由处理函数。注册后只读：首次 dispatch 会把它连同
---                                               中间件拼成执行链缓存起来，之后改这个字段不会生效
---@field mws     fun(ctx:Ctx,next:fun())[]        路由级中间件列表（分组中间件已静态合并）。
---                                               同 handler，注册后只读
---@field _segs   (string|RouterSeg)[]             注册时 C 侧解析好的段序列，Router:url 回填占位符用
---@field _name   string?                         命名路由键，由 :name("key") 写入
---@field _step   fun(ctx:Ctx)?                    dispatch 缓存的执行链（全局中间件+路由级+handler），已编成从第一层跑起的 step
---@field _chain_ver integer?                     _step 对应的全局中间件版本号，与 Router._mw_version 不等即重建
---@field name    fun(self:RouteEntry,n:string):RouteEntry  链式命名方法

-- 路由注册的共同实现；handler 与 on_chunk 恰有一个非 nil，决定这条是普通派发还是流式接收。
-- 返回 entry，支持 :name() 链式调用
function Router:_add_common(method, path, handler, on_chunk, extra_mws)
    local prefix, ctx_mws = self:_ctx()
    local full = prefix .. path
    -- handler 漏传（一个笔误就够了）在 dispatch 时表现为：chain 里少一项 → 中间件跑完没人响应
    -- → 兜底 500，日志里也没有线索。C 侧同款情形是打日志拒掉的，这里对齐
    local cb = handler or on_chunk
    if "function" ~= type(cb) then
        WARN("router: %s '%s' handler must be a function, got %s.", method, full, type(cb))
        return _bad_entry
    end
    local mws = ctx_mws
    if extra_mws then
        -- 不能用 ipairs：撞上中间的 nil 就停，后面的中间件一起丢掉且 _resolve 压根不被调用。
        -- 表表示不了"中间有个 nil"，所以拿 pairs 的计数与 # 比一次，对不上就当场报错
        local n = #extra_mws
        assert(table_size(extra_mws) == n, "middleware list must not contain nil")
        for i = 1, n do
            mws[#mws + 1] = self:_resolve(extra_mws[i])
        end
    end
    -- 去重由 C 侧 router_add_index 做：它按 (已注册项的掩码把新掩码整个包住, 匹配意义上的
    -- 段序列) 比对，能覆盖这里用字符串 key 看不出来的两类——先注册的 ANY /x 遮住后注册的
    -- GET /x、/u/{id} 与 /u/{uid} 参数名不同但路由等价。dispatch 取首条命中，被遮的永远够不着
    local ok, code, csegs = self._c_router:add(method, full)
    if not ok then
        if -2 == code then
            WARN("router: %s '%s' is shadowed by an existing route, this registration is ignored.", method, full)
        else
            WARN("router: path '%s' rejected.", full)
        end
        return _bad_entry
    end
    local idx = code
    local router = self
    local entry = {
        raw      = full,     -- 原始完整路径字符串（含前缀），调试用
        handler  = handler,  -- 普通路由处理函数 fun(ctx)；流式路由为 nil
        on_chunk = on_chunk, -- 流式路由数据回调 fun(ctx, slice, data)；普通路由为 nil
        mws      = mws,      -- 路由级中间件列表（分组中间件已静态合并进来）
        _segs    = csegs,    -- C 侧解析好的段序列，Router:url 回填占位符用
        _name    = nil,      -- 命名路由键，由 :name("key") 写入
    }
    -- :name("key") 链式命名路由
    entry.name = function(_, n)
        entry._name = n
        router._named[n] = entry
        return entry
    end
    self._routes[idx] = entry  -- 以 C 侧路由索引为键
    return entry
end

-- 普通路由注册
function Router:_add(method, path, handler, extra_mws)
    return self:_add_common(method, path, handler, nil, extra_mws)
end

-- 流式路由注册
function Router:_add_stream(method, path, on_chunk, extra_mws)
    self:_watch_closed_once()
    return self:_add_common(method, path, nil, on_chunk, extra_mws)
end

-- 拼执行链（全局中间件 → 路由级 → 末位）、编成 step 缓存到 route：末位普通路由是 handler，
-- 流式路由是准入哨兵。注册期即静态确定，仅当全局中间件版本变化（use() 追加）时重建。
-- 内部方法写成 local 供派发路径直调（省一次元表查找），Router._chain_of 留作同名入口
local function _chain_of(self, route)
    if route._chain_ver == self._mw_version then
        return route._step
    end
    local chain = {}
    for _, mw in ipairs(self._global_mw) do
        chain[#chain + 1] = mw
    end
    for _, mw in ipairs(route.mws) do
        chain[#chain + 1] = mw
    end
    chain[#chain + 1] = route.handler or _admit
    local step = _compile_chain(chain)
    route._step = step
    route._chain_ver = self._mw_version
    return step
end
Router._chain_of = _chain_of

---注册全局中间件，对所有路由生效
---@param mw string|fun(ctx:Ctx, next:fun()) 中间件名称或函数
function Router:use(mw)
    self._global_mw[#self._global_mw + 1] = self:_resolve(mw)
    self._mw_version = self._mw_version + 1
end

---注册 GET 路由；同时接住 HEAD——HEAD 的语义就是"要 GET 的头不要体"，分开注册会让 HEAD 落 404。
---handler 照常写 body，响应侧按请求方法自动抑制报文体（见 _send）
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:get(path, handler, mws)
    return self:_add("GET|HEAD", path, handler, mws)
end

---注册独立的 HEAD 路由。多数情况不需要：get() 已经覆盖 HEAD。只在 HEAD 要跑与 GET 不同的
---handler 时用，且须注册在同路径的 get() 之前——反过来会被那条的 GET|HEAD 掩码整个包住
---而遭遮蔽忽略（先注册者胜）
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:head(path, handler, mws)
    return self:_add("HEAD", path, handler, mws)
end

---注册 OPTIONS 路由
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:options(path, handler, mws)
    return self:_add("OPTIONS", path, handler, mws)
end

---注册 POST 路由
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:post(path, handler, mws)
    return self:_add("POST", path, handler, mws)
end

---注册 PUT 路由
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:put(path, handler, mws)
    return self:_add("PUT", path, handler, mws)
end

---注册 DELETE 路由
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:delete(path, handler, mws)
    return self:_add("DELETE", path, handler, mws)
end

---注册 PATCH 路由
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:patch(path, handler, mws)
    return self:_add("PATCH", path, handler, mws)
end

---注册任意方法路由（匹配所有 HTTP 方法）
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:any(path, handler, mws)
    return self:_add("ANY", path, handler, mws)
end

---注册流式 POST 路由：请求体逐块交给 on_chunk，router 不缓存；只有 net_recv 认它。连接中途断开
---的清理已由本函数自订阅 CLOSE 接管，不必再把 closed 接到 on_closed。
---与普通路由的不同：中间件链只做准入，它 next 之后的后置处理在请求体到达前就跑完，拦不住已放行
---的流；响应由 on_chunk 自己写，到 SLICE_TYPE.END 未响应兜底 500；chunked 时 ctx.body 为 nil、
---请求体只从 data 取，slice == 0 那次 data 即 ctx.body。
---on_chunk 与准入中间件都不能挂起（分片逐帧投递，挂起会丢帧或让两帧交错）；要异步就 srey.fork 并置 ctx.responded = true
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param on_chunk fun(ctx:Ctx, slice:integer, data:string?) 数据回调，slice 见 SLICE_TYPE 与 STREAM_ABORT
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:post_stream(path, on_chunk, mws)
    return self:_add_stream("POST", path, on_chunk, mws)
end

---注册流式 PUT 路由，语义同 post_stream
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param on_chunk fun(ctx:Ctx, slice:integer, data:string?) 数据回调，slice 见 SLICE_TYPE 与 STREAM_ABORT
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:put_stream(path, on_chunk, mws)
    return self:_add_stream("PUT", path, on_chunk, mws)
end

---返回 GroupBuilder，用于流式 :prefix():middleware():group() 路由分组
---@param p string 路径前缀
---@return GroupBuilder
function Router:prefix(p)
    return _gb_new(self, p, {})
end

---返回 GroupBuilder，用于流式 :middleware():group() 中间件分组
---@param ... string|fun(ctx:Ctx, next:fun()) 中间件名称或函数
---@return GroupBuilder
function Router:middleware(...)
    local mws = {}
    local args = { ... }
    -- #args 的问题同 GroupBuilder:middleware
    for i = 1, select("#", ...) do
        mws[#mws + 1] = self:_resolve(args[i])
    end
    return _gb_new(self, "", mws)
end

-- 通配段是路径余部，里面的 '/' 是真分隔符不能编码，只编码段内内容，空段与首尾斜杠原样留着——
-- 拆开再拼会把调用方给的值改掉。占位符段不走这条：{id} 取值里的 '/' 必须编成 %2F，
-- 否则业务给个 "42/admin" 就凭空多出一段
local function _encode_piece(piece)
    return url.encode(piece, 0)
end
local function _encode_path(v)
    return (string.gsub(v, "[^/]+", _encode_piece))
end

---按名字反向生成 URL：拿注册时 C 侧解析好的段序列回填占位符，params 里没被占位符用掉的键拼成查询串。
---口径同 Laravel 的 route()：必填占位符缺参报错，可选占位符（{x?}）与末尾通配（*）缺参时整段丢弃。
---占位符取值与查询串都经 url.encode，路径段用 %20、查询串用 '+' 编码空格；通配段的 '/' 不编码。
---查询串按 "键=值" 整串排序：Lua 表无序，不排的话同一组参数每个进程给出的 URL 都不一样
---@param name string :name("key") 登记的名字
---@param params table<string,any>? 占位符取值；末尾通配取 params["*"]，其余键按查询串附加
---@return string url 以 "/" 开头的路径，带查询串时以 "?" 分隔
function Router:url(name, params)
    local entry = self._named[name]
    if not entry then
        error(string.format("router: no named route '%s'", tostring(name)), 2)
    end
    params = params or {}
    local used = {}
    local segs = {}
    local csegs = entry._segs
    local seg, val
    for i = 1, #csegs do
        seg = csegs[i]
        if "string" == type(seg) then
            segs[#segs + 1] = seg
        else
            used[seg.key] = true
            val = params[seg.key]
            if nil ~= val then
                val = _val_str(val)
                segs[#segs + 1] = ("*" == seg.key) and _encode_path(val)
                                                   or url.encode(val, 0)
            elseif not seg.opt then
                error(string.format("router: url('%s') missing param '%s'", name, seg.key), 2)
            end
        end
    end
    local path = "/" .. table.concat(segs, "/")
    local qs = {}
    for k, v in pairs(params) do
        if not used[k] then
            qs[#qs + 1] = url.encode(_val_str(k)) .. "=" .. url.encode(_val_str(v))
        end
    end
    if 0 == #qs then
        return path
    end
    table.sort(qs)
    return path .. "?" .. table.concat(qs, "&")
end

---两条派发入口（一次到齐 / chunked 首帧）共用的前半段：C 侧取请求行并匹配 → 建 ctx。
---方法识别、url_parse、空段过滤、路由扫描全在 C 侧完成，状态码由 C 一处决定（200/400/404/405）。
---匹配不上就按那个码回响应；要不要顺带关连接由调用方给 close_on_fail 决定
---写成 local 供派发路径直调（同 _chain_of），Router._match_ctx 留作同名入口
---@param self Router
---@param sk userdata 连接标识
---@param pack lightuserdata http_pack_ctx 指针
---@param client integer? 连接方向标志，1=客户端 0=服务端；不是地址，取对端 IP 用 utils.remote_addr(sk)
---@param close_on_fail boolean 匹配失败回完响应后是否顺带关连接
---@return Ctx? ctx 请求上下文；非 HTTP 包或未匹配返回 nil（响应已发）
---@return RouteEntry? route 命中的路由条目
local function _match_ctx(self, sk, pack, client, close_on_fail)
    -- ok 为 nil 表示非 HTTP 包（如连接/断开事件），直接忽略
    local ok, code, method, version, path, query, idx, params = self._c_router:match_pack(pack)
    if nil == ok then
        return nil
    end
    if not ok then
        _send(method, sk, code, _PLAIN_BLOCK, nil, _CODE_BODY[code] or (http.code_status(code) .. "\n"))
        if close_on_fail then
            srey.close(sk)
        end
        return nil
    end
    local route = self._routes[idx]
    -- 匹配到索引却在 _routes 里没有条目：注册中途失败留下的空洞。C 侧对同类配置错误答 500
    -- (_router_entry_misconfigured)，这里同处置——不挡的话 _chain_of 会在调用方的 xpcall
    -- 之外抛出去，客户端一个字节都收不到，只能等自己超时
    if not route then
        _send(method, sk, 500, _PLAIN_BLOCK, nil, _CODE_BODY[500])
        if close_on_fail then
            srey.close(sk)
        end
        return nil
    end
    -- 建请求上下文。headers / body 惰性取（见 _ctx_lazy）。query / params 有一个非空就都写进构造器：
    -- 构造器按字段数一次建好哈希部，事后再塞第 9 个键会整张重建一次。两个都空时用 7 字段的构造器，
    -- 哈希部小一半，读到时再由 _ctx_lazy 给空表
    if nil == query and nil == params then
        return setmetatable({
            sk      = sk,
            client  = client,
            method  = method,
            version = version,
            path    = path,
            _pack   = pack,
            responded = false,
        }, CtxMeta), route
    end
    return setmetatable({
        sk      = sk,
        client  = client,
        method  = method,
        version = version,
        path    = path,
        query   = query,
        params  = params,
        _pack   = pack,
        responded = false,
    }, CtxMeta), route
end
Router._match_ctx = _match_ctx

---分发 HTTP 请求：解析方法和路径，匹配路由后执行中间件链；URL 解析失败响应 400,无匹配响应 404。
---只认一次到齐的请求（on_recved 的 slice == 0）；chunked 请求须走 net_recv，
---直接喂给本函数会把首包当成一个 body 为空的完整请求
---@param sk userdata 连接标识
---@param pack lightuserdata http_pack_ctx 指针
---@param client integer? 连接方向标志，1=客户端 0=服务端；不是地址，取对端 IP 用 utils.remote_addr(sk)
function Router:dispatch(sk, pack, client)
    local ctx, route = _match_ctx(self, sk, pack, client, false)
    if not ctx then
        return
    end
    -- 链内任意位置抛出异常均由 srey.xpcall 兜底（自动 ERROR + traceback），避免 handler/中间件崩溃丢失响应。
    -- 缓存命中（绝大多数请求）就不进 _chain_of
    local step = (route._chain_ver == self._mw_version) and route._step or _chain_of(self, route)
    local run_ok = srey.xpcall(step, ctx)
    -- 流式路由命中一次到齐的请求：准入通过才把请求体按 slice == 0 一次交出去
    if run_ok and route.on_chunk and ctx._admitted then
        srey.xpcall(route.on_chunk, ctx, 0, ctx.body)
    end
    if not ctx.responded then
        _fallback_500(ctx)
    end
    -- pack 的生命周期到本次 dispatch 为止（同 C 侧 router_req）。摘掉引用后，handler 若把 ctx
    -- 带进 fork/timer 再读 headers/body，拿到的是 nil；留着的话就是对已释放内存解引用
    ctx._pack = nil
end

-- 摘掉一条流式记录并投 STREAM_ABORT。正常收尾走 _st_feed 的 END 分支，不经过这里，两者只会来一个。
-- 流表按 skid 做键：skid 进程内全局唯一且不复用（C 侧 _router_st_hash 按整个 sock_ctx，同样不会认错）。
-- sk.skid 每读一次是一次 C 调用，各入口只读一次、往下传 skid
function Router:_st_drop(skid)
    local rec = self._streams[skid]
    if not rec then
        return
    end
    self._streams[skid] = nil
    -- ABORT 只做清理，它自己再抛也没人接得住，xpcall 兜住并打 traceback
    srey.xpcall(rec.route.on_chunk, rec.ctx, STREAM_ABORT, nil)
end

-- 调一次流式回调；抛异常就终止这条流：补 500、关连接，再投 ABORT 给最后一次清理机会
function Router:_st_call(rec, slice, data)
    local ctx = rec.ctx
    if srey.xpcall(rec.route.on_chunk, ctx, slice, data) then
        return
    end
    local sk = ctx.sk
    _fallback_500(ctx)
    srey.close(sk)
    self:_st_drop(sk.skid)
end

-- 流式首帧：匹配路由 → 跑准入链 → 建记录 → 回调 SLICE_TYPE.START。
-- 任一步不通过都回响应并关连接：请求体还在后面，连接留着也收不了
function Router:_st_begin(sk, pack, client)
    local skid = sk.skid
    -- 同连接已有记录说明上一条流式请求没收尾，丢旧的重开。排在匹配之前：新的首帧一到，
    -- 旧记录就作废了，哪怕这一帧本身不合法也不该把它留着
    self:_st_drop(skid)
    -- 匹配失败要关连接：请求体还在后面，连接留着也收不了
    local ctx, route = _match_ctx(self, sk, pack, client, true)
    if not ctx then
        return
    end
    -- 命中的不是流式路由：请求体正一块块往这边来，普通 handler 接不住，回 411 让客户端改用定长
    if not route.on_chunk then
        _reject_chunked(sk)
        return
    end
    local step = (route._chain_ver == self._mw_version) and route._step or _chain_of(self, route)
    local run_ok = srey.xpcall(step, ctx)
    -- 链尾是准入哨兵而非 handler；中间件截断即拒绝，它没写响应就兜底 500
    if not run_ok or not ctx._admitted then
        _fallback_500(ctx)
        srey.close(sk)
        return
    end
    -- 这条 ctx 要跨帧活到 END/ABORT，而 pack 只在首帧有效：把 headers/body 就地固化下来，
    -- 首帧回调跑完再摘 _pack（契约说首帧读得到请求头，提前摘 ctx:header 在 START 里恒返 nil）。
    -- 不固化的话后续帧读 headers 会拿到 nil —— 流式是低频路径，这次急切物化的开销无所谓
    local _ = ctx.headers
    _ = ctx.body
    -- 建记录要排在回调之前：回调里若关连接，也才找得到这条记录
    local rec = { sk = sk, route = route, ctx = ctx }
    self._streams[skid] = rec
    self:_st_call(rec, SLICE_TYPE.START, nil)
    ctx._pack = nil
end

-- 流式中间/结束帧：原样把 slice 与数据交给 on_chunk
function Router:_st_feed(sk, pack, slice)
    -- 首帧被拒过（连接那时就关了）或连接已关、流记录已被摘掉，后续帧静默丢
    local skid = sk.skid
    local rec = self._streams[skid]
    if not rec then
        return
    end
    if SLICE_TYPE.END ~= slice then
        self:_st_call(rec, slice, http.datastr(pack))
        return
    end
    -- 结束帧：先摘记录再回调，这样它抛异常也不会再补一次 ABORT —— END 本身就是收尾信号
    self._streams[skid] = nil
    local ctx = rec.ctx
    srey.xpcall(rec.route.on_chunk, ctx, slice, nil)
    _fallback_500(ctx)
end

---on_recved 回调的标准实现：一次到齐的请求直接 dispatch；chunked 命中流式路由则逐帧交给它，
---命中普通路由则回 411 并关连接。参数与 on_recved 一一对应，整串转发即可
---@param pktype PACK_TYPE 协议类型（未使用）
---@param sk userdata 连接标识
---@param client integer? 连接方向标志，1=客户端 0=服务端
---@param slice integer 分片标志，0 表示一次到齐的完整请求
---@param data lightuserdata? http_pack_ctx 指针
---@param size integer? HTTP 包的记账字节数，口径见 srey.lua 的 Message.size（未使用）
function Router:net_recv(pktype, sk, client, slice, data, size)
    if 0 == slice then
        return self:dispatch(sk, data, client)
    end
    if SLICE_TYPE.START == slice then
        -- 一条流式路由都没注册也照样走 _st_begin：匹配不上得回 404/400/405，
        -- 与一次到齐的同一请求同码。411 只表示"路由在，但它接不住 chunked"（对齐 C 侧）
        return self:_st_begin(sk, data, client)
    end
    return self:_st_feed(sk, data, slice)
end

---连接关闭时清掉该连接尚未收齐的流式请求。本模块注册第一条流式路由时已经经 srey.watch_closed
---自订阅（模块级，只订一次），正常不必再调；留作自己接管 CLOSE 分发（不走 srey 的分发器）时的手动入口。
---回收前会投一次 STREAM_ABORT
---@param sk userdata 连接标识
function Router:closed(sk)
    self:_st_drop(sk.skid)
end

-- ── 默认实例（Laravel Route facade 风格）────────────────────────────────

local Route    = Router.new()
Route.new      = Router.new -- 暴露构造函数，支持 require("advance.router").new()
Router.STREAM_ABORT = STREAM_ABORT -- 流式回调的中止 slice，见 post_stream
return Route
