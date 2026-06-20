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
--   -- on_recved 中分发（client 可选，供中间件访问客户端 IP）
--   Route:dispatch(fd, skid, data)
--   Route:dispatch(fd, skid, data, client)
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

local srey     = require("lib.srey")
local http     = require("lib.http")
local url_mod  = require("srey.url")
local type     = type
local pcall    = pcall
local tostring = tostring
local assert   = assert

-- 路径段数上限,须与 C urlparse.h 的 URL_MAX_PATH_DEPTH 同步;仅校验注册期模板,运行期请求段数由 C url_parse 以同一上限兜底
local _URL_MAX_PATH_DEPTH = 64

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
    for i = 1, #args do
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
-- 须与 C 端 _router_method_str_to_mask 的方法集同步
local _KNOWN_METHODS = {
    GET = true, POST = true, PUT = true, DELETE = true,
    PATCH = true, HEAD = true, OPTIONS = true,
}

-- 将路由路径解析为结构化段数组;非法模板(空名/段数超上限/中置 *)返 nil+errmsg,由 _add 告警并跳过
-- 段类型: lit(字面量) / param({x}) / opt({x?}) / wild(*)
local function _parse(path)
    local segs = {}
    for seg in path:gmatch("[^/]+") do
        if seg == "*" then
            segs[#segs + 1] = { t = "wild" }
        else
            local name, q = seg:match("^{([^}?]+)(%??)}$")
            if name then
                segs[#segs + 1] = { t = q == "?" and "opt" or "param", name = name }
            else
                if seg:match("^{%??}$") then
                    return nil, "empty param name in segment: " .. seg
                end
                segs[#segs + 1] = { t = "lit", val = seg }
            end
        end
    end
    if #segs > _URL_MAX_PATH_DEPTH then
        return nil, "path segments exceed " .. _URL_MAX_PATH_DEPTH
    end
    for i = 1, #segs - 1 do
        if "wild" == segs[i].t then
            return nil, "wildcard '*' must be the last segment"
        end
        if "opt" == segs[i].t then
            return nil, "optional param '{x?}' must be the last segment"
        end
    end
    return segs
end

-- 将路由段与请求段匹配，填充 params；返回 true/false
local function _match(rsegs, qsegs, params)
    local ri = 1
    local qi = 1
    local rn = #rsegs
    local qn = #qsegs
    while ri <= rn do
        local seg = rsegs[ri]
        if seg.t == "wild" then
            return true
        elseif seg.t == "lit" then
            if qi > qn or seg.val ~= qsegs[qi] then
                return false
            end
            ri = ri + 1
            qi = qi + 1
        elseif seg.t == "param" then
            if qi > qn then
                return false
            end
            params[seg.name] = qsegs[qi]
            ri = ri + 1
            qi = qi + 1
        elseif seg.t == "opt" then
            if qi <= qn then
                params[seg.name] = qsegs[qi]
                qi = qi + 1
            end
            ri = ri + 1
        end
    end
    return qi > qn
end

---@class Ctx
---@field fd      integer            socket fd
---@field skid    integer            连接 skid
---@field client  string?            客户端地址（由 dispatch 传入）
---@field method  string             HTTP 方法，如 "GET"、"POST"
---@field version string?            HTTP 版本，如 "HTTP/1.1"
---@field path    string             请求路径，如 "/user/42"
---@field params  table<string,string>  路由路径参数，dispatch 匹配后填充
---@field query   table<string,string>  URL 查询参数
---@field body    string?            请求体，无则为 nil
---@field headers table<string,string>  请求头
---@field responded boolean            是否已发送响应;响应方法置位,dispatch 据此避免重复 500
---@field text    fun(self:Ctx, code:integer, body:string?)        纯文本响应
---@field json    fun(self:Ctx, code:integer, tbl:table)           JSON 响应，自动附加 Content-Type
---@field html    fun(self:Ctx, code:integer, body:string?)        HTML 响应，自动附加 Content-Type
---@field respond fun(self:Ctx, code:integer, headers:table?, body:string?)  自定义响应

-- 构造请求上下文，附加响应辅助方法
local function _make_ctx(fd, skid, pack, client, method, parsed, version)
    local ctx = {
        fd      = fd,
        skid    = skid,
        client  = client,
        method  = method,
        version = version,
        path    = parsed.path or "/",
        params  = {},   -- 由 dispatch 在匹配后填充
        query   = parsed.param or {},
        body    = http.datastr(pack),
        headers = http.heads(pack) or {},
        responded = false,
    }
    function ctx:text(code, body)
        http.response(self.fd, self.skid, code, nil, tostring(body or ""))
        self.responded = true
    end
    function ctx:json(code, tbl)
        http.response(self.fd, self.skid, code,
            { ["Content-Type"] = "application/json" }, tbl)
        self.responded = true
    end
    function ctx:html(code, body)
        http.response(self.fd, self.skid, code,
            { ["Content-Type"] = "text/html; charset=utf-8" }, tostring(body or ""))
        self.responded = true
    end
    function ctx:respond(code, headers, body)
        http.response(self.fd, self.skid, code, headers, body)
        self.responded = true
    end
    return ctx
end

-- 执行中间件链，异常向上抛出由 dispatch 统一捕获。
-- 每次调用为当前层（chain[i]）现场构造匿名闭包作为 next 参数传入；
-- 中间件调用 next() 即递归推进到 i+1，不调则链路在此截断。
local function _run_chain(chain, ctx, i)
    if i > #chain then
        return
    end
    chain[i](ctx, function()
        _run_chain(chain, ctx, i + 1)
    end)--function -> next
end

-- ── Router ────────────────────────────────────────────────────────────────

---@class Router
local Router = {}
Router.__index = Router

---创建独立路由器实例
---@return Router
function Router.new()
    return setmetatable({
        _routes    = {},    -- 所有已注册路由条目（entry）列表，dispatch 线性扫描
        _named     = {},    -- 命名路由索引：name → entry，由 :name("key") 写入
        _global_mw = {},    -- 全局中间件列表，对所有路由生效
        _mw_reg    = {},    -- 具名中间件注册表：name → fun，由 :define() 写入
        _stack     = {},    -- 分组上下文栈，group() 进入时压栈、退出时弹栈
    }, Router)
end

---注册具名中间件，后续可通过名称字符串在 use / middleware / 路由 mws 中引用
---@param name string 中间件名称
---@param fn fun(ctx:Ctx, next:fun()) 中间件函数
function Router:define(name, fn)
    self._mw_reg[name] = fn
end

-- 解析中间件：字符串 → 注册表查找，函数 → 直接透传
function Router:_resolve(mw)
    if type(mw) == "string" then
        local fn = self._mw_reg[mw]
        assert(fn, "middleware not defined: " .. mw)
        return fn
    end
    return mw
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
---@field method  string                          HTTP 方法（"GET"/"POST"/... 或 "ANY"）
---@field raw     string                          注册时的完整路径（含 prefix），调试用
---@field handler fun(ctx:Ctx)                    路由处理函数
---@field mws     fun(ctx:Ctx,next:fun())[]        路由级中间件列表（分组中间件已静态合并）
---@field _name   string?                         命名路由键，由 :name("key") 写入
---@field name    fun(self:RouteEntry,n:string):RouteEntry  链式命名方法

-- 内部路由注册；返回 entry，支持 :name() 链式调用
function Router:_add(method, path, handler, extra_mws)
    local prefix, ctx_mws = self:_ctx()
    local full = prefix .. path
    local segs, err = _parse(full)
    if not segs then
        WARN("router: path '%s' rejected: %s", full, err)
        return _bad_entry
    end
    local mws  = {}
    for _, mw in ipairs(ctx_mws) do
        mws[#mws + 1] = mw
    end
    if extra_mws then
        for _, mw in ipairs(extra_mws) do
            mws[#mws + 1] = self:_resolve(mw)
        end
    end
    local router = self
    local entry  = {
        method  = method,           -- HTTP 方法（"GET"/"POST"/... 或 "ANY"）
        segs    = segs,             -- 已校验的路由路径段数组，供 _match 使用
        raw     = full,             -- 原始完整路径字符串（含前缀），调试用
        handler = handler,          -- 路由处理函数 fun(ctx)
        mws     = mws,              -- 路由级中间件列表（分组中间件已静态合并进来）
        _name   = nil,              -- 命名路由键，由 :name("key") 写入
    }
    -- :name("key") 链式命名路由
    entry.name = function(_, n)
        entry._name = n
        router._named[n] = entry
        return entry
    end
    self._routes[#self._routes + 1] = entry
    return entry
end

---注册全局中间件，对所有路由生效
---@param mw string|fun(ctx:Ctx, next:fun()) 中间件名称或函数
function Router:use(mw)
    self._global_mw[#self._global_mw + 1] = self:_resolve(mw)
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
    for i = 1, #args do
        mws[#mws + 1] = self:_resolve(args[i])
    end
    return _gb_new(self, "", mws)
end

---分发 HTTP 请求：解析方法和路径，匹配路由后执行中间件链；URL 解析失败响应 400,无匹配响应 404
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param pack lightuserdata http_pack_ctx 指针
---@param client integer? 客户端地址标识，供中间件使用
function Router:dispatch(fd, skid, pack, client)
    -- status 为 nil 表示非 HTTP 包（如连接/断开事件），直接忽略
    local status = http.status(pack)
    if not status then
        return
    end
    local method   = status[1]
    if not _KNOWN_METHODS[method] then
        http.response(fd, skid, 405, _PLAIN_HEADERS, "Method Not Allowed\n")
        return
    end
    local parsed   = url_mod.parse(status[2] or "")
    if not parsed then
        http.response(fd, skid, 400, _PLAIN_HEADERS, "Bad Request\n")
        return
    end
    -- C 侧 segs 已解码、%2F 在段内不当分隔符;RFC 保留空段,这里按路由语义剔除空段
    local req_segs = {}
    for _, s in ipairs(parsed.segs or {}) do
        if "" ~= s then
            req_segs[#req_segs + 1] = s
        end
    end
    -- 线性扫描路由表，首个方法匹配且路径匹配的条目即为结果
    local route, params
    for _, r in ipairs(self._routes) do
        if r.method == "ANY" or r.method == method then
            local p = {}
            if _match(r.segs, req_segs, p) then
                route  = r
                params = p
                break
            end
        end
    end
    if not route then
        http.response(fd, skid, 404, _PLAIN_HEADERS, "Not Found\n")
        return
    end
    local ctx = _make_ctx(fd, skid, pack, client, method, parsed, status[3])
    ctx.params = params
    -- 拼接执行链：全局中间件 → 路由级中间件 → handler
    local chain = {}
    for _, mw in ipairs(self._global_mw) do
        chain[#chain + 1] = mw
    end
    for _, mw in ipairs(route.mws) do
        chain[#chain + 1] = mw
    end
    --路由处理函数
    chain[#chain + 1] = route.handler
    -- 链内任意位置抛出异常均由 srey.xpcall 兜底（自动 ERROR + traceback），避免 handler/中间件崩溃丢失响应
    local ok, err = srey.xpcall(_run_chain, chain, ctx, 1)
    -- 仅未响应时补 500
    if not ctx.responded then
        local errmsg = ok and "Internal Server Error\n"
            or string.format("Internal Server Error. %s\n", tostring(err))
        pcall(http.response, fd, skid, 500, _PLAIN_HEADERS, errmsg)
    end
end

-- ── 默认实例（Laravel Route facade 风格）────────────────────────────────

local Route    = Router.new()
Route.new      = Router.new   -- 暴露构造函数，支持 require("advance.router").new()
return Route
