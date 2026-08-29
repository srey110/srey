-- HTTP 路由/中间件框架，仿 Laravel 风格。
-- 路径参数使用 {param} 语法，可选参数 {param?}，末尾通配 *。
-- 快速上手:
--   local Route = require("advance.router")
--   -- 具名中间件注册（可选）
--   Route:define("auth", function(ctx, next)
--       if ctx.headers["x-api-key"] ~= "secret" then ctx:text(401, "Unauthorized\n") return end
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
--   -- 命名路由
--   Route:get("/user/{id}", handler):name("user.show")
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
--   -- 要对端 IP 用 utils.remote_addr(fd)）
--   Route:dispatch(fd, skid, data)
--   Route:dispatch(fd, skid, data, client)
--   -- 流式路由（chunked 请求体逐块到，router 不缓存）；用它就得把两个回调都接上
--   local SLICE_TYPE = require("lib.srey").SLICE_TYPE
--   Route:post_stream("/upload", function(ctx, slice, data)
--       if 0 == slice then ctx:text(200, data or "")                        -- 非 chunked，一次到齐
--       elseif SLICE_TYPE.START == slice then ctx.buf = {}                  -- 首帧，头部/参数已可读
--       elseif SLICE_TYPE.SLICE == slice then ctx.buf[#ctx.buf + 1] = data  -- 数据块
--       elseif SLICE_TYPE.END == slice then ctx:text(200, table.concat(ctx.buf))
--       else ctx.buf = nil end                                              -- STREAM_ABORT，只清理
--   end)
--   srey.on_recved(function(...) Route:net_recv(...) end)
--   srey.on_closed(function(_, fd, skid) Route:closed(fd, skid) end)
-- ctx 字段:
--   ctx.fd / ctx.skid / ctx.client
--   ctx.method  -- "GET" "POST" ...
--   ctx.version -- "HTTP/1.1"
--   ctx.path    -- "/user/42"
--   ctx.params  -- 路由参数 {id="42"}
--   ctx.query   -- 查询参数 {page="1"}
--   ctx.body    -- 请求体（可能为 nil）
--   ctx.headers -- 请求头 table
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
local SLICE_TYPE = srey.SLICE_TYPE
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
    local mws  = {}
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

local _PLAIN_HEADERS = { ["Content-Type"] = "text/plain; charset=utf-8" }

-- 拒绝 chunked：回 411 后关连接。对齐 C 侧 router_reject_chunked
local function _reject_chunked(fd, skid)
    http.response(fd, skid, 411, _PLAIN_HEADERS, "chunked request not supported\n")
    srey.close(fd, skid)
end

-- 兜底 500：中间件与 handler 都没写响应时补一发。正文与 C 侧 ROUTER_BODY_500 一字不差；
-- 异常原文只进日志（srey.xpcall 已打 ERROR + traceback），发给远端等于泄露脚本路径与行号。
-- dispatch 与三个流式入口共用，改文案 / 头 / 是否 pcall 只此一处
local function _fallback_500(ctx)
    if ctx.responded then
        return
    end
    pcall(http.response, ctx.fd, ctx.skid, 500, _PLAIN_HEADERS, "Internal Server Error\n")
end

---@class Ctx
---@field fd      integer            socket fd
---@field skid    integer            连接 skid
---@field client  integer?           连接方向标志，1=客户端 0=服务端（由 dispatch 透传 on_recved 的同名参数）。
---                                  **不是地址**；取对端 IP 用 utils.remote_addr(ctx.fd)
---@field method  string             HTTP 方法，如 "GET"、"POST"
---@field version string?            HTTP 版本，如 "HTTP/1.1"
---@field path    string             请求路径，如 "/user/42"
---@field params  table<string,string>  路由路径参数，dispatch 匹配后填充
---@field query   table<string,string>  URL 查询参数
---@field body    string?            请求体，无则为 nil
---@field headers table<string,string>  请求头
---@field responded boolean            已响应标志;ctx:* 方法自动置位,dispatch 据此补兜底 500;延迟/手动响应须手动置 true 且勿直接调 http.response(否则与兜底叠成双响应)
---@field _admitted boolean?           流式路由准入标志，router 内部填写
---@field text    fun(self:Ctx, code:integer, body:string?)        纯文本响应
---@field json    fun(self:Ctx, code:integer, tbl:table)           JSON 响应，自动附加 Content-Type
---@field html    fun(self:Ctx, code:integer, body:string?)        HTML 响应，自动附加 Content-Type
---@field respond fun(self:Ctx, code:integer, headers:table?, body:string?)  自定义响应

-- body 为 nil 时保持 nil，非 nil 统一转 string——数字等类型直接往下传会被
-- http.response 的类型分派当"无 body"丢掉
local function _body_str(body)
    if nil == body then
        return nil
    end
    return tostring(body)
end

-- ctx 响应方法：共享一份挂在 CtxMeta.__index，避免每请求重建 4 个闭包
local CtxMethods = {}
function CtxMethods:text(code, body)
    http.response(self.fd, self.skid, code, nil, _body_str(body))
    self.responded = true
end
function CtxMethods:json(code, tbl)
    http.response(self.fd, self.skid, code, nil, tbl)
    self.responded = true
end
function CtxMethods:html(code, body)
    http.response(self.fd, self.skid, code,
        { ["Content-Type"] = "text/html; charset=utf-8" }, _body_str(body))
    self.responded = true
end
function CtxMethods:respond(code, headers, body)
    http.response(self.fd, self.skid, code, headers, body)
    self.responded = true
end
-- ctx 元表：仅提供共享响应方法；body/headers 为急切物化的 raw 字段（见 _make_ctx），命中 rawget 不走 __index
local CtxMeta = { __index = CtxMethods }
-- 构造请求上下文；body/headers 急切物化为独立 Lua 值（pack 释放后仍有效，允许 handler 经 srey.fork/timer 延迟访问 ctx），params 由 dispatch 匹配后填充
local function _make_ctx(fd, skid, pack, client, method, parsed, version)
    return setmetatable({
        fd      = fd,
        skid    = skid,
        client  = client,
        method  = method,
        version = version,
        path    = parsed.path,
        query   = parsed.param or {},
        body    = http.datastr(pack),
        headers = http.heads(pack) or {},
        responded = false,
    }, CtxMeta)
end

-- 执行中间件链，异常向上抛出由 dispatch 统一捕获。不调 next 即在此截断，next 返回后仍可做后置处理。
-- 每层的 next 绑死自己的下一位，不能全链共用一个游标：重试型中间件 pcall(next) 后再调一次会跳层
local function _run_chain(chain, ctx, i)
    if i > #chain then
        return
    end
    chain[i](ctx, function()
        _run_chain(chain, ctx, i + 1)
    end)
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
    return setmetatable({
        _c_router  = _srey_router_new(), -- C 路由器（持有路径段数组，匹配在 C 侧完成）
        -- [C索引] = {handler, mws, raw}。C 侧索引只增不回收也没有删除接口，add 成功即在此写入，
        -- 中间无失败点，所以 match 命中的 idx 必然取得到 entry
        _routes    = {},
        _named     = {},    -- 命名路由索引：name → entry，由 :name("key") 写入
        _global_mw = {},    -- 全局中间件列表，对所有路由生效
        _mw_reg    = {},    -- 具名中间件注册表：name → fun，由 :define() 写入
        _stack     = {},    -- 分组上下文栈，group() 进入时压栈、退出时弹栈
        _mw_version = 0,    -- 全局中间件版本号，use() 追加时自增，route chain 缓存据此失效重建
        -- 正在接收的流式请求：[fd] = {skid, route, ctx}。只按 fd 索引、把 skid 存进记录里比对，
        -- 省掉每帧一个 "fd:skid" 字符串键；fd 被新连接复用时 skid 对不上，当没有记录处理
        _streams   = {},
    }, Router)
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
    local mws    = {}
    for _, frame in ipairs(self._stack) do
        prefix = prefix .. frame.prefix
        for _, mw in ipairs(frame.mws) do
            mws[#mws + 1] = mw
        end
    end
    return prefix, mws
end

local _bad_entry = {}
_bad_entry.name = function(_, n) WARN("router: :name(%s) on rejected entry.", tostring(n)) return _bad_entry end

---@class RouteEntry
---@field raw     string                          注册时的完整路径（含 prefix），调试用
---@field handler fun(ctx:Ctx)                    路由处理函数。注册后只读：首次 dispatch 会把它连同
---                                               中间件拼成执行链缓存起来，之后改这个字段不会生效
---@field mws     fun(ctx:Ctx,next:fun())[]        路由级中间件列表（分组中间件已静态合并）。
---                                               同 handler，注册后只读
---@field _name   string?                         命名路由键，由 :name("key") 写入
---@field _chain  fun(ctx:Ctx,next:fun())[]?      dispatch 缓存的执行链（全局中间件+路由级+handler）
---@field _chain_ver integer?                     _chain 对应的全局中间件版本号，与 Router._mw_version 不等即重建
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
    local mws  = {}
    for _, mw in ipairs(ctx_mws) do
        mws[#mws + 1] = mw
    end
    if extra_mws then
        -- 不能用 ipairs：撞上中间的 nil 就停，后面的中间件一起丢掉且 _resolve 压根不被调用。
        -- 表表示不了"中间有个 nil"，所以拿 pairs 的计数与 # 比一次，对不上就当场报错
        local n = #extra_mws
        local cnt = 0
        for _ in pairs(extra_mws) do
            cnt = cnt + 1
        end
        assert(cnt == n, "middleware list must not contain nil")
        for i = 1, n do
            mws[#mws + 1] = self:_resolve(extra_mws[i])
        end
    end
    -- 去重由 C 侧 router_add_index 做：它按 (方法掩码有交集, 匹配意义上的段序列) 比对，
    -- 能覆盖这里用字符串 key 看不出来的两类——先注册的 ANY /x 遮住后注册的 GET /x、
    -- /u/{id} 与 /u/{uid} 参数名不同但路由等价。dispatch 取首条命中，被遮的那条永远够不着
    local ok, code = self._c_router:add(method, full)
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
    local entry  = {
        raw      = full,     -- 原始完整路径字符串（含前缀），调试用
        handler  = handler,  -- 普通路由处理函数 fun(ctx)；流式路由为 nil
        on_chunk = on_chunk, -- 流式路由数据回调 fun(ctx, slice, data)；普通路由为 nil
        mws      = mws,      -- 路由级中间件列表（分组中间件已静态合并进来）
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
    return self:_add_common(method, path, nil, on_chunk, extra_mws)
end

-- 拼执行链（全局中间件 → 路由级 → 末位）并缓存到 route：末位普通路由是 handler，
-- 流式路由是准入哨兵。注册期即静态确定，仅当全局中间件版本变化（use() 追加）时重建
function Router:_chain_of(route)
    if route._chain_ver == self._mw_version then
        return route._chain
    end
    local chain = {}
    for _, mw in ipairs(self._global_mw) do
        chain[#chain + 1] = mw
    end
    for _, mw in ipairs(route.mws) do
        chain[#chain + 1] = mw
    end
    chain[#chain + 1] = route.handler or _admit
    route._chain = chain
    route._chain_ver = self._mw_version
    return chain
end

---注册全局中间件，对所有路由生效
---@param mw string|fun(ctx:Ctx, next:fun()) 中间件名称或函数
function Router:use(mw)
    self._global_mw[#self._global_mw + 1] = self:_resolve(mw)
    self._mw_version = self._mw_version + 1
end

---注册 GET 路由
---@param path string 路由路径，支持 {param}、{param?}、* 语法
---@param handler fun(ctx:Ctx) 路由处理函数
---@param mws (string|fun(ctx:Ctx, next:fun()))[]? 路由级中间件列表（名称字符串或函数）
---@return RouteEntry entry 路由条目，可链式调用 :name("key") 命名
function Router:get(path, handler, mws)
    return self:_add("GET", path, handler, mws)
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

---注册流式 POST 路由：请求体逐块交给 on_chunk，router 不缓存；只有 net_recv 认它，用它就必须
---把 closed 接到 on_closed。与普通路由的不同：中间件链只做准入，它 next 之后的后置处理在请求体
---到达前就跑完，拦不住已放行的流；响应由 on_chunk 自己写，到 SLICE_TYPE.END 未响应兜底 500；
---chunked 时 ctx.body 为 nil、请求体只从 data 取，slice == 0 那次 data 即 ctx.body。
---on_chunk 与准入中间件都不能挂起——分片逐帧投递，挂起期间下一帧会在新协程上带同一个 ctx 重入，
---中间件挂起丢帧、on_chunk 挂起则两帧交错；要异步就 srey.fork 并置 ctx.responded = true
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

---分发 HTTP 请求：解析方法和路径，匹配路由后执行中间件链；URL 解析失败响应 400,无匹配响应 404。
---只认一次到齐的请求（on_recved 的 slice == 0）；chunked 请求须走 net_recv，
---直接喂给本函数会把首包当成一个 body 为空的完整请求
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param pack lightuserdata http_pack_ctx 指针
---@param client integer? 连接方向标志，1=客户端 0=服务端；不是地址，取对端 IP 用 utils.remote_addr(fd)
function Router:dispatch(fd, skid, pack, client)
    -- status 为 nil 表示非 HTTP 包（如连接/断开事件），直接忽略
    local status = http.status(pack)
    if not status then
        return
    end
    local method = status[1]
    -- 方法识别、url_parse、空段过滤、路由扫描全在 C 侧完成，状态码由 C 一处决定（200/400/404/405）
    local ok, code, parsed, idx, params = self._c_router:match(method, status[2] or "")
    if not ok then
        http.response(fd, skid, code, _PLAIN_HEADERS, http.code_status(code) .. "\n")
        return
    end
    local route = self._routes[idx]
    local ctx = _make_ctx(fd, skid, pack, client, method, parsed, status[3])
    ctx.params = params
    -- 链内任意位置抛出异常均由 srey.xpcall 兜底（自动 ERROR + traceback），避免 handler/中间件崩溃丢失响应
    local run_ok = srey.xpcall(_run_chain, self:_chain_of(route), ctx, 1)
    -- 流式路由命中一次到齐的请求：准入通过才把请求体按 slice == 0 一次交出去
    if run_ok and route.on_chunk and ctx._admitted then
        srey.xpcall(route.on_chunk, ctx, 0, ctx.body)
    end
    _fallback_500(ctx)
end

-- 摘掉一条流式记录并投 STREAM_ABORT。skid 非 nil 时还要对得上（fd 可能已被新连接复用）。
-- 正常收尾走 _st_feed 的 END 分支，不经过这里，两者只会来一个
function Router:_st_drop(fd, skid)
    local rec = self._streams[fd]
    if not rec or (skid and rec.skid ~= skid) then
        return
    end
    self._streams[fd] = nil
    -- ABORT 只做清理，它自己再抛也没人接得住，xpcall 兜住并打 traceback
    srey.xpcall(rec.route.on_chunk, rec.ctx, STREAM_ABORT, nil)
end

-- 调一次流式回调；抛异常就终止这条流：补 500、关连接，再投 ABORT 给最后一次清理机会
function Router:_st_call(rec, slice, data)
    local ctx = rec.ctx
    if srey.xpcall(rec.route.on_chunk, ctx, slice, data) then
        return
    end
    _fallback_500(ctx)
    srey.close(ctx.fd, ctx.skid)
    self:_st_drop(ctx.fd, ctx.skid)
end

-- 流式首帧：匹配路由 → 跑准入链 → 建记录 → 回调 SLICE_TYPE.START。
-- 任一步不通过都回响应并关连接：请求体还在后面，连接留着也收不了
function Router:_st_begin(fd, skid, pack, client)
    local status = http.status(pack)
    if not status then
        return
    end
    -- 同连接已有记录说明上一条流式请求没收尾，丢旧的重开
    self:_st_drop(fd)
    local method = status[1]
    local ok, code, parsed, idx, params = self._c_router:match(method, status[2] or "")
    if not ok then
        http.response(fd, skid, code, _PLAIN_HEADERS, http.code_status(code) .. "\n")
        srey.close(fd, skid)
        return
    end
    local route = self._routes[idx]
    -- 命中的不是流式路由：请求体正一块块往这边来，普通 handler 接不住，回 411 让客户端改用定长
    if not route.on_chunk then
        _reject_chunked(fd, skid)
        return
    end
    local ctx = _make_ctx(fd, skid, pack, client, method, parsed, status[3])
    ctx.params = params
    local run_ok = srey.xpcall(_run_chain, self:_chain_of(route), ctx, 1)
    -- 链尾是准入哨兵而非 handler；中间件截断即拒绝，它没写响应就兜底 500
    if not run_ok or not ctx._admitted then
        _fallback_500(ctx)
        srey.close(fd, skid)
        return
    end
    -- 建记录要排在回调之前：回调里若关连接，也才找得到这条记录
    local rec = { skid = skid, route = route, ctx = ctx }
    self._streams[fd] = rec
    self:_st_call(rec, SLICE_TYPE.START, nil)
end

-- 流式中间/结束帧：原样把 slice 与数据交给 on_chunk
function Router:_st_feed(fd, skid, pack, slice)
    local rec = self._streams[fd]
    -- 首帧被拒过（连接那时就关了）或 fd 已被新连接复用，后续帧静默丢
    if not rec or rec.skid ~= skid then
        return
    end
    if SLICE_TYPE.END ~= slice then
        self:_st_call(rec, slice, http.datastr(pack))
        return
    end
    -- 结束帧：先摘记录再回调，这样它抛异常也不会再补一次 ABORT —— END 本身就是收尾信号
    self._streams[fd] = nil
    local ctx = rec.ctx
    srey.xpcall(rec.route.on_chunk, ctx, slice, nil)
    _fallback_500(ctx)
end

---on_recved 回调的标准实现：一次到齐的请求直接 dispatch；chunked 命中流式路由则逐帧交给它，
---命中普通路由则回 411 并关连接。参数与 on_recved 一一对应，整串转发即可
---@param pktype PACK_TYPE 协议类型（未使用）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param client integer? 连接方向标志，1=客户端 0=服务端
---@param slice integer 分片标志，0 表示一次到齐的完整请求
---@param data lightuserdata? http_pack_ctx 指针
---@param size integer? 数据字节数（未使用）
function Router:net_recv(pktype, fd, skid, client, slice, data, size)
    if 0 == slice then
        return self:dispatch(fd, skid, data, client)
    end
    if SLICE_TYPE.START == slice then
        -- 一条流式路由都没注册也照样走 _st_begin：匹配不上得回 404/400/405，
        -- 与一次到齐的同一请求同码。411 只表示"路由在，但它接不住 chunked"（对齐 C 侧）
        return self:_st_begin(fd, skid, data, client)
    end
    return self:_st_feed(fd, skid, data, slice)
end

---连接关闭时清掉该连接尚未收齐的流式请求 —— 在 on_closed 回调中调用。
---注册了流式路由就必须接上：ctx 与用户挂在它上面的东西只有这里能回收，
---回收前会投一次 STREAM_ABORT
---@param fd integer socket fd
---@param skid integer 连接 skid
function Router:closed(fd, skid)
    self:_st_drop(fd, skid)
end

-- ── 默认实例（Laravel Route facade 风格）────────────────────────────────

local Route    = Router.new()
Route.new      = Router.new   -- 暴露构造函数，支持 require("advance.router").new()
Router.STREAM_ABORT = STREAM_ABORT -- 流式回调的中止 slice，见 post_stream
return Route
