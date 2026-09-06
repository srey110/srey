-- advance/router.lua 单元测试。
-- lib.http 在加载路由器前注入 mock，让所有断言在纯 Lua 中同步执行，无需网络。

local srey   = require("lib.srey")
local runner = require("test.runner")

-- ── mock lib.http（必须在 require advance.router 之前） ────────────────────
local last_resp

-- 响应发出次数：兜底 500 只该发一次，"no double response" 这类文案要有计数器才立得住
local nresp = 0

local mock_http = {
    status      = function(pack) return pack._status end,
    datastr     = function(pack) return pack._body end,
    heads       = function(pack) return pack._headers or {} end,
    -- 真实现是 C 的 http_header 走 buf_icompare;这里用小写归一等价复现,
    -- ctx:header 的大小写无关契约靠它兜住
    head        = function(pack, key)
        local hs = pack._headers
        if not hs then
            return nil
        end
        local want = key:lower()
        for k, v in pairs(hs) do
            if k:lower() == want then
                return v
            end
        end
        return nil
    end,
    code_status = require("srey.http").code_status,
    response    = function(fd, skid, code, headers, body)
        nresp = nresp + 1
        last_resp = { fd = fd, skid = skid, code = code,
                      headers = headers, body = body }
    end,
    -- HEAD 出口：真实现只发头不发体，这里同样把 body 记成 nil，
    -- 但留下 headonly 与算出的长度，好让用例断言"头与 GET 一致、体没发"
    response_head = function(fd, skid, code, headers, body)
        last_resp = { fd = fd, skid = skid, code = code, headers = headers,
                      body = nil, headonly = true,
                      clen = ("string" == type(body)) and #body or 0 }
    end,
}
package.loaded["lib.http"] = mock_http

-- srey.close 一并换掉：这里的 fd/skid 是假的，真去关会打到 C 层的事件线程。
-- 每个 unit 模块是独立 task（各有各的 lua_State），改这里波及不到别的模块
local closed_log = {}
srey.close = function(fd, skid)
    closed_log[#closed_log + 1] = { fd = fd, skid = skid }
end

-- watch_closed 同样换掉：它往 task 级观察者表里塞的闭包没有反注册，而用例要建几十个 router。
-- 顺带拿计数断言"没有流式路由就不订阅"
local watch_n = 0
srey.watch_closed = function(_)
    watch_n = watch_n + 1
end

local Route = require("advance.router")
local SLICE_TYPE   = srey.SLICE_TYPE
local STREAM_ABORT = Route.STREAM_ABORT

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
    -- 第 4 个参数是 on_recved 的 client 标志(1=客户端 0=服务端)，不是地址
    router:dispatch(1, 1, make_pack(method, path, body, headers, version), 0)
    return last_resp
end

srey.startup(function()
runner.run(function(t)

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
        -- ROUTER_M_ANY 是 7 位掩码，7 个方法都得跑到；漏掉 HEAD/OPTIONS 的话
        -- 掩码少置一位也发现不了
        for _, m in ipairs({"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"}) do
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
        local got
        r:get("/", function(ctx) got = ctx.path; ctx:text(200, "root") end)
        t:eq(200,    (dispatch(r, "GET", "/") or {}).code, "root / matches")
        t:eq("root", (dispatch(r, "GET", "/") or {}).body, "root / body")
        -- "/" 的段全是空段，C 侧压完 npath 归零，url_reorg_path 只吐得出空串；
        -- ctx.path 仍须是 "/" 全靠绑定层 _lrouter_push_url 的 npath==0 分支兜住
        t:eq("/", got, "root / 的 ctx.path")
        -- "//" 同样压成 0 段，命中同一条路由、走同一个分支
        got = nil
        t:eq(200, (dispatch(r, "GET", "//") or {}).code, "// 也命中根路由")
        t:eq("/", got, "// 的 ctx.path 也是 /")
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

    -- 1.15 {a?b} 参数名含内部 '?' → 当字面量段(对齐 C 端 B2 文法), 不当参数匹配
    do
        local r = Route.new()
        r:get("/litq/{a?b}", function(ctx) ctx:text(200, "litok") end)
        t:eq(404, (dispatch(r, "GET", "/litq/xyz") or {}).code, "{a?b} 当字面量, /litq/xyz 不命中参数 → 404")
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
        local pack = make_pack("POST", "/items/5", "hello",
                               { ["content-type"] = "text/plain" })
        r:dispatch(10, 20, pack, 0)
        t:eq("POST",      got.method, "ctx.method")
        t:eq("/items/5",  got.path,   "ctx.path")
        t:eq("5",         got.id,     "ctx.params.id")
        t:eq("hello",     got.body,   "ctx.body")
        -- client 是连接方向标志(1=客户端 0=服务端)，不是地址；取对端 IP 走 utils.remote_addr(fd)
        t:eq(0,           got.client, "ctx.client")
        t:eq(10,          got.fd,     "ctx.fd")
        t:eq(20,          got.skid,   "ctx.skid")
    end

    -- 空段 URL（/a//b）：匹配按压缩段进行，回填的 ctx.path 不应残留重复/空段
    -- 回归 router.c _router_find 压缩空段后未回写 npath（Lua 绑定按旧 npath 重建 path）
    do
        local r = Route.new()
        local got = {}
        r:get("/a/b", function(ctx)
            got.path = ctx.path
            ctx:text(200, "ok")
        end)
        local pack = make_pack("GET", "/a//b")
        r:dispatch(1, 1, pack, nil)
        t:eq("/a/b", got.path, "ctx.path 去空段不残留 (/a//b)")
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
        t:check(resp and resp.headers and
                "text/plain; charset=utf-8" == resp.headers["Content-Type"],
                "ctx:text 带 Content-Type(不带会被内容嗅探)")
    end

    -- HEAD / OPTIONS：get() 连带注册 HEAD，独立入口也在
    do
        local r = Route.new()
        r:head("/h", function(ctx) ctx:text(200) end)
        r:options("/o", function(ctx) ctx:text(204) end)
        t:eq(200, (dispatch(r, "HEAD", "/h") or {}).code, "Router:head 注册的路由能匹配 HEAD")
        t:eq(204, (dispatch(r, "OPTIONS", "/o") or {}).code, "Router:options 注册的路由能匹配 OPTIONS")
        -- get() 注册的是 GET|HEAD 组合掩码，HEAD 落到同一个 handler
        r:get("/g", function(ctx) ctx:text(200, "hello") end)
        t:eq(200, (dispatch(r, "GET", "/g") or {}).code, "get() 注册的路由匹配 GET")
        t:eq(200, (dispatch(r, "HEAD", "/g") or {}).code, "get() 连带接住 HEAD，不再 404")
        -- get() 之后再注册独立 HEAD 会被遮蔽：GET|HEAD 把 HEAD 整个包住，先注册者胜
        local dup = r:head("/g", function(ctx) ctx:text(201, "other") end)
        t:eq(200, (dispatch(r, "HEAD", "/g") or {}).code, "被 get() 遮蔽的 head() 注册被忽略")
        t:check(nil ~= dup, "被遮蔽的注册返回占位条目而不是 nil")
        -- 命中路由时不发体，但 Content-Length 记的是 GET 那份的真实长度
        local hit = dispatch(r, "HEAD", "/g") or {}
        t:check(hit.headonly and nil == hit.body, "HEAD 命中路由只发头不发体")
        t:eq(#"hello", hit.clen, "HEAD 的 Content-Length 等于 GET 报文体的长度")
        -- 反过来（head 先、get 后，即 Router:head 文档要求的顺序）两条都得进表：
        -- GET|HEAD 只是与 HEAD 有交集、包不住它，按交集判遮蔽会把 GET 那条整条吞掉
        r:head("/x", function(ctx) ctx:text(201, "head-only") end)
        r:get("/x", function(ctx) ctx:text(200, "get-body") end)
        t:eq(201, (dispatch(r, "HEAD", "/x") or {}).code, "head 先注册时 HEAD 走它自己的 handler")
        t:eq(200, (dispatch(r, "GET", "/x") or {}).code, "head 先注册不影响同路径 get() 的注册")
        t:eq("get-body", (dispatch(r, "GET", "/x") or {}).body, "GET 拿到的是 get() 的报文体")
    end

    -- HEAD 的失败路径同样不能带体：多发的字节会被对端当成下一条响应的开头。
    -- C 侧 _router_send_code / _router_send_simple 都带 head_only，Lua 侧三个出口要对齐
    do
        local r = Route.new()
        r:get("/ok", function(ctx) ctx:text(200, "x") end)
        -- 未匹配：走 _match_ctx 的 404
        local miss = dispatch(r, "HEAD", "/nope") or {}
        t:eq(404, miss.code, "HEAD 未匹配仍回 404")
        t:check(miss.headonly and nil == miss.body, "HEAD 的 404 不发报文体")
        -- handler 什么都不写：走 _fallback_500
        r:get("/silent", function() end)
        local fb = dispatch(r, "HEAD", "/silent") or {}
        t:eq(500, fb.code, "handler 漏发响应时 HEAD 也补兜底 500")
        t:check(fb.headonly and nil == fb.body, "HEAD 的兜底 500 不发报文体")
        -- 同一条 handler 走 GET 时报文体照常发出，两边只差报文体
        local g = dispatch(r, "GET", "/nope") or {}
        t:check(not g.headonly and nil ~= g.body, "GET 的 404 照常带报文体")
    end

    -- headers / body 惰性物化：dispatch 内读得到，出了 dispatch 就没了（与 C 侧 router_req 同口径）
    do
        local r = Route.new()
        local saved, inside_h, inside_b
        r:post("/lazy", function(ctx)
            inside_h = ctx.headers and ctx.headers["X-Probe"]
            inside_b = ctx.body
            saved = ctx
            ctx:text(200, "ok")
        end)
        dispatch(r, "POST", "/lazy", "bodybody", { ["X-Probe"] = "v" })
        t:eq("v", inside_h, "dispatch 内读得到请求头")
        t:eq("bodybody", inside_b, "dispatch 内读得到请求体")
        -- 读过即固化在 ctx 上，出了 dispatch 仍在
        t:eq("v", saved.headers["X-Probe"], "读过的 headers 出了 dispatch 仍有效")
        t:eq("bodybody", saved.body, "读过的 body 出了 dispatch 仍有效")

        -- 没读过的：出了 dispatch 拿到的是空表 / nil，不是悬空指针
        local later
        r:post("/lazy2", function(ctx) later = ctx; ctx:text(200, "ok") end)
        dispatch(r, "POST", "/lazy2", "xx", { ["X-Probe"] = "v" })
        t:eq(0, next(later.headers) and 1 or 0, "没读过的 headers 出了 dispatch 是空表")
        t:eq(nil, later.body, "没读过的 body 出了 dispatch 为 nil")
    end

    -- ctx:header 大小写无关，走 C 的 buf_icompare（与 router_req_header 同一条路径）；
    -- 而 ctx.headers 是线格式原样大小写的裸表，大小写不一致就取不到。两者分工得钉住：
    -- 混在一起用会让 router.lua 模块头那段 auth 中间件对大小写规范的客户端一律 401
    do
        local r = Route.new()
        local exact, lower, upper, mixed, miss, tbl_exact, tbl_lower
        r:get("/hdr", function(ctx)
            exact = ctx:header("X-Api-Key")
            lower = ctx:header("x-api-key")
            upper = ctx:header("X-API-KEY")
            mixed = ctx:header("x-ApI-kEy")
            miss  = ctx:header("x-nope")
            tbl_exact = ctx.headers["X-Api-Key"]
            tbl_lower = ctx.headers["x-api-key"]
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/hdr", nil, { ["X-Api-Key"] = "secret" })
        t:eq("secret", exact, "ctx:header 原样大小写取得到")
        t:eq("secret", lower, "ctx:header 全小写取得到")
        t:eq("secret", upper, "ctx:header 全大写取得到")
        t:eq("secret", mixed, "ctx:header 混合大小写取得到")
        t:eq(nil, miss, "ctx:header 不存在的头返回 nil")
        t:eq("secret", tbl_exact, "ctx.headers 按线格式大小写取得到")
        t:eq(nil, tbl_lower, "ctx.headers 大小写不一致取不到")
    end
    -- 出了 dispatch _pack 已摘，ctx:header 恒返 nil（同 C 侧 router_req_header 的 pack 判空）
    do
        local r = Route.new()
        local saved
        r:get("/hdrlate", function(ctx)
            saved = ctx
            ctx:text(200, "ok")
        end)
        dispatch(r, "GET", "/hdrlate", nil, { ["X-Api-Key"] = "secret" })
        t:eq(nil, saved:header("X-Api-Key"), "出了 dispatch 的 ctx:header 返 nil")
    end

    -- 重名占位符：注册期拒收（C 侧 router_req_param 取首个、Lua 侧取末个，同一路由两个答案）
    do
        local r = Route.new()
        r:get("/u/{id}/{id}", function(ctx) ctx:text(200, "dup") end)
        t:eq(404, (dispatch(r, "GET", "/u/7/9") or {}).code, "重名占位符的路由被拒，请求落 404")
        -- 不同名的两个占位符照常注册
        r:get("/v/{a}/{b}", function(ctx) ctx:text(200, ctx.params.a .. ctx.params.b) end)
        t:eq("79", (dispatch(r, "GET", "/v/7/9") or {}).body, "不同名占位符不受影响")
    end

    -- ctx:json
    do
        local r = Route.new()
        r:get("/j", function(ctx) ctx:json(200, { ok = true }) end)
        local resp = dispatch(r, "GET", "/j")
        t:eq(200, resp and resp.code, "ctx:json code 200")
        -- Content-Type 由 http.response 的 table 分支自己写,ctx:json 不再重复传
        -- (传了会在线缆上出现两条同名头);这里断言的是"交给了它去写"——body 原样是 table
        t:eq(nil, resp and resp.headers, "ctx:json 不自带 Content-Type(交给 http 层)")
        t:check(resp ~= nil and "table" == type(resp.body) and true == resp.body.ok,
                "ctx:json body 原样传 table")
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
        -- 连步数一起钉：只比前几项的话，链被多跑一遍(order 变成 6 项)也发现不了
        t:eq("g1,g2,h", table.concat(order, ","), "multi-global 顺序与步数")
    end

    -- 4.3 路由级中间件
    do
        local order = {}
        local r = Route.new()
        r:get("/x", function(ctx)
            order[#order + 1] = "h"; ctx:text(200, "ok")
        end, { function(ctx, next) order[#order + 1] = "rm"; next() end })
        dispatch(r, "GET", "/x")
        t:eq("rm,h", table.concat(order, ","), "route mw 在 handler 之前，且各跑一次")
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

    -- 4.12(回归):路由级具名中间件未定义 → 注册在提交 C 路由前抛错,不留幽灵路由
    -- 修复前 c_router:add 先提交、_resolve 后 assert 抛错 → _routes[idx] 空,该路径 dispatch 索引 nil route 崩溃且无响应
    do
        local r = Route.new()
        local ok = pcall(function()
            r:get("/ghost", function(ctx) ctx:text(200, "ok") end, { "undefined_mw" })
        end)
        t:eq(false, ok, "未定义路由级具名中间件 → 注册抛错")
        local dok, dcode = pcall(function() return (dispatch(r, "GET", "/ghost") or {}).code end)
        t:eq(true, dok,  "注册抛错后 dispatch 不崩溃(无幽灵路由 nil-deref)")
        t:eq(404, dcode, "注册抛错后 /ghost 干净 404")
        -- 注册原子:后补 define 同名中间件也不复活未注册路径,须显式重注册
        r:define("undefined_mw", function(ctx, next) next() end)
        t:eq(404, (dispatch(r, "GET", "/ghost") or {}).code, "后补 define 不复活未注册路径")
    end

    -- 4.12b(回归):ctx:text/html 省略 body 时须原样传 nil 给 http.response,
    -- 物化成 "" 会走它的 string 分支无条件补 Content-Length: 0,
    -- 绕过专为 1xx/204/304 设的 nocl 守卫(那三类禁带 CL,304 补 CL:0 等于谎报资源为空)
    do
        local r = Route.new()
        r:get("/nc", function(ctx) ctx:text(204) end)
        r:get("/nm", function(ctx) ctx:html(304) end)
        r:get("/ok", function(ctx) ctx:text(200, "hi") end)
        t:eq(204, (dispatch(r, "GET", "/nc") or {}).code, "ctx:text(204) 状态码")
        t:eq(nil, last_resp.body, "ctx:text(204) body 传 nil 而不是空串")
        t:eq(304, (dispatch(r, "GET", "/nm") or {}).code, "ctx:html(304) 状态码")
        t:eq(nil, last_resp.body, "ctx:html(304) body 传 nil 而不是空串")
        -- 带 body 的正常路径不受影响
        t:eq("hi", (dispatch(r, "GET", "/ok") or {}).body, "带 body 时照常传字符串")
    end

    -- 4.13(回归):非函数中间件必须当场抛错,不能静默丢弃
    -- 修复前 _resolve 把 nil 原样返回,use() 的 mws[#mws+1] = nil 是一次 no-op,
    -- 一个拼错的变量名就足以让全局鉴权中间件彻底消失,且日志/返回值/启动检查全都不报
    do
        local r = Route.new()
        local ok, err = pcall(function() r:use(nil) end)
        t:eq(false, ok, "use(nil) → 抛错")
        t:check(err ~= nil and err:find("middleware") ~= nil, "错误信息点明是中间件问题")
        t:eq(false, pcall(function() r:use(42) end), "use(非函数) → 抛错")
        t:eq(0, #r._global_mw, "抛错后全局中间件表未被写入")
    end

    -- 4.14(回归):变长中间件参数中间夹 nil,须抛错而不是连后续中间件一起丢掉
    -- #args 遇到中间的 nil 可能只数到它之前,后面的中间件静默消失
    do
        local r = Route.new()
        local mw = function(ctx, next) next() end
        t:eq(false, pcall(function() r:middleware(mw, nil, mw) end),
             "Router:middleware(mw, nil, mw) → 抛错")
        t:eq(false, pcall(function() r:prefix("/g"):middleware(mw, nil, mw) end),
             "GroupBuilder:middleware(mw, nil, mw) → 抛错")
    end

    -- 4.15(回归):路由级 mws 表里夹 nil,须抛错而不是把后面的中间件一起丢掉
    -- ipairs 撞上中间的 nil 就停,_resolve 压根不被调用,一个拼错的变量名足以让鉴权无声消失
    do
        local r = Route.new()
        local mw = function(ctx, next) next() end
        local h = function(ctx) ctx:text(200, "ok") end
        t:eq(false, pcall(function() r:get("/h", h, { mw, nil, mw }) end),
             "get(path, h, {mw, nil, mw}) → 抛错")
        t:eq(false, pcall(function() r:post("/h2", h, { mw, 42 }) end),
             "表里混入非可调用值 → 抛错")
        -- 正常表照旧可用
        local hit = 0
        r:get("/ok2", h, { function(ctx, nxt) hit = hit + 1 nxt() end })
        t:eq(200, (dispatch(r, "GET", "/ok2") or {}).code, "合法 mws 表不受影响")
        t:eq(1, hit, "合法 mws 表里的中间件被执行")
    end

    -- 4.16(回归):具名中间件也要过可调用性校验,不能拖到每个请求才炸成 500;
    -- 带 __call 的表是合法中间件,而设了 __metatable 的对象不得让校验本身崩掉
    do
        local r = Route.new()
        t:eq(false, pcall(function() r:define("bad", {}) end), "define(普通表) → 注册期抛错")
        t:eq(false, pcall(function() r:define("bad2", 42) end), "define(数字) → 注册期抛错")
        local callable = setmetatable({}, { __call = function(_, ctx, nxt) nxt() end })
        r:define("ok", callable)
        r:use("ok")
        r:get("/c", function(ctx) ctx:text(200, "c") end)
        t:eq("c", (dispatch(r, "GET", "/c") or {}).body, "__call 表可作具名中间件")
        -- 元表被 __metatable 藏起来:查不出真相就放行,不能索引到非表值上崩掉
        local hidden = setmetatable({}, { __call = function(_, ctx, nxt) nxt() end,
                                          __metatable = true })
        t:eq(true, pcall(function() r:use(hidden) end), "__metatable 隐藏元表时不崩且放行")
    end

    -- 4.17(回归):同一层调两次 next,须重跑自己的下一位而不是接着游标往后跳。
    -- 共用游标时 pcall(next) 捕获异常后游标已停在出错那层之后,第二次 next 直接落到 handler,
    -- 出错那层(这里就是鉴权)被静默跳过 —— 结果是未鉴权请求拿到 200
    do
        local r = Route.new()
        local seq = ""
        local nxt_ok = {}
        r:use(function(ctx, nxt)
            seq = seq .. "A1,"
            -- pcall 结果要断言：下游 error 若被 next 自己吞掉，seq 与兜底 500 都不变，
            -- 只有这两个返回值能证明异常确实穿了上来
            nxt_ok[1] = pcall(nxt)
            seq = seq .. "A2,"
            nxt_ok[2] = pcall(nxt)
        end)
        r:use(function(ctx, nxt)
            seq = seq .. "B,"
            error("deny")
        end)
        r:get("/dn", function(ctx) seq = seq .. "H," ctx:text(200, "h") end)
        local resp = dispatch(r, "GET", "/dn")
        t:eq("A1,B,A2,B,", seq, "二次 next 重跑被拒的那层")
        t:eq(false, nxt_ok[1], "下游 error 由 next 原样抛给上游(第一次)")
        t:eq(false, nxt_ok[2], "下游 error 由 next 原样抛给上游(第二次)")
        t:check(nil == seq:find("H"), "handler 不因二次 next 被跳到")
        t:eq(500, (resp or {}).code, "中间件全程拒绝 → 兜底 500")
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

    -- 7.1 handler 抛出异常 → 500，正文必须与 C 侧 ROUTER_BODY_500 一字不差：
    -- 异常原文只进日志，混进响应体就把脚本路径与行号发给了远端
    do
        local r = Route.new()
        r:get("/boom", function(ctx) error("kaboom") end)
        local resp = dispatch(r, "GET", "/boom") or {}
        t:eq(500, resp.code, "handler error → 500")
        t:eq("Internal Server Error\n", resp.body, "500 正文不得带异常原文")
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
        nresp = 0
        t:eq(500, (dispatch(r, "GET", "/x") or {}).code, "first mw error → 500")
        t:eq(1, nresp, "两层中间件都抛，兜底 500 只发一次")
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
            ctx:text(200, "done") -- 先成功响应
            error("after response") -- 再抛错
        end)
        local resp = dispatch(r, "GET", "/late")
        mock_http.response = orig_resp -- 还原 mock
        t:eq(1,   resp_count,           "已响应后抛错:只发 1 次响应,不补 500")
        t:eq(200, resp and resp.code,   "保留 handler 的 200,不被 500 覆盖")
    end

    -- 8.1(本次:对齐 C 端 router_add 软失败):非法模板被跳过(返 sentinel)不注册、不中断 startup
    do
        local r = Route.new()
        t:check(nil ~= r:get("/a/*/b", function() end), "中置 * 被跳过(返 sentinel)")
        -- 空名段 {} / {?} 不是占位符, 按字面量段注册成功(同 C 侧 _router_parse_seg)。
        -- 用 :name() 有没有写进 _named 区分真 entry 与 sentinel —— 只查 nil ~= r:get(...)
        -- 的话两者都为真, 分不出注册成功还是被跳过
        r:get("/x/{}",  function() end):name("lit_brace")
        r:get("/y/{?}", function() end):name("lit_bq")
        t:check(nil ~= r._named["lit_brace"], "{} 名字为空 → 字面量段, 注册成功")
        t:check(nil ~= r._named["lit_bq"],    "{?} 名字为空 → 字面量段, 注册成功")
        -- 是字面量而不是参数: 拿任意文本去打都不该命中(当成 OPT 的话 /y/zzz 会返 200)
        t:eq(404, (dispatch(r, "GET", "/x/zzz") or {}).code, "{} 当字面量, /x/zzz 不命中")
        t:eq(404, (dispatch(r, "GET", "/y/zzz") or {}).code, "{?} 当字面量, /y/zzz 不命中")
        -- 判据同上：_bad_entry 与真 entry 都非 nil，只查 nil ~= 分不出被跳过还是注册成功。
        -- 用 :name() 有没有落进 _named 来分
        r:get("/" .. string.rep("s/", 65), function() end):name("seg65")
        t:eq(nil, r._named["seg65"], "段数超 64 被跳过(返 sentinel,名字不入表)")
        r:get("/ok/{id}/*", function() end):name("tail_star")
        t:check(nil ~= r._named["tail_star"], "末段 * 合法,返真 entry(名字入表)")
        -- sentinel 支持链式 :name() 不崩溃，且不污染命名路由表
        r:get("/a/*/b", function() end):name("bad_route")
        t:eq(nil, r._named["bad_route"], "非法路由 :name() 不写入命名表")
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

    -- 8.4 OPT 中置：对齐 C 端 _router_match_path 的可行性 DP（旧的一步前瞻 skip_opt 已删）
    do
        local r = Route.new()
        local got_x
        r:get("/a/{x?}/b", function(ctx)
            got_x = ctx.params.x
            ctx:text(200, "ok")
        end)
        -- OPT 有值时消耗，后续 LIT /b 匹配
        got_x = nil
        t:eq(200,  (dispatch(r, "GET", "/a/42/b") or {}).code, "opt mid+value: 200")
        t:eq("42", got_x, "opt mid+value: param filled")
        -- OPT 无值：唯一可行解是 OPT 不取值，由 LIT /b 吞掉当前请求段
        got_x = "SENTINEL"
        t:eq(200, (dispatch(r, "GET", "/a/b") or {}).code, "opt mid+skip: 200")
        t:eq(nil, got_x, "opt mid+skip: param nil")
        -- 多余段不匹配
        t:eq(404, (dispatch(r, "GET", "/a/b/c") or {}).code, "opt mid: extra seg → 404")
    end

    -- ── 注册期的两类误用必须当场可见 ────────────────────────────────────────
    do
        -- handler 漏传：dispatch 时表现为"中间件跑完没人响应"→ 兜底 500，
        -- 没抛异常所以日志里也没线索，线上完全查不出来。注册期直接拒掉
        local r = Route.new()
        local e = r:get("/noh")
        t:check(e ~= nil, "handler 缺失返回占位 entry 而非报错")
        t:eq(404, (dispatch(r, "GET", "/noh") or {}).code, "handler 缺失的路由未被注册 → 404")
        -- 占位 entry 上继续链式 :name() 也不炸
        t:check(e:name("noh") ~= nil, "占位 entry 的 :name 可链式调用")

        -- 非函数同样拒掉
        r:get("/noh2", "not-a-function")
        t:eq(404, (dispatch(r, "GET", "/noh2") or {}).code, "handler 非函数 → 未注册")
    end
    do
        -- 重复注册：C 侧 add 不去重、match 返回首条命中，后注册的永远够不着。
        -- 两个模块各注册一次、或热更重跑注册块都会撞上，静默丢弃极难查
        local r = Route.new()
        r:get("/dup", function(ctx) ctx:text(200, "first") end)
        r:get("/dup", function(ctx) ctx:text(200, "second") end)
        local resp = dispatch(r, "GET", "/dup") or {}
        t:eq(200, resp.code, "重复注册后原路由仍可用")
        t:eq("first", resp.body, "生效的是先注册的那个（后者已告警并丢弃）")
        -- 方法不同不算重复
        r:post("/dup", function(ctx) ctx:text(201, "post") end)
        t:eq(201, (dispatch(r, "POST", "/dup") or {}).code, "同路径不同方法不算重复注册")
    end
    do
        -- 去重下沉到 C 之后新覆盖的两类：Lua 侧原来用 "方法\0路径" 字符串做 key，这两类看不出来
        -- (a) ANY 的掩码含所有具体方法，先注册的 ANY 会让后注册的 GET 永远够不着
        local r = Route.new()
        r:any("/shadow", function(ctx) ctx:text(200, "any") end)
        r:get("/shadow", function(ctx) ctx:text(201, "get") end)
        local resp = dispatch(r, "GET", "/shadow") or {}
        t:eq(200, resp.code, "ANY 已注册时 GET 同路径被拒")
        t:eq("any", resp.body, "生效的仍是 ANY 那条")
        -- 反向：先具体方法再 ANY。ANY 有 GET|HEAD 盖不住的方法，够得着，不算被遮蔽，
        -- 两条并存并按注册顺序命中
        local r2 = Route.new()
        r2:get("/shadow2", function(ctx) ctx:text(200, "get") end)
        r2:any("/shadow2", function(ctx) ctx:text(201, "any") end)
        t:eq(200, (dispatch(r2, "GET", "/shadow2") or {}).code, "GET 仍走先注册的 get() 那条")
        t:eq("any", (dispatch(r2, "PUT", "/shadow2") or {}).body, "其余方法落到后注册的 ANY")
        -- 此时 ANY 已把 POST 整个包住，再注册 POST 就是永远够不着，注册期拒掉
        r2:post("/shadow2", function(ctx) ctx:text(202, "post") end)
        t:eq(201, (dispatch(r2, "POST", "/shadow2") or {}).code,
             "被在先的 ANY 全包住的 POST 注册被忽略")
        -- 掩码互不相干就不算遮蔽：GET 挡不住 POST，这条照常注册
        local r3 = Route.new()
        r3:get("/shadow3", function(ctx) ctx:text(200, "get") end)
        r3:post("/shadow3", function(ctx) ctx:text(202, "post") end)
        t:eq(202, (dispatch(r3, "POST", "/shadow3") or {}).code,
             "GET 与 POST 掩码不相干，POST 同路径可注册")
    end
    do
        -- (b) 参数名不同但路由等价：匹配时参数名不参与比对，两条完全一样
        local r = Route.new()
        r:get("/user/{id}", function(ctx) ctx:text(200, ctx.params.id or "") end)
        r:get("/user/{uid}", function(ctx) ctx:text(201, "second") end)
        local resp = dispatch(r, "GET", "/user/7") or {}
        t:eq(200, resp.code, "{id} 与 {uid} 视为同一条路由，后者被拒")
        t:eq("7", resp.body, "生效的是先注册的 {id}")
        -- 段数不同不算等价
        r:get("/user/{id}/edit", function(ctx) ctx:text(202, "edit") end)
        t:eq(202, (dispatch(r, "GET", "/user/7/edit") or {}).code, "段数不同不算重复")
        -- 字面量段内容不同也不算等价
        r:get("/other/{id}", function(ctx) ctx:text(203, "other") end)
        t:eq(203, (dispatch(r, "GET", "/other/7") or {}).code, "字面量不同不算重复")
    end
    do
        -- 已知未覆盖：通配段吞并。/s/* 会让后注册的 /s/css 永远够不着，但"谁比谁宽泛"
        -- 在 OPT 与 WILD 组合下是偏序判定，判宽了会误杀合法注册，故不做——本用例把这个
        -- 缺口钉成显式行为，哪天补上了这里会红，是提醒不是回归
        local r = Route.new()
        r:get("/s/*", function(ctx) ctx:text(200, "wild") end)
        r:get("/s/css", function(ctx) ctx:text(201, "css") end)
        local resp = dispatch(r, "GET", "/s/css") or {}
        t:eq(200, resp.code, "通配段吞并暂不检测：/s/css 注册成功但被 /s/* 遮住")
        t:eq("wild", resp.body, "命中的仍是先注册的通配路由")
    end

    -- ── 9. 流式路由（chunked） ──────────────────────────────────────────────

    -- 喂一帧给 net_recv：首帧带首行与头部，数据帧只带 body，终止块两者都没有
    local function feed(r, slice, pack, fd, skid)
        r:net_recv(nil, fd or 1, skid or 1, 0, slice, pack, nil)
    end
    -- 走完一条完整的 chunked 请求：首帧 → 若干数据块 → 终止块
    local function feed_stream(r, path, chunks, headers, fd, skid)
        feed(r, SLICE_TYPE.START, make_pack("POST", path, nil, headers), fd, skid)
        for _, c in ipairs(chunks) do
            feed(r, SLICE_TYPE.SLICE, { _body = c }, fd, skid)
        end
        feed(r, SLICE_TYPE.END, {}, fd, skid)
    end
    -- 把每次回调的 slice 记进 log；收齐时回显拼起来的 body
    local function echo_stream(log)
        return function(ctx, slice, data)
            log[#log + 1] = slice
            if SLICE_TYPE.START == slice then
                ctx.buf = {}
            elseif STREAM_ABORT == slice then
                ctx.buf = nil
            elseif SLICE_TYPE.END == slice then
                ctx:text(200, table.concat(ctx.buf))
            elseif 0 == slice then
                ctx:text(200, data or "")
            else
                ctx.buf[#ctx.buf + 1] = data
            end
        end
    end

    -- 9.1 三块按序到齐，回调次数与顺序均正确
    do
        local r = Route.new()
        local log = {}
        r:post_stream("/st", echo_stream(log))
        last_resp = nil
        feed_stream(r, "/st", { "aaa", "bbbb", "c" })
        t:eq(200, (last_resp or {}).code, "chunked 收齐 → 200")
        t:eq("aaabbbbc", (last_resp or {}).body, "分块按序拼齐")
        t:eq(5, #log, "START + 3 数据块 + END 共 5 次回调")
        t:eq(SLICE_TYPE.START, log[1], "第一次是 START")
        t:eq(SLICE_TYPE.SLICE, log[2], "中间是 SLICE")
        t:eq(SLICE_TYPE.END, log[5], "最后是 END")
    end

    -- 9.1a 首帧回调里读得到请求头：_pack 要等 START 回调跑完才摘（口径同 C 侧 _router_st_begin），
    -- 提前摘的话 ctx:header 在 START 里恒返 nil。ctx.headers 不是等价替代——它的键是报文原样
    -- 大小写，而 ctx:header 走 C 的 buf_icompare 大小写无关
    do
        local r = Route.new()
        local seen = {}
        r:post_stream("/hd", function(ctx, slice, data)
            if SLICE_TYPE.START == slice then
                seen.start = ctx:header("content-type")
                ctx.buf = {}
            elseif SLICE_TYPE.END == slice then
                ctx:text(200, "ok")
            else
                seen.mid = ctx:header("content-type")
                ctx.buf[#ctx.buf + 1] = data
            end
        end)
        last_resp = nil
        feed_stream(r, "/hd", { "a" }, { ["Content-Type"] = "text/plain" })
        t:eq("text/plain", seen.start, "START 回调里 ctx:header 读得到，且大小写无关")
        t:eq(nil, seen.mid, "首帧之后 _pack 已摘，ctx:header 返 nil")
        t:eq(200, (last_resp or {}).code, "流照常收齐")
    end

    -- 9.2 零数据块：只有首帧与终止块，回显空 body
    do
        local r = Route.new()
        local log = {}
        r:post_stream("/st", echo_stream(log))
        last_resp = nil
        feed_stream(r, "/st", {})
        t:eq(200, (last_resp or {}).code, "零数据块 → 200")
        t:eq("", (last_resp or {}).body, "零数据块 body 为空")
        t:eq(2, #log, "只有 START 与 END 两次回调")
    end

    -- 9.3 chunked 打到普通路由 → 411 并关连接，后续分片静默丢
    do
        local r = Route.new()
        r:post("/plain", function(ctx) ctx:text(200, "ok") end)
        last_resp = nil
        closed_log = {}
        feed(r, SLICE_TYPE.START, make_pack("POST", "/plain"))
        t:eq(411, (last_resp or {}).code, "chunked 打普通路由 → 411")
        t:eq(1, #closed_log, "411 后关连接")
        last_resp = nil
        feed(r, SLICE_TYPE.SLICE, { _body = "x" })
        feed(r, SLICE_TYPE.END, {})
        t:eq(nil, last_resp, "被拒后的后续分片静默丢")
    end

    -- 9.3b chunked 匹配不上 → 404/405 而非 411：411 只表示"路由在但接不住 chunked"。
    -- 与 C 侧 _router_chunked_probe 同形；改前无流式路由时会被短路成一律 411
    do
        local r = Route.new()
        r:post("/plain", function(ctx) ctx:text(200, "ok") end)
        last_resp = nil
        closed_log = {}
        feed(r, SLICE_TYPE.START, make_pack("POST", "/nope"))
        t:eq(404, (last_resp or {}).code, "chunked 打未注册路径 → 404")
        t:eq(1, #closed_log, "404 后关连接")
        -- 路径对上但方法掩码不交也是 404，不是 405
        last_resp = nil
        feed(r, SLICE_TYPE.START, make_pack("PUT", "/plain"))
        t:eq(404, (last_resp or {}).code, "chunked 方法不匹配 → 404")
        -- 405 只由方法名不认识产生
        last_resp = nil
        feed(r, SLICE_TYPE.START, make_pack("FROB", "/plain"))
        t:eq(405, (last_resp or {}).code, "chunked 未知方法 → 405")
    end

    -- 9.3a 命名路由反查：Route:url 拿注册时的完整路径回填占位符
    do
        local r = Route.new()
        r:get("/user/{id}", function(ctx) ctx:text(200, "u") end):name("user.show")
        r:get("/file/{path?}", function(ctx) ctx:text(200, "f") end):name("file.show")
        r:get("/static/*", function(ctx) ctx:text(200, "s") end):name("static.any")
        t:eq("/user/42", r:url("user.show", { id = 42 }), "必填占位符回填")
        t:eq("/user/42?page=2", r:url("user.show", { id = 42, page = 2 }),
             "用不到的键拼成查询串")
        t:eq("/user/a%20b", r:url("user.show", { id = "a b" }), "路径段用 %20 编码空格")
        t:eq("/file", r:url("file.show", {}), "可选占位符缺参时整段丢弃")
        t:eq("/file/x", r:url("file.show", { path = "x" }), "可选占位符有参时照常回填")
        t:eq("/static", r:url("static.any", {}), "末尾通配缺参时整段丢弃")
        t:eq("/static/a/b", r:url("static.any", { ["*"] = "a/b" }),
             "末尾通配取 params[\"*\"]（'/' 不编码，通配本就跨段）")
        t:eq("/static/a//b/", r:url("static.any", { ["*"] = "a//b/" }),
             "通配值原样保留空段与尾斜杠，拆开重拼会改掉调用方给的值")
        t:eq("/static/a%20b/c", r:url("static.any", { ["*"] = "a b/c" }),
             "通配段内空格照编，作分隔的 '/' 不动")
        t:eq("/static/", r:url("static.any", { ["*"] = "" }),
             "通配值为空串留尾斜杠，与缺参丢整段区分开")
        t:eq("/user/42?a=1&b=2&c=3", r:url("user.show", { id = 42, a = 1, b = 2, c = 3 }),
             "多个剩余键按整串排序，同一组参数每次给出同一个 URL")
        t:eq(false, pcall(r.url, r, "user.show", {}), "必填占位符缺参报错")
        t:eq(false, pcall(r.url, r, "nosuch", {}), "未登记的名字报错")

        -- 生成与匹配共用 C 侧那一份段解析（Route:url 走注册时交回的 entry._segs），这里把生成的
        -- 路径喂回匹配器验闭环：段序列的消费方式、占位符编码与 C 侧解码得两两对得上
        local function back(u)
            local hit, _, _, idx, ps = r._c_router:match("GET", u)
            return hit, (nil ~= idx) and r._routes[idx]._name or nil, ps or {}
        end
        local hit, nm, ps = back(r:url("user.show", { id = 42 }))
        t:check(hit, "url 生成的路径能被 C 匹配器接住")
        t:eq("user.show", nm, "必填占位符往返后落回同一条路由")
        t:eq("42", ps.id, "占位符取值原样还回")
        _, nm, ps = back(r:url("user.show", { id = "a b" }))
        t:eq("user.show", nm, "含空格的取值往返后仍落回同一条路由")
        t:eq("a b", ps.id, "生成时编的 %20 被 C 侧解码还原")
        _, nm, ps = back(r:url("user.show", { id = 42, page = 2 }))
        t:eq("user.show", nm, "带查询串的 url 照样落回同一条路由")
        t:eq("42", ps.id, "查询串不影响占位符取值")
        _, nm = back(r:url("file.show", {}))
        t:eq("file.show", nm, "可选段缺参生成的路径落回同一条路由")
        _, nm, ps = back(r:url("file.show", { path = "x" }))
        t:eq("file.show", nm, "可选段有参生成的路径落回同一条路由")
        t:eq("x", ps.path, "可选占位符取值原样还回")
        _, nm = back(r:url("static.any", {}))
        t:eq("static.any", nm, "末尾通配缺参生成的路径落回同一条路由")
        _, nm = back(r:url("static.any", { ["*"] = "a/b" }))
        t:eq("static.any", nm, "末尾通配有参生成的路径落回同一条路由")
    end

    -- 9.4 准入中间件截断 → 401 且不建流；带对 token 则放行收齐
    do
        local r = Route.new()
        local log = {}
        r:define("auth", function(ctx, next)
            if "secret" ~= ctx:header("x-token") then
                ctx:text(401, "no")
                return
            end
            next()
        end)
        r:post_stream("/st", echo_stream(log), { "auth" })
        last_resp = nil
        closed_log = {}
        feed(r, SLICE_TYPE.START, make_pack("POST", "/st"))
        t:eq(401, (last_resp or {}).code, "准入截断 → 401")
        t:eq(1, #closed_log, "准入截断后关连接")
        t:eq(0, #log, "截断时 on_chunk 一次都没调到")
        last_resp = nil
        feed_stream(r, "/st", { "ok" }, { ["x-token"] = "secret" })
        t:eq(200, (last_resp or {}).code, "带对 token 收齐 → 200")
        t:eq("ok", (last_resp or {}).body, "放行后 body 正确")
    end

    -- 9.5 一次到齐的请求打到流式路由 → 单次回调，slice 为 0，data 是完整 body
    do
        local r = Route.new()
        local log = {}
        r:post_stream("/st", echo_stream(log))
        last_resp = nil
        r:net_recv(nil, 1, 1, 0, 0, make_pack("POST", "/st", "whole"), nil)
        t:eq(200, (last_resp or {}).code, "一次到齐打流式路由 → 200")
        t:eq("whole", (last_resp or {}).body, "slice==0 时 data 即完整 body")
        t:eq(1, #log, "只调一次")
        t:eq(0, log[1], "slice 为 0")
    end

    -- 9.6 连接中途断开 → 投 STREAM_ABORT；记录随之摘掉，skid 对不上的不投
    do
        local r = Route.new()
        local log = {}
        r:post_stream("/st", echo_stream(log))
        feed(r, SLICE_TYPE.START, make_pack("POST", "/st"))
        feed(r, SLICE_TYPE.SLICE, { _body = "half" })
        last_resp = nil
        r:closed(1, 1)
        t:eq(3, #log, "START + SLICE + ABORT 共 3 次")
        t:eq(STREAM_ABORT, log[3], "中途断开 → 投 STREAM_ABORT")
        t:eq(nil, last_resp, "ABORT 那次不写响应")
        r:closed(1, 1)
        t:eq(3, #log, "记录已摘，重复 closed 不再投")
        -- skid 对不上（fd 被新连接复用）不该动到别人的记录
        feed(r, SLICE_TYPE.START, make_pack("POST", "/st"))
        r:closed(1, 999)
        t:eq(4, #log, "skid 不匹配时不投 ABORT")
    end

    -- 9.7 同连接又来一个流式首帧 → 旧记录被顶掉并收到 ABORT
    do
        local r = Route.new()
        local log = {}
        r:post_stream("/st", echo_stream(log))
        feed(r, SLICE_TYPE.START, make_pack("POST", "/st"))
        feed(r, SLICE_TYPE.SLICE, { _body = "a" })
        feed(r, SLICE_TYPE.START, make_pack("POST", "/st"))
        t:eq(STREAM_ABORT, log[3], "旧流被顶掉时收到 ABORT")
        t:eq(SLICE_TYPE.START, log[4], "随后才是新流的 START")
    end

    -- 9.8 on_chunk 抛异常 → 500 + 关连接 + 仍投 ABORT 给清理机会
    do
        local r = Route.new()
        local log = {}
        r:post_stream("/st", function(_, slice)
            log[#log + 1] = slice
            if SLICE_TYPE.SLICE == slice then
                error("boom")
            end
        end)
        last_resp = nil
        closed_log = {}
        feed(r, SLICE_TYPE.START, make_pack("POST", "/st"))
        feed(r, SLICE_TYPE.SLICE, { _body = "x" })
        t:eq(500, (last_resp or {}).code, "on_chunk 抛异常 → 500")
        t:eq(1, #closed_log, "抛异常后关连接")
        t:eq(STREAM_ABORT, log[#log], "抛异常后仍投 ABORT")
    end

    -- 9.9 收齐仍未响应 → 兜底 500（对齐普通路由的 responded 机制）
    do
        local r = Route.new()
        r:post_stream("/st", function() end)
        last_resp = nil
        feed_stream(r, "/st", { "x" })
        t:eq(500, (last_resp or {}).code, "收齐仍未响应 → 兜底 500")
    end

    -- 9.10 CLOSE 订阅推迟到第一条流式路由：srey.watch_closed 没有反注册，
    -- 每个 router 订一次就是永久占位，没有流式路由的 router 不该付这份
    do
        local before = watch_n
        local r = Route.new()
        r:get("/plain", function(ctx) ctx:text(200, "ok") end)
        t:eq(before, watch_n, "纯 REST router 不订阅 CLOSE")
        r:post_stream("/lazy", function() end)
        t:eq(before + 1, watch_n, "第一条流式路由触发订阅")
        r:put_stream("/lazy2", function() end)
        t:eq(before + 1, watch_n, "第二条流式路由不重复订阅")
    end

end)
end)
