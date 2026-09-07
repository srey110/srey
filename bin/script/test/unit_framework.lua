-- srey 框架绑定层单元测试：srey.core (SSL/cert) + srey.task (timer/timeout)

local srey   = require("lib.srey")
local runner = require("test.runner")
local core   = require("srey.core")
local task   = require("srey.task")

-- bind_task 用例的 UDP 端口。端口必须全仓唯一（测试模块并发跑，UDP 建链只设
-- SO_REUSEADDR 不设 SO_REUSEPORT，撞了第二个 bind 直接失败且被守卫静默吞掉），
-- 新端口先 grep 再定，别写裸字面量
local UDP_PORT = 15046

srey.startup(function()
runner.run(function(t)
    -- ── srey.core: SSL 证书注册与查询 ─────────────────────────────────
    -- 使用一组独立 name（unit_pem/unit_p12）避免污染其他 task 用的 SSL_NAME.SERVER/CLIENT
    local NAME_PEM = "unit_pem"
    local NAME_P12 = "unit_p12"
    do
        -- 注册 PEM 服务端证书；返回 ssl ctx lightuserdata，未注册时返回 nil
        local ssl = core.cert_register(NAME_PEM, "ca.crt", "server.crt", "server.key")
        t:check(ssl ~= nil, "cert_register PEM 返回 ssl ctx")
        -- ssl_qury 用同 name 查回同一指针
        local q = core.ssl_qury(NAME_PEM)
        t:check(q ~= nil, "ssl_qury PEM 返回 ssl ctx")
        t:eq(ssl, q, "ssl_qury 返回与 register 同一指针")
        -- 未注册的 name 返回 nil
        t:eq(nil, core.ssl_qury("unregistered"), "ssl_qury 未注册 name 返回 nil")
        -- 重复 register 同一 name 返回 nil（evssl_register 已占用）
        local dup = core.cert_register(NAME_PEM, "ca.crt", "server.crt", "server.key")
        t:eq(nil, dup, "重复 register 同 name 拒绝")

        -- seclevel / verify 无返回值（OpenSSL 那两个函数本身就没有），只能钉"调完之后 ctx 还能用"：
        -- 任何一个把 ctx 搞坏，紧接着的 ssl_free 或再注册就会露馅
        core.ssl_seclevel(ssl, 1)
        core.ssl_verify(ssl, 0)
        -- 0 是 OpenSSL 文档里的默认值（不设下限），绑定层不该把它挡在值域外；排在 TLS1_2 之前跑，
        -- 跑完这一段 ctx 的下限与不加这条时一致
        t:eq(true, core.ssl_min_proto(ssl, TLS_VERSION.AUTO), "ssl_min_proto 收 0(不设下限)")
        t:eq(true, core.ssl_min_proto(ssl, TLS_VERSION.TLS1_2), "ssl_min_proto 合法版本返 true")
        t:eq(ssl, core.ssl_qury(NAME_PEM), "三个 setter 调完后 ctx 仍在注册表里")
        -- 非法入参必须被绑定层拒（收窄前会被 lua_tointeger 静默转 0）
        t:eq(false, pcall(core.ssl_seclevel, ssl, "x"), "ssl_seclevel 拒非数字")
        t:eq(false, pcall(core.ssl_min_proto, ssl, {}), "ssl_min_proto 拒非数字")
        -- 值域同样得拒：打错一个字节的版本号（0x0399）落在 TLS1_0..TLS1_3 之外，放过去的话
        -- OpenSSL 返 0、最低版本原样保持无下限，而业务以为自己已经把下限抬上去了
        t:eq(false, pcall(core.ssl_min_proto, ssl, 0x0399), "ssl_min_proto 拒表外版本号")
        t:eq(false, pcall(core.ssl_seclevel, ssl, 99), "ssl_seclevel 拒超上界")
        t:eq(false, pcall(core.ssl_seclevel, ssl, -1), "ssl_seclevel 拒负数")
    end

    -- ── core.listen / core.connect: netev 非整数不得静默降级 ───────────
    -- 用 lua_isinteger 三目取值时，整值浮点（Lua 里 2^2 恒为 float）与任何非数字都会静默变成
    -- NETEV_NONE：listen 返回合法 id，acp_cb / s_cb 却一个都不装，C 层、Lua 层、日志三处均无提示。
    -- 三条都在 task_listen 之前抛出，不会真去 bind 端口
    do
        t:eq(false, pcall(core.listen, PACK_TYPE.CUSTZ_FIXED, nil, "127.0.0.1", 0, 1.5),
             "listen netev 非整数浮点报错而不是静默 NETEV_NONE")
        t:eq(false, pcall(core.listen, PACK_TYPE.CUSTZ_FIXED, nil, "127.0.0.1", 0, "x"),
             "listen netev 非数字报错")
        t:eq(false, pcall(core.connect, PACK_TYPE.CUSTZ_FIXED, nil, "127.0.0.1", 0, 1.5),
             "connect netev 同口径")
    end

    -- ── core.task_list: 任意长度的名字都原样交出 ───────────────────────
    -- task 名在 C 层没有长度上限（task_new 里是 dup_zero），搬运用的 _task_entry.name 也是
    -- 堆指针而非定长缓冲。名字既不截断（截断的名字拿去 grab 会命中别的 task）也不丢，
    -- 于是"没有 name 字段"就只剩匿名 task 一种含义，不再与"名字太长"混在一起
    do
        local LONG = string.rep("L", 80)
        local SHORT = "tasklist_short"
        t:check(task.register("test.toplevel_bind", LONG, 0) ~= nil, "80 字节名字的 task 注册成功")
        t:check(task.register("test.toplevel_bind", SHORT, 0) ~= nil, "短名字的 task 注册成功")
        -- task.handle(nil) 取的是当前 task，grab 结果必须先判空再取句柄，否则匹配到的是自己
        local long_tk = task.grab(LONG)
        local short_tk = task.grab(SHORT)
        local long_h = long_tk and task.handle(long_tk)
        local short_h = short_tk and task.handle(short_tk)
        local long_item, short_item
        for _, item in ipairs(core.task_list()) do
            if nil ~= long_h and item.handle == long_h then
                long_item = item
            elseif nil ~= short_h and item.handle == short_h then
                short_item = item
            end
        end
        -- 收尾排在断言之前：断言失败会中断本块，两个 task 连同各自的 lua_State 活到进程结束
        if long_tk then
            task.close(long_tk)
            task.ungrab(long_tk)
        end
        if short_tk then
            task.close(short_tk)
            task.ungrab(short_tk)
        end
        t:check(long_tk ~= nil, "80 字节名字的 task 能按名 grab 到")
        t:check(long_item ~= nil, "超长名字的 task 仍出现在 task_list 里")
        t:eq(LONG, long_item and long_item.name, "80 字节名字原样交出，不截断不丢弃")
        t:eq(SHORT, short_item and short_item.name, "短名字照常带 name")
    end
    do
        -- p12_register
        local ssl = core.p12_register(NAME_P12, "client.p12", "srey")
        t:check(ssl ~= nil, "p12_register 返回 ssl ctx")
        t:eq(ssl, core.ssl_qury(NAME_P12), "ssl_qury 查回 p12 ssl ctx")
        -- 错误密码注册失败
        local bad = core.p12_register("unit_p12_bad", "client.p12", "wrong-password")
        t:eq(nil, bad, "p12_register 错误密码失败")
    end

    -- ── srey.task: timer_ms 单调递增 ──────────────────────────────────
    do
        local a = task.timer_ms()
        t:check(type(a) == "number" and a > 0, "timer_ms returns positive number")
        srey.sleep(20) -- 让出协程让时间真的走过
        local b = task.timer_ms()
        -- 用差值而非 b >= a：后者对"时钟冻住、恒返同一个值"同样成立。上界留宽，只挡"根本不走"
        t:check(b - a >= 15 and b - a < 5000, "timer_ms 随真实时间前进 (" .. (b - a) .. "ms)")
    end

    -- ── srey.task: set/get *_timeout round-trip ───────────────────────
    do
        -- 保存原值后还原（避免污染本 task 后续协程）
        local saved_req = task.get_request_timeout()
        local saved_con = task.get_connect_timeout()
        local saved_net = task.get_netread_timeout()

        task.set_request_timeout(7777)
        t:eq(7777, task.get_request_timeout(), "set/get_request_timeout")

        task.set_connect_timeout(8888)
        t:eq(8888, task.get_connect_timeout(), "set/get_connect_timeout")

        task.set_netread_timeout(9999)
        t:eq(9999, task.get_netread_timeout(), "set/get_netread_timeout")

        task.set_request_timeout(saved_req)
        task.set_connect_timeout(saved_con)
        task.set_netread_timeout(saved_net)
        t:eq(saved_req, task.get_request_timeout(), "request_timeout restored")
        t:eq(saved_con, task.get_connect_timeout(), "connect_timeout restored")
        t:eq(saved_net, task.get_netread_timeout(), "netread_timeout restored")
    end

    -- ── core.bind_task: 目标不存在须返回 false ────────────────────────
    -- 字符串名走 task_find_name 返 INVALID_TNAME 可挡；数字句柄原样下传，
    -- 只查 INVALID_TNAME 的话陈旧句柄会被放行，绑定后该连接下一条消息才被静默关闭
    do
        local fd, skid = srey.udp(PACK_TYPE.NONE, "0.0.0.0", UDP_PORT)
        t:check(fd and fd ~= INVALID_SOCK, "bind_task 用 udp 创建")
        if fd and fd ~= INVALID_SOCK then
            t:eq(false, srey.sock_bind_task(fd, skid, "no_such_task_name"), "未注册的字符串名返回 false")
            t:eq(false, srey.sock_bind_task(fd, skid, 0x7FFFFFFF), "不存在的数字句柄返回 false")
            t:eq(true, srey.sock_bind_task(fd, skid, task.handle()), "有效数字句柄绑定成功(确认没把有效的也挡掉)")
            srey.close(fd, skid)
        end
    end

    -- ── core.timeout 数值边界：(uint32_t) 截断会把超大延时变成极小值甚至 0(当场触发) ──
    do
        local big, neg = false, false
        srey.timeout(4294967296, function() big = true end)-- 2^32：截断成 0 则被 tw_add 当场回调
        srey.timeout(-1, function() neg = true end)
        srey.sleep(50)-- 让出协程，令已到期的 TIMEOUT 消息完成投递与派发
        t:eq(false, big, "超 UINT32_MAX 延时钳到上界,不当场触发")
        t:eq(true, neg, "非正延时立即触发")
    end

    -- ── srey.task: isclosing / name (当前 task) ───────────────────────
    do
        -- 当前 task 未关闭
        t:eq(false, task.isclosing(), "current task not closing")
        -- 注册名取自 test.lua 的 TESTS 第 2 列（本模块是 framework）；这里改了那边也要改，
        -- 但漂移会让本条断言直接失败，不像 reporter 那样只是静默少一行
        t:eq("framework", task.name(), "current task name")
        -- runner 的模块名就取自 task.name()，两者必须同源
        t:eq(task.name(), t.name, "runner 的模块名取自 task.name()")
    end

    -- ── srey.task: grab/ungrab 引用计数 ───────────────────────────────
    do
        -- 抓取 reporter task（必然存在）
        local rep = task.grab("reporter")
        t:check(rep ~= nil, "task.grab existing task")
        task.incref(rep)
        task.ungrab(rep) -- 平衡 incref
        task.ungrab(rep) -- 平衡 grab

        -- 不存在的 task name 返回 nil
        t:eq(nil, task.grab("__no_such_task__"), "task.grab missing returns nil")
    end

    -- ── srey.task: register 的可变参数个数 ─────────────────────────────
    -- 这些参数是逐个 push 进新建 lua_State 的，而新 state 的栈只有 45 个位置，
    -- 个数却完全由调用脚本给。lua_push* 只推进栈顶不扩容，release 构建下越界那道
    -- api_check 又是空操作，没有 lua_checkstack 的话 44 个参数就已经写到栈数组外面了
    do
        local args = {}
        for i = 1, 256 do
            args[i] = i
        end
        -- toplevel_bind 不读 ...，这里只关心参数搬运本身不越界
        local tk = task.register("test.toplevel_bind", "argstress", 0, table.unpack(args))
        t:check(tk ~= nil, "task.register 256 个参数不越界")
        -- 收尾：不关的话这个 task 连同它自己那个 lua_State 一直活到进程结束
        local helper = task.grab("argstress")
        if helper then
            task.close(helper)
            task.ungrab(helper)
        end
    end

    -- ── 崩溃向量：绑定层不该让脚本把整个进程打死 ──────────────────────
    do
        -- 顶层调 C 绑定：绑定层依赖的东西（ltask->lua、_curtask 全局）曾拖到 chunk
        -- 跑完才赋值，期间顶层这一句就是空指针解引用（进程 exit 139，本用例根本跑不到）
        local itop = task.register("test.toplevel_bind", "itop", 0)
        t:check(itop ~= nil, "顶层调 C 绑定不崩且注册成功")
        local h = task.grab("itop")
        if h then
            task.close(h)
            task.ungrab(h)
        end

        -- chunk 顶层抛非字符串错误对象：lua_tostring 返 NULL，曾原样喂给 LOG_ERROR 的 %s
        t:eq(nil, task.register("test.err_object", "errobj", 0), "chunk 抛 table：注册失败而非崩溃")

        -- sess=0：三个消费方都是 ASSERTAB，而 srey.core 可被业务直接 require，绕得过脚本守卫
        t:eq(false, pcall(core.timeout, 0, 100), "core.timeout sess=0 报错而非 abort")
        t:eq(false, pcall(core.request, task.handle(), 1, 0, "x"), "core.request sess=0 报错而非 abort")
        t:eq(false, pcall(core.multi_request, { task.handle() }, 1, 0, "x"),
            "core.multi_request sess=0 报错而非 abort")
    end

    -- ── srey.core: reqtype 的截断回归 ─────────────────────────────────
    -- subtype_t 是 uint16_t，曾用裸转换：65537 截成 1 == REQ_DEBUG，业务载荷被
    -- 接收方路由给 _debug_request，自己的 on_requested 一次都不触发
    do
        t:eq(false, pcall(core.call, 1, 65537, "x"), "core.call: reqtype 65537 被拒")
        t:eq(false, pcall(core.call, 1, -1, "x"), "core.call: reqtype 负值被拒")
        t:eq(false, pcall(core.request, 1, 65537, srey.id(), "x"), "core.request: reqtype 越界被拒")
        t:eq(false, pcall(core.multi_call, { 1 }, 65537, "x"), "core.multi_call: reqtype 越界被拒")
    end

    -- ── srey.core: 失败与成功的返回值个数必须一致 ────────────────────
    -- lpub_rtn_nil 写死的全仓规矩：返回值直接塞进另一个调用时少一个就整体错位
    do
        t:eq(2, select("#", core.udp(PACK_TYPE.NONE, "300.300.300.300", 0)),
            "core.udp 失败也返 2 个值")
        local fd, skid = core.udp(PACK_TYPE.NONE, "127.0.0.1", 0)
        t:check(fd and INVALID_SOCK ~= fd, "core.udp 绑定成功")
        if fd and INVALID_SOCK ~= fd then
            -- 用 table.pack 而非 select("#", ...)：后者把 fd/skid 吞掉，那个 socket
            -- 建出来就没有任何路径能 close 它（下面那句 close 关的是上面第一个）
            local ret = table.pack(core.udp(PACK_TYPE.NONE, "127.0.0.1", 0))
            t:eq(2, ret.n, "core.udp 成功返 2 个值")
            t:check(ret[1] and INVALID_SOCK ~= ret[1] and ret[2], "两个返回值都有效")
            if ret[1] and INVALID_SOCK ~= ret[1] then
                srey.close(ret[1], ret[2])
            end
            srey.close(fd, skid)
        end
        -- 同一条规矩也管 Lua 包装层：srey.connect 成功返 (fd, skid)，失败也得返两个。
        -- 走 ssl_qury 那条失败路径，它在任何挂起之前就返回，不依赖网络
        t:eq(2, select("#", srey.connect(PACK_TYPE.NONE, "no-such-ssl-name", "127.0.0.1", 1)),
            "srey.connect 失败也返 2 个值")
        -- ssl_qury 的契约只有 (true,nil)/(true,ssl)/(false,nil) 三档,没有"抛出"这一档：
        -- core.ssl_qury 是 luaL_checkstring,漏传 sslname 会抛,而 srey.connect 的 extra
        -- 释放排在它之后——websock 的 hsctx 就那样漏掉一份(lightuserdata,没有 __gc)
        t:eq(false, srey.ssl_qury(nil), "ssl_qury 漏传返 false 而不抛")
        t:eq(false, srey.ssl_qury(42), "ssl_qury 收非字符串返 false")
        t:eq(true, srey.ssl_qury(SSL_NAME.NONE), "SSL_NAME.NONE 仍是明文放行")
        t:eq(2, select("#", srey.connect(PACK_TYPE.NONE, nil, "127.0.0.1", 1)),
            "sslname 漏传时 srey.connect 仍返 2 个值")
    end

    -- ── srey.closing: CLOSING 回调被调到，且回调抛错不影响 task_ungrab ─────
    -- 这条路径 Lua 侧原先零注册：func_cbs[MSG_TYPE.CLOSING] 恒为 nil，_closing 里
    -- srey.xpcall(func) 那一支从没执行过。而 MEMORY 记的正是它出过
    -- "task ref 不归零 → _loader_task_closing 死循环 → Ctrl+C 失效"
    do
        local got = {}
        srey.on_requested(function(reqtype, _, _, data, size)
            if 200 == reqtype and data then
                got[#got + 1] = srey.ud_str(data, size)
            end
        end)
        local NORMAL, RAISE = "closing_ok", "closing_raise"
        t:check(nil ~= task.register("test.closing_probe", NORMAL, 0, "framework", 0),
                "closing 探针注册成功")
        t:check(nil ~= task.register("test.closing_probe", RAISE, 0, "framework", 1),
                "closing 抛错探针注册成功")
        srey.sleep(100)-- 等两个探针的 startup 跑完(closing 是在 startup 里注册的)
        local ntk = task.grab(NORMAL)
        if ntk then
            task.close(ntk)
            task.ungrab(ntk)
        end
        local rtk = task.grab(RAISE)
        if rtk then
            task.close(rtk)
            task.ungrab(rtk)
        end
        srey.sleep(300)-- task_close 是异步的,等 CLOSING 派发 + ungrab + free 走完
        t:eq(1, #got, "CLOSING 回调被调到一次 (" .. #got .. ")")
        t:eq("closed", got[1], "回调里 srey.call 回来的负载")
        -- 必须接住再判:grab 命中时返回的是裸 lightuserdata,没有 __gc,不 ungrab 就永久多一个 ref,
        -- 那个 task 再也不会 free,退出时 _loader_task_closing 等 maptasks 归零会等不到,Ctrl+C 失效
        local rtk2 = task.grab(RAISE)
        t:eq(nil, rtk2, "closing 抛错的 task 仍然消失(task_ungrab 未被跳过)")
        if rtk2 then
            task.ungrab(rtk2)
        end
    end

    -- ── srey.core: sock_session 的会话键写死 skid，与 C 侧 coro_sync 对齐 ────
    -- CLOSE 恒以 skid 为 sess 发出，挂在别的键上的等待者断连时一个都唤不到（ms>0 白等满
    -- 超时、ms==0 永久挂起），故不再开放自定义。fd 传 INVALID_SOCK：ev_props 对它直接早退
    do
        t:eq(false, core.session(-1, 2), "sock_session: 两参形态可调用，无效 fd 返 false")
        -- 旧的 sess=0 语义搬到独立接口，不再靠第 3 参表达
        t:eq(false, core.session_clear(-1, 2), "session_clear: 两参形态可调用，无效 fd 返 false")
    end

    -- ── srey.task: set_priority 超界 clamp 而不是被截断 ───────────────
    -- 裸 (int32_t) 会把 2^32 截成 0，clamp 看到的已经是合法的最低优先级，
    -- 于是"超界 clamp 到最大"的契约悄悄反过来。拿它跟一个普通超界值比，
    -- 两者必须 clamp 到同一个上限，且不能是 0
    do
        local saved = task.get_priority()
        task.set_priority(1000000)
        local capped = task.get_priority()
        t:check(capped > 0, "普通超界值 clamp 到非 0 上限")
        task.set_priority(4294967296)
        t:eq(capped, task.get_priority(), "超 int32 的值 clamp 到同一上限而非截成 0")
        task.set_priority(-4294967296)
        t:eq(0, task.get_priority(), "超下界 clamp 到 0")
        task.set_priority(saved)
        t:eq(saved, task.get_priority(), "优先级已还原")
    end

    -- ── srey.task: mem / memlimit (per-task lua_State 内存监控) ──
    -- memlimit 为软告警阈值：超阈值仅 LOG_WARN，不拒绝分配、不触发 LUA_ERRMEM
    do
        -- task.mem() 返回 lua_State 当前累计字节数；新建 state 后内部已 alloc 若干，必非 0
        local m0 = task.mem()
        t:check(m0 > 0, "task.mem() returns positive (got " .. tostring(m0) .. ")")
        t:check(m0 < 1024 * 1024 * 1024, "task.mem() 合理范围 < 1GB (got " .. tostring(m0) .. ")")

        -- memlimit(0) 禁用告警：大分配应成功
        task.memlimit(0)
        local ok_big = pcall(function()
            local _ = string.rep("y", 64 * 1024) -- 64KB
        end)
        t:eq(true, ok_big, "memlimit=0 时大分配应成功")

        -- memlimit = 当前 mem（告警阈值）：超阈值只 LOG_WARN、不拒绝分配 → 分配应成功
        collectgarbage("collect")
        local before = task.mem()
        task.memlimit(before)
        local ok_over = pcall(function()
            local _ = string.rep("z", 32 * 1024)
        end)
        task.memlimit(0) -- 解除告警阈值
        t:eq(true, ok_over, "memlimit=current mem 超阈值时分配应成功(软告警不拒绝)")
    end
end)
end)
