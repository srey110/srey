-- advance/router.lua 单元测试。
-- lib.http 在加载路由器前注入 mock，让所有断言在纯 Lua 中同步执行，无需网络。

local srey   = require("lib.srey")
local runner = require("test.runner")

-- ── mock lib.http（必须在 require advance.router 之前） ────────────────────
local last_resp

local mock_http = {
    status    = function(pack) return pack._status end,
    datastr   = function(pack) return pack._body end,
    heads     = function(pack) return pack._headers or {} end,
    response  = function(fd, skid, code, headers, body)
        last_resp = { fd = fd, skid = skid, code = code,
                      headers = headers, body = body }
    end,
}
package.loaded["lib.http"] = mock_http

local Route = require("advance.router")

-- 构造 mock pack；_status[1] 是 HTTP 方法，_status[2] 是完整 URI（含查询字符串）
local function make_pack(method, path, body, headers, version)
    return {
        _status  = { method, path or "/", version },
        _body    = body,
        _headers = headers or {},
    }
end

-- 分发一次请求并返回捕获到的响应（单次响应场景）
local function dispatch(router, method, path, body, headers, version)
    last_resp = nil
    router:dispatch(1, 1, make_pack(method, path, body, headers, version), "127.0.0.1")
    return last_resp
end

srey.startup(function()
runner.run("unit_router", function(t)

    -- ── 1. 基础路由匹配 ─────────────────────────────────────────────────────

    -- 1.1 字面量路径匹配
    do
        local r = Route.new()
        r:get("/users", function(ctx) ctx:text(200, "ok") end)
        t:eq(200, (dispatch(r, "GET", "/users") or {}).code,  "literal match /users")
        t:eq(404, (dispatch(r, "GET", "/user")  or {}).code,  "partial path /user → 404")
    end

    -- 1.2 方法不匹配 → 404
    do
        local r = Route.new()
        r:get("/x", function(ctx) ctx:text(200, "ok") end)
        t:eq(404, (dispatch(r, "POST", "/x") or {}).code, "method mismatch → 404")
    end

    -- 1.3 单个路径参数
    do
        local r = Route.new()
        local got_id
        r:get("/user/{id}", function(ctx)
            got_id = ctx.params.id
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/user/42")
        t:eq("42", got_id, "single param id=42")
    end

    -- 1.3b ctx 元数据：version 注入 + pack 不再暴露裸指针(yield 后悬空 footgun 已消除)
    do
        local r = Route.new()
        local got = {}
        r:get("/meta", function(ctx)
            got.version  = ctx.version
            got.has_pack = (ctx.pack ~= nil)
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/meta", nil, nil, "HTTP/1.1")
        t:eq("HTTP/1.1", got.version,  "ctx.version 来自状态行第三段")
        t:eq(false,      got.has_pack, "ctx.pack 已移除,不暴露 yield 后悬空裸指针")
    end

    -- 1.4 多个路径参数
    do
        local r = Route.new()
        local got = {}
        r:get("/users/{uid}/posts/{pid}", function(ctx)
            got.uid = ctx.params.uid
            got.pid = ctx.params.pid
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/users/7/posts/99")
        t:eq("7",  got.uid, "multi-param uid=7")
        t:eq("99", got.pid, "multi-param pid=99")
    end

    -- 1.5 可选参数：存在时填充
    do
        local r = Route.new()
        local got_name
        r:get("/files/{name?}", function(ctx)
            got_name = ctx.params.name
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/files/readme.txt")
        t:eq("readme.txt", got_name, "optional param present")
    end

    -- 1.6 可选参数：缺失时为 nil
    do
        local r = Route.new()
        local got_name = "SENTINEL"
        r:get("/files/{name?}", function(ctx)
            got_name = ctx.params.name
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/files")
        t:eq(nil, got_name, "optional param absent → nil")
    end

    -- 1.7 通配符 *
    do
        local r = Route.new()
        r:get("/static/*", function(ctx) ctx:text(200, "static") end)
        t:eq(200, (dispatch(r, "GET", "/static/js/app.js") or {}).code, "wildcard match deep path")
        t:eq(200, (dispatch(r, "GET", "/static/x")         or {}).code, "wildcard match single segment")
    end

    -- 1.8 ANY 匹配所有方法
    do
        local r = Route.new()
        r:any("/ping", function(ctx) ctx:text(200, "pong") end)
        for _, m in ipairs({"GET", "POST", "PUT", "DELETE", "PATCH"}) do
            t:eq(200, (dispatch(r, m, "/ping") or {}).code, "ANY matches " .. m)
        end
    end

    -- 1.9 无匹配路由 → 404
    do
        local r = Route.new()
        r:get("/a", function(ctx) ctx:text(200, "ok") end)
        t:eq(404, (dispatch(r, "GET", "/b") or {}).code, "no match → 404")
    end

    -- 1.10 nil status（非 HTTP pack）→ 不触发任何响应
    do
        local r = Route.new()
        r:get("/a", function(ctx) ctx:text(200, "ok") end)
        last_resp = nil
        r:dispatch(1, 1, { _status = nil, _path = "/a" }, nil)
        t:eq(nil, last_resp, "nil status → no response")
    end

    -- 1.11 五种注册方法各自路由独立
    do
        local r = Route.new()
        r:get(    "/m", function(ctx) ctx:text(200, "GET")    end)
        r:post(   "/m", function(ctx) ctx:text(200, "POST")   end)
        r:put(    "/m", function(ctx) ctx:text(200, "PUT")    end)
        r:delete( "/m", function(ctx) ctx:text(200, "DELETE") end)
        r:patch(  "/m", function(ctx) ctx:text(200, "PATCH")  end)
        for _, m in ipairs({"GET", "POST", "PUT", "DELETE", "PATCH"}) do
            local resp = dispatch(r, m, "/m")
            t:eq(200, resp and resp.code, m .. " route code 200")
            t:eq(m,   resp and resp.body, m .. " route body matches")
        end
    end

    -- 1.12 根路径 /
    do
        local r = Route.new()
        r:get("/", function(ctx) ctx:text(200, "root") end)
        t:eq(200,    (dispatch(r, "GET", "/") or {}).code, "root / matches")
        t:eq("root", (dispatch(r, "GET", "/") or {}).body, "root / body")
    end

    -- 1.13 %2F 不当分隔符(A3):段内 %2F 解成字面 '/', 不重新分段
    do
        local r = Route.new()
        local got
        r:get("/files/{name}", function(ctx) got = ctx.params.name; ctx:text(200, "ok") end)
        t:eq(200,   (dispatch(r, "GET", "/files/a%2Fb") or {}).code, "%2F 命中单段路由")
        t:eq("a/b", got, "%2F 解码为字面 'a/b', 未被切成两段")
    end

    -- 1.14 path 里 '+' 是字面量(A2):不转空格
    do
        local r = Route.new()
        r:get("/lit/a+b", function(ctx) ctx:text(200, "litok") end)
        t:eq("litok", (dispatch(r, "GET", "/lit/a+b") or {}).body, "'+' 保持字面, 命中字面路由")
    end

    -- ── 2. ctx 字段 ────────────────────────────────────────────────────────

    do
        local r = Route.new()
        local got = {}
        r:post("/items/{id}", function(ctx)
            got.method  = ctx.method
            got.path    = ctx.path
            got.id      = ctx.params.id
            got.body    = ctx.body
            got.client  = ctx.client
            got.fd      = ctx.fd
            got.skid    = ctx.skid
            ctx:text(200, "ok")
        end)
        last_resp = nil
        local pack = make_pack("POST", "/items/5", "hello",
                               { ["content-type"] = "text/plain" })
        r:dispatch(10, 20, pack, "10.0.0.1")
        t:eq("POST",      got.method, "ctx.method")
        t:eq("/items/5",  got.path,   "ctx.path")
        t:eq("5",         got.id,     "ctx.params.id")
        t:eq("hello",     got.body,   "ctx.body")
        t:eq("10.0.0.1",  got.client, "ctx.client")
        t:eq(10,          got.fd,     "ctx.fd")
        t:eq(20,          got.skid,   "ctx.skid")
    end

    -- ctx.query 传递
    do
        local r = Route.new()
        local got_q
        r:get("/q", function(ctx)
            got_q = ctx.query
            ctx:text(200, "ok")
        end)
        local pack = make_pack("GET", "/q?page=3&sort=asc")
        r:dispatch(1, 1, pack, nil)
        t:eq("3",   got_q and got_q.page, "ctx.query.page")
        t:eq("asc", got_q and got_q.sort, "ctx.query.sort")
    end

    -- ── 3. 响应辅助方法 ─────────────────────────────────────────────────────

    -- ctx:text
    do
        local r = Route.new()
        r:get("/t", function(ctx) ctx:text(201, "created") end)
        local resp = dispatch(r, "GET", "/t")
        t:eq(201,       resp and resp.code, "ctx:text code 201")
        t:eq("created", resp and resp.body, "ctx:text body")
        t:eq(nil,       resp and resp.headers, "ctx:text no Content-Type header")
    end

    -- ctx:json
    do
        local r = Route.new()
        r:get("/j", function(ctx) ctx:json(200, { ok = true }) end)
        local resp = dispatch(r, "GET", "/j")
        t:eq(200, resp and resp.code, "ctx:json code 200")
        t:check(resp ~= nil and resp.headers ~= nil and
                resp.headers["Content-Type"] == "application/json",
                "ctx:json Content-Type: application/json")
    end

    -- ctx:html
    do
        local r = Route.new()
        r:get("/h", function(ctx) ctx:html(200, "<h1>hi</h1>") end)
        local resp = dispatch(r, "GET", "/h")
        t:eq(200, resp and resp.code, "ctx:html code 200")
        t:check(resp ~= nil and resp.headers ~= nil and
                resp.headers["Content-Type"] == "text/html; charset=utf-8",
                "ctx:html Content-Type: text/html")
    end

    -- ctx:respond
    do
        local r = Route.new()
        r:get("/r", function(ctx)
            ctx:respond(202, { ["X-Custom"] = "yes" }, "accepted")
        end)
        local resp = dispatch(r, "GET", "/r")
        t:eq(202,        resp and resp.code, "ctx:respond code 202")
        t:eq("accepted", resp and resp.body, "ctx:respond body")
        t:check(resp ~= nil and resp.headers ~= nil and
                resp.headers["X-Custom"] == "yes",
                "ctx:respond custom header X-Custom")
    end

    -- ── 4. 中间件 ───────────────────────────────────────────────────────────

    -- 4.1 单个全局中间件在 handler 前执行
    do
        local order = {}
        local r = Route.new()
        r:use(function(ctx, next) order[#order + 1] = "mw"; next() end)
        r:get("/x", function(ctx) order[#order + 1] = "h"; ctx:text(200, "ok") end)
        dispatch(r, "GET", "/x")
        t:eq(2,    #order,   "global mw: 2 steps total")
        t:eq("mw", order[1], "global mw runs first")
        t:eq("h",  order[2], "handler runs second")
    end

    -- 4.2 多个全局中间件按注册顺序执行
    do
        local order = {}
        local r = Route.new()
        r:use(function(ctx, next) order[#order + 1] = "g1"; next() end)
        r:use(function(ctx, next) order[#order + 1] = "g2"; next() end)
        r:get("/x", function(ctx) order[#order + 1] = "h"; ctx:text(200, "ok") end)
        dispatch(r, "GET", "/x")
        t:eq("g1", order[1], "multi-global order: g1 first")
        t:eq("g2", order[2], "multi-global order: g2 second")
        t:eq("h",  order[3], "multi-global order: handler last")
    end

    -- 4.3 路由级中间件
    do
        local order = {}
        local r = Route.new()
        r:get("/x", function(ctx)
            order[#order + 1] = "h"; ctx:text(200, "ok")
        end, { function(ctx, next) order[#order + 1] = "rm"; next() end })
        dispatch(r, "GET", "/x")
        t:eq("rm", order[1], "route mw before handler")
        t:eq("h",  order[2], "handler after route mw")
    end

    -- 4.4 全局 + 路由级顺序：g1 → g2 → r1 → r2 → handler
    do
        local order = {}
        local r = Route.new()
        r:use(function(ctx, next) order[#order + 1] = "g1"; next() end)
        r:use(function(ctx, next) order[#order + 1] = "g2"; next() end)
        r:get("/x", function(ctx)
            order[#order + 1] = "h"; ctx:text(200, "ok")
        end, {
            function(ctx, next) order[#order + 1] = "r1"; next() end,
            function(ctx, next) order[#order + 1] = "r2"; next() end,
        })
        dispatch(r, "GET", "/x")
        t:eq("g1", order[1], "full chain order: g1")
        t:eq("g2", order[2], "full chain order: g2")
        t:eq("r1", order[3], "full chain order: r1")
        t:eq("r2", order[4], "full chain order: r2")
        t:eq("h",  order[5], "full chain order: handler")
    end

    -- 4.5 中间件不调 next → 截断链路，handler 不执行
    do
        local handler_called = false
        local r = Route.new()
        r:use(function(ctx, next)
            ctx:text(401, "blocked")
            -- 故意不调 next()
        end)
        r:get("/x", function(ctx)
            handler_called = true
            ctx:text(200, "ok")
        end)
        local resp = dispatch(r, "GET", "/x")
        t:eq(401,   resp and resp.code, "short-circuit: 401 returned")
        t:eq(false, handler_called,     "short-circuit: handler not called")
    end

    -- 4.6 next() 返回后可执行后置逻辑
    do
        local log = {}
        local r = Route.new()
        r:use(function(ctx, next)
            log[#log + 1] = "before"
            next()
            log[#log + 1] = "after"
        end)
        r:get("/x", function(ctx)
            log[#log + 1] = "handler"
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/x")
        t:eq("before",  log[1], "post-next: before")
        t:eq("handler", log[2], "post-next: handler")
        t:eq("after",   log[3], "post-next: after")
    end

    -- 4.7 中间件可向 ctx 附加字段供 handler 使用
    do
        local r = Route.new()
        r:use(function(ctx, next) ctx.user = "alice"; next() end)
        local got_user
        r:get("/x", function(ctx)
            got_user = ctx.user
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/x")
        t:eq("alice", got_user, "middleware can attach ctx fields")
    end

    -- 4.8 全局中间件只对已注册路由生效，未注册路径仍 404
    do
        local mw_ran = false
        local r = Route.new()
        r:use(function(ctx, next) mw_ran = true; next() end)
        r:get("/exists", function(ctx) ctx:text(200, "ok") end)
        mw_ran = false
        dispatch(r, "GET", "/noexist")
        t:eq(false, mw_ran, "global mw not called when route not found")
    end

    -- 4.9 具名中间件：define + use 按名称引用
    do
        local order = {}
        local r = Route.new()
        r:define("log", function(ctx, next) order[#order + 1] = "log"; next() end)
        r:use("log")
        r:get("/x", function(ctx) order[#order + 1] = "h"; ctx:text(200, "ok") end)
        dispatch(r, "GET", "/x")
        t:eq("log", order[1], "named mw via use: runs first")
        t:eq("h",   order[2], "named mw via use: handler after")
    end

    -- 4.10 具名中间件：路由级 mws 按名称引用
    do
        local order = {}
        local r = Route.new()
        r:define("auth", function(ctx, next) order[#order + 1] = "auth"; next() end)
        r:get("/x", function(ctx)
            order[#order + 1] = "h"; ctx:text(200, "ok")
        end, { "auth" })
        dispatch(r, "GET", "/x")
        t:eq("auth", order[1], "named route mw: runs first")
        t:eq("h",    order[2], "named route mw: handler after")
    end

    -- 4.11 未定义的具名中间件 → assert 错误
    do
        local r = Route.new()
        local ok, err = pcall(function() r:use("nonexistent") end)
        t:eq(false, ok, "unknown mw name → error raised")
        t:check(err ~= nil and err:find("nonexistent") ~= nil,
                "error message mentions unknown name")
    end

    -- ── 5. GroupBuilder（prefix / middleware / group） ──────────────────────

    -- 5.1 prefix：组内路由加前缀，组外不受影响
    do
        local r = Route.new()
        r:prefix("/api"):group(function()
            r:get("/users", function(ctx) ctx:text(200, "ok") end)
        end)
        r:get("/users", function(ctx) ctx:text(200, "bare") end)
        t:eq(200,    (dispatch(r, "GET", "/api/users") or {}).code, "prefix: /api/users → 200")
        t:eq("bare", (dispatch(r, "GET", "/users")     or {}).body, "bare /users unaffected by prefix")
    end

    -- 5.2 middleware group：组内路由携带中间件
    do
        local order = {}
        local r = Route.new()
        r:middleware(function(ctx, next)
            order[#order + 1] = "gm"; next()
        end):group(function()
            r:get("/x", function(ctx) order[#order + 1] = "h"; ctx:text(200, "ok") end)
        end)
        dispatch(r, "GET", "/x")
        t:eq("gm", order[1], "mw group: mw runs first")
        t:eq("h",  order[2], "mw group: handler after")
    end

    -- 5.3 prefix + middleware 组合
    do
        local mw_ran = false
        local r = Route.new()
        r:prefix("/v1"):middleware(function(ctx, next)
            mw_ran = true; next()
        end):group(function()
            r:get("/ping", function(ctx) ctx:text(200, "pong") end)
        end)
        local resp = dispatch(r, "GET", "/v1/ping")
        t:eq(200,  resp and resp.code, "prefix+mw group: /v1/ping → 200")
        t:eq(true, mw_ran,            "prefix+mw group: mw ran")
    end

    -- 5.4 嵌套 prefix
    do
        local r = Route.new()
        r:prefix("/api"):group(function()
            r:prefix("/v2"):group(function()
                r:get("/info", function(ctx) ctx:text(200, "info") end)
            end)
        end)
        t:eq(200, (dispatch(r, "GET", "/api/v2/info") or {}).code, "nested prefix: /api/v2/info → 200")
        t:eq(404, (dispatch(r, "GET", "/api/info")    or {}).code, "nested prefix: /api/info → 404")
        t:eq(404, (dispatch(r, "GET", "/v2/info")     or {}).code, "nested prefix: /v2/info → 404")
    end

    -- 5.5 GroupBuilder:prefix 链式追加
    do
        local r = Route.new()
        r:prefix("/a"):prefix("/b"):group(function()
            r:get("/c", function(ctx) ctx:text(200, "ok") end)
        end)
        t:eq(200, (dispatch(r, "GET", "/a/b/c") or {}).code, "prefix chain /a/b/c → 200")
        t:eq(404, (dispatch(r, "GET", "/a/c")   or {}).code, "prefix chain /a/c → 404")
    end

    -- 5.6 组内中间件不泄漏到组外路由
    do
        local mw_ran = false
        local r = Route.new()
        r:middleware(function(ctx, next)
            mw_ran = true; next()
        end):group(function()
            r:get("/inside", function(ctx) ctx:text(200, "ok") end)
        end)
        r:get("/outside", function(ctx) ctx:text(200, "ok") end)
        mw_ran = false
        dispatch(r, "GET", "/outside")
        t:eq(false, mw_ran, "group mw does not leak to /outside")
        mw_ran = false
        dispatch(r, "GET", "/inside")
        t:eq(true, mw_ran, "group mw runs for /inside")
    end

    -- 5.7 Router:middleware() 接受多个中间件，按顺序执行
    do
        local order = {}
        local r = Route.new()
        r:middleware(
            function(ctx, next) order[#order + 1] = "m1"; next() end,
            function(ctx, next) order[#order + 1] = "m2"; next() end
        ):group(function()
            r:get("/x", function(ctx) order[#order + 1] = "h"; ctx:text(200, "ok") end)
        end)
        dispatch(r, "GET", "/x")
        t:eq("m1", order[1], "Router:middleware multi: m1")
        t:eq("m2", order[2], "Router:middleware multi: m2")
        t:eq("h",  order[3], "Router:middleware multi: handler")
    end

    -- 5.8 具名中间件在分组中按名称引用
    do
        local ran = false
        local r = Route.new()
        r:define("check", function(ctx, next) ran = true; next() end)
        r:prefix("/g"):middleware("check"):group(function()
            r:get("/y", function(ctx) ctx:text(200, "ok") end)
        end)
        dispatch(r, "GET", "/g/y")
        t:eq(true, ran, "named mw in group by name")
    end

    -- ── 6. 命名路由 ─────────────────────────────────────────────────────────

    do
        local r = Route.new()
        local entry = r:get("/user/{id}", function(ctx) ctx:text(200, "ok") end)
                       :name("user.show")
        t:eq("user.show", entry._name,              "named route: entry._name set")
        t:check(r._named["user.show"] == entry,     "named route: stored in _named table")
        -- 路由仍可正常匹配
        t:eq(200, (dispatch(r, "GET", "/user/1") or {}).code, "named route still dispatches")
    end

    -- ── 7. 错误处理 ─────────────────────────────────────────────────────────

    -- 7.1 handler 抛出异常 → 500
    do
        local r = Route.new()
        r:get("/boom", function(ctx) error("kaboom") end)
        t:eq(500, (dispatch(r, "GET", "/boom") or {}).code, "handler error → 500")
    end

    -- 7.2 中间件抛出异常 → 500
    do
        local r = Route.new()
        r:use(function(ctx, next) error("mw crash") end)
        r:get("/x", function(ctx) ctx:text(200, "ok") end)
        t:eq(500, (dispatch(r, "GET", "/x") or {}).code, "middleware error → 500")
    end

    -- 7.3 全局 + 路由中间件都抛出，第一个被 pcall 捕获即回 500
    do
        local r = Route.new()
        r:use(function(ctx, next) error("global crash") end)
        r:get("/x", function(ctx) ctx:text(200, "ok") end,
              { function(ctx, next) error("route crash") end })
        t:eq(500, (dispatch(r, "GET", "/x") or {}).code, "first mw error → 500 (no double response)")
    end

    -- 7.4 handler 已成功响应后再抛错 → 不补第二个 500（responded 机制，对齐 C 端 router_dispatch 兜底逻辑）
    do
        local resp_count = 0
        local orig_resp = mock_http.response
        mock_http.response = function(fd, skid, code, headers, body)
            resp_count = resp_count + 1
            orig_resp(fd, skid, code, headers, body)
        end
        local r = Route.new()
        r:get("/late", function(ctx)
            ctx:text(200, "done")      -- 先成功响应
            error("after response")    -- 再抛错
        end)
        local resp = dispatch(r, "GET", "/late")
        mock_http.response = orig_resp  -- 还原 mock
        t:eq(1,   resp_count,           "已响应后抛错:只发 1 次响应,不补 500")
        t:eq(200, resp and resp.code,   "保留 handler 的 200,不被 500 覆盖")
    end

    -- 8.1(本次:对齐 C 端 router_add 软失败):非法模板被跳过(返 nil)不注册、不中断 startup
    do
        local r = Route.new()
        t:eq(nil, r:get("/a/*/b", function() end), "中置 * 被跳过(返 nil)")
        t:eq(nil, r:get("/x/{}",  function() end), "空名 {} 被跳过")
        t:eq(nil, r:get("/y/{?}", function() end), "空名 {?} 被跳过")
        t:eq(nil, r:get("/" .. string.rep("s/", 65), function() end), "段数超 64 被跳过")
        t:check(nil ~= r:get("/ok/{id}/*", function() end), "末段 * 合法,返 entry")
        r:get("/legit", function(ctx) ctx:text(200, "ok") end)
        t:eq(200, (dispatch(r, "GET", "/legit") or {}).code, "非法跳过后合法路由仍正常")
    end

    -- 8.2(本次:对齐 C 端 405):未知/小写 method → 405,响应带 Content-Type
    do
        local r = Route.new()
        r:any("/ping", function(ctx) ctx:text(200, "pong") end)
        local resp = dispatch(r, "BREW", "/ping")
        t:eq(405, resp and resp.code, "未知 method → 405")
        t:eq("text/plain; charset=utf-8", resp and resp.headers and resp.headers["Content-Type"], "405 带 Content-Type")
        t:eq(405, (dispatch(r, "get", "/ping") or {}).code, "小写 method → 405")
        t:eq(200, (dispatch(r, "GET", "/ping") or {}).code, "已知 method → 200")
    end

    -- 8.3(本次:对齐 C 端):解析失败(400)/无匹配(404) 响应带 Content-Type
    do
        local r = Route.new()
        r:get("/exists", function(ctx) ctx:text(200, "ok") end)
        local r404 = dispatch(r, "GET", "/nope")
        t:eq(404, r404 and r404.code, "无匹配 → 404")
        t:eq("text/plain; charset=utf-8", r404 and r404.headers and r404.headers["Content-Type"], "404 带 Content-Type")
        local r400 = dispatch(r, "GET", "/" .. string.rep("a", 1100))
        t:eq(400, r400 and r400.code, "URI 超长解析失败 → 400")
        t:eq("text/plain; charset=utf-8", r400 and r400.headers and r400.headers["Content-Type"], "400 带 Content-Type")
    end

end)
end)
