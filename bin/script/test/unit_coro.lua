-- coro_sess 契约测试（第一批）：CLOSE 广播期间重新注册、并发 sendto 的等待者配对。
-- 对应 CLAUDE.md「Architecture Invariants」里 coro_sess 那一行的 ④ 与 ②。
-- 这套调度表与 C 侧是镜像实现（sess 规则在 lib/coro/coro.c，分发表在 lib/srey/coro_task.c；见 feedback_coro_c_lua_mirror），
-- C 侧同场景在 test/task_coro_extra.c，两边测同一组场景、各自断言。
--
-- 单独挑这两条的理由：④ 此前只有静态代码走查确认过，从没被运行时驱动；
-- ② 的并发面只靠 test/task_kcp.c 的 fifo 用例间接覆盖（kcp_synsend 复用同一套底层），
-- 那个用例一旦被跳过、或两者调用路径分叉，这块就没人看着了。
--
-- 末尾一段测消息载荷的释放时机：同步跑完的回调由 C 在分发返回后回收（攒批释放）；中途挂起过的回调
-- 留到回调结束由 _coro_exec 交还（分发返回时补挂带 __gc 的元表兜底）；等待者拿到的 msg 只到本协程下次挂起前有效。

local srey   = require("lib.srey")
local runner = require("test.runner")
local seri   = require("srey.seri")

local TCP_PORT = 15071
local UDP_PORT = 15072
local NSEND    = 4
-- 等超时唤醒要多给的富余：超时监控每 1s 才扫一次到期表(lib/srey 的 _coro_timeout)，
-- 所以 N 毫秒的等待最坏 N+1000 才被观察到。凡断言超时的地方一律等 N + 本值
local SETTLE   = 1300
-- 载荷释放时机那段的请求类型（避开 REQUEST_TYPE 保留值）。RT_ECHO + n：回显前先 sleep n 次（n=0..3）
local RT_ECHO       = 0x200
local RT_NEST       = 0x210 -- 回调里嵌套 serial / fork_wait / fork 后回显
local RT_THROW      = 0x211 -- 同步回显后抛错
local RT_THROW_Y    = 0x212 -- 挂起后回显再抛错
local RT_GATE       = 0x213 -- 等 NPOOL 条到齐后回显
local RT_HELD       = 0x214 -- 拿着本条 msg 挂起，记下挂起前后的元表与 data 后回显
local CORO_POOL_MAX = 128 -- 同 lib/srey.lua 的协程池上限
local NPOOL         = 200 -- 同时挂着的回调数，要超过 CORO_POOL_MAX

-- 取 coros 汇总行的计数：suspended(coro_sess 里的等待者) / sessions / fork_wait / serial(排队数) / yield total。
-- 发起请求的协程自己也占着一个条目（等 RESPONSE），所以这些值恒含一个常量 +1；
-- 下面一律比较前后差值，不看绝对值
local function _counts()
    local ptr, sz = seri.pack("coros")
    local rdata, rsize = srey.request(srey.task_handle(), REQUEST_TYPE.REQ_DEBUG, ptr, sz, 0)
    local txt = rdata and srey.ud_str(rdata, rsize) or ""
    local c = { txt:match("(%d+) suspended, (%d+) sessions, (%d+) fork_wait, (%d+) serial, (%d+) yield total") }
    assert(c[1], "coros summary not matched: " .. txt)-- 不挡的话各字段全 nil，计数断言变成 nil == nil 恒过
    return { susp = tonumber(c[1]), sess = tonumber(c[2]), fork = tonumber(c[3]), serial = tonumber(c[4]), nyield = tonumber(c[5]) }
end

-- coro_sess 的条目数
local function _sessions()
    return _counts().sess or -1
end

-- 向本 task 发请求并取回响应串；rdata 只到本协程下次挂起前有效，当场拷出
local function _req(rt, body)
    local rd, rs = srey.request(srey.task_handle(), rt, body)
    return rd and srey.ud_str(rd, rs) or nil
end

-- 按名字取函数的 upvalue，返回值与序号
local function _upvalue(f, name)
    for i = 1, 255 do
        local n, v = debug.getupvalue(f, i)
        if nil == n then
            return nil
        end
        if name == n then
            return v, i
        end
    end
end

-- 顺 upvalue 摸到 srey.lua 的 _coro_exec：message_dispatch → _dispatchers → REQUEST 分发 → _coro_run → _coro_new
local function _find_exec()
    local disp = _upvalue(message_dispatch, "_dispatchers")
    local run = disp and _upvalue(disp[srey.MSG_TYPE.REQUEST], "_coro_run")
    local new = run and _upvalue(run, "_coro_new")
    return new and _upvalue(new, "_coro_exec")
end

-- 回调拿不到自己那条 msg：在当前协程栈上找 _coro_exec 那一帧，取它的 msg 实参
local function _exec_msg(exec)
    for level = 2, 32 do
        local info = debug.getinfo(level, "f")
        if nil == info then
            return nil
        end
        if exec == info.func then
            for i = 1, 16 do
                local n, v = debug.getlocal(level, i)
                if nil == n then
                    return nil
                end
                if "msg" == n then
                    return v
                end
            end
            return nil
        end
    end
end

srey.startup(function()
runner.run(function(t)
    -- 服务端侧回显：keep 那段要让客户端 skid 上真收到一条 RECV（走正常摘空，不是超时）。
    -- 只回显 accept 来的那条（client==0）：客户端那条的 RECV 有协程等着，先被 _resume_waiter
    -- 摘走、根本到不了这里，等它没人等了再回显就成互相打乒乓
    srey.on_recved(function(_, sk, client, _, data, size)
        if 0 == client and data then
            srey.send(sk, data, size, 1)
        end
    end)

    -- ── ④ CLOSE 排空期间重新注册的等待者必须活下来 ────────────────────
    -- _net_close_dispatch 先把 keep 清 false、把 waiters 整批挪到局部表再逐个 resume。
    -- 被唤醒的协程若就在这一轮里重新在同一 sess 上注册，收尾那句 _coro_sess_del_empty
    -- 必须看见这个新等待者、不删条目。删错了它就永远等不到唤醒——连超时扫描也找不到它，
    -- 因为条目已经从 coro_sess 里没了。
    do
        local lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT, NET_EV.ACCEPT)
        t:check(ERR_FAILED ~= lid, "listen " .. TCP_PORT)

        -- srey.connect 内部已经等过 CONNECT 了，返回有效 fd 即已连上；
        -- 再调一次 srey.wait_connect 会去等第二条永远不来的 CONNECT，白等满 5s 连接超时
        local sk = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT)
        t:check(sk and sk.valid, "connect 回自己的监听口")
        if sk and sk.valid then
            local first, second
            srey.fork(function()
                -- 3s 只是失败时的兜底上界，正常路径是被下面那句 close 的 CLOSE 广播唤醒
                first = srey._coro_wait(sk.skid, srey.MSG_TYPE.RECV, 3000).mtype
                -- 关键动作：就在 CLOSE 的排空循环里，同一个 sess 上再注册一次
                second = srey._coro_wait(sk.skid, srey.MSG_TYPE.RECV, 200).mtype
            end)
            srey.sleep(30)
            srey.close(sk)
            srey.sleep(200 + SETTLE)

            t:eq(srey.MSG_TYPE.CLOSE, first, "第一次等待被 CLOSE 广播唤醒")
            t:eq(srey.MSG_TYPE.TIMEOUT, second, "排空期间重新注册的等待者仍被超时唤醒（条目没被收尾误删）")
        end
        srey.unlisten(lid)
    end

    -- ── ② N 个并发等待者、N 个响应，一个都不能丢 ──────────────────────
    -- 同一个 skid 上并发 syn_sendto：N 个协程都挂在 sess=skid 上等 RECVFROM，
    -- 每条数据报唤醒当时的队头。取队头这步若越过队头找、或摘空后把条目删早了，
    -- 后面的等待者就再也醒不过来——表现为 done < N（而不是报错）。
    -- 不断言"第 i 个协程收到第 i 份数据"：UDP 到达顺序本身不保证，
    -- 那样断言会变成一条随机失败的用例。能确定的是配对不丢、内容不串。
    do
        local sk = srey.udp(PACK_TYPE.NONE, "0.0.0.0", UDP_PORT)
        t:check(sk and sk.valid, "udp 建 socket")
        if sk and sk.valid then
            -- 默认 netread 超时 10s，失败时会把用例拖满；缩短后还原
            local old = srey.get_netread_timeout()
            srey.set_netread_timeout(500)

            local done, got = 0, {}
            for i = 1, NSEND do
                srey.fork(function()
                    local payload = "p" .. i
                    local d, s = srey.syn_sendto(sk, "127.0.0.1", UDP_PORT, payload, #payload, 1)
                    if d then
                        got[srey.ud_str(d, s)] = true
                    end
                    done = done + 1
                end)
            end
            srey.sleep(500 + SETTLE)
            srey.set_netread_timeout(old)

            t:eq(NSEND, done, "并发 sendto 的 N 个协程全部返回（没有等待者被丢在表里）")
            local n = 0
            for _ in pairs(got) do
                n = n + 1
            end
            t:eq(NSEND, n, "N 份响应各自配到一个协程，内容不重不漏")

            srey.close(sk)
        end
    end

    -- ── ② 等待者严格 FIFO：先注册的先醒 ───────────────────────────────
    -- 用合成 sess + 自发 RESPONSE 驱动：fork 按入队顺序起协程，两条 response 也按序投递，
    -- 所以这里的顺序是确定的（不像 UDP 那样受网络到达顺序影响）
    do
        local sess = srey.id()
        local order = {}
        for i = 1, 2 do
            srey.fork(function()
                srey._coro_wait(sess, srey.MSG_TYPE.RESPONSE, 2000)
                order[#order + 1] = i
            end)
        end
        srey.sleep(30)
        local me = srey.task_handle()
        srey.response(me, 0, sess, 0, "r1")
        srey.response(me, 0, sess, 0, "r2")
        srey.sleep(120)
        t:eq(2, #order, "两个等待者都被唤醒")
        t:eq(1, order[1], "FIFO: 先注册的先醒")
        t:eq(2, order[2], "FIFO: 后注册的后醒")
    end

    -- ── ② 队头 mtype 不匹配即视为无等待者，不越过队头去找 ──────────────
    -- 队头等 RECV，来的却是同 sess 的 RESPONSE：_get_coro_sess 必须返 nil，
    -- 那条 RESPONSE 落到"没人等"的兜底路径，队头继续等到自己超时
    do
        local sess = srey.id()
        local got
        srey.fork(function()
            got = srey._coro_wait(sess, srey.MSG_TYPE.RECV, 300).mtype
        end)
        srey.sleep(30)
        srey.response(srey.task_handle(), 0, sess, 0, "x")
        srey.sleep(300 + SETTLE)
        t:eq(srey.MSG_TYPE.TIMEOUT, got, "队头等 RECV 时不被同 sess 的 RESPONSE 唤醒")
    end

    -- ── ①③④ keep 决定条目存亡，CLOSE 清零不可逆 ──────────────────────
    -- RECV 属于 message_may_keep 为真的那六个 mtype（连接类，sess 即 skid，高频复用）。
    -- 摘空 waiters 后条目应当留着；直到 CLOSE 把 keep 清 false，它才真正可删。
    do
        local s0 = _sessions() -- 连之前先取基线
        local lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT, NET_EV.ACCEPT)
        local sk = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT)
        t:check(sk and sk.valid, "connect 回自己的监听口")
        if sk and sk.valid then
            -- srey.connect 内部就在 skid 上等过 CONNECT，而 CONNECT 也在 may_keep 那六个里，
            -- 所以走到这里条目已经建好、keep 已经是 true 了
            t:eq(s0 + 1, _sessions(), "connect 之后 skid 上留下 keep=true 的条目")

            local woke
            srey.fork(function()
                woke = srey._coro_wait(sk.skid, srey.MSG_TYPE.RECV, 3000).mtype
            end)
            srey.sleep(30)
            t:eq(s0 + 1, _sessions(), "RECV 等待者追加到已有条目，不新建第二个")

            -- 从对端回写让 RECV 正常到达，把 waiters 摘空（不是超时路径）
            srey.send(sk, "z", 1, 1)
            srey.sleep(200)
            t:eq(srey.MSG_TYPE.RECV, woke, "等待者被 RECV 唤醒")
            t:eq(s0 + 1, _sessions(), "keep=true 的条目摘空 waiters 后仍留着")

            srey.close(sk)
            srey.sleep(200)
            t:eq(s0, _sessions(), "CLOSE 清零 keep 后条目才真正删掉")
        end
        srey.unlisten(lid)
    end

    -- ── ⑤ 超时路径无视 keep ───────────────────────────────────────────
    -- 同样是 keep=true 的 RECV 条目，上一段里它扛过了"摘空"，这里走超时就该直接删。
    -- 留着的话该 skid 的 CLOSE 已被消费过，条目再没有任何路径能删掉
    do
        local s0 = _sessions()
        local lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT, NET_EV.ACCEPT)
        local sk = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT)
        t:check(sk and sk.valid, "connect 回自己的监听口")
        if sk and sk.valid then
            t:eq(s0 + 1, _sessions(), "connect 之后条目已在（keep=true）")
            local woke
            srey.fork(function()
                woke = srey._coro_wait(sk.skid, srey.MSG_TYPE.RECV, 200).mtype
            end)
            srey.sleep(200 + SETTLE)
            t:eq(srey.MSG_TYPE.TIMEOUT, woke, "等待者超时唤醒")
            -- 与上一段对照：同样是 keep=true 的条目，走摘空活着、走超时就删
            t:eq(s0, _sessions(), "超时摘空后条目被删，不受 keep=true 保护")
            srey.close(sk)
        end
        srey.unlisten(lid)
    end

    -- ── ⑤ 超时摘的是到期的那个等待者本身，不一定是队头 ─────────────────
    -- 与 C 侧 task_coro_extra 的 _test_timeout_non_head 对应。两个 sess 各排两个 RESPONSE 等待者：
    -- sessA 队尾先到期，sessB 队头先到期。还剩等待者的条目不删，之后的 RESPONSE 叫醒剩下那个
    do
        local s0 = _sessions()
        local sessA, sessB = srey.id(), srey.id()
        local woke = {}
        local plan = { { sessA, 3000 }, { sessA, 200 }, { sessB, 200 }, { sessB, 3000 } }
        for i = 1, #plan do
            srey.fork(function()
                woke[i] = srey._coro_wait(plan[i][1], srey.MSG_TYPE.RESPONSE, plan[i][2]).mtype
            end)
        end
        srey.sleep(30)
        t:eq(s0 + 2, _sessions(), "两个 sess 各一个条目")
        srey.sleep(200 + SETTLE)
        t:eq(nil, woke[1], "sessA 队头未到期，仍在等")
        t:eq(srey.MSG_TYPE.TIMEOUT, woke[2], "sessA 队尾先到期，被单独摘走")
        t:eq(srey.MSG_TYPE.TIMEOUT, woke[3], "sessB 队头到期被摘走")
        t:eq(nil, woke[4], "sessB 队尾未到期，仍在等")
        t:eq(s0 + 2, _sessions(), "还剩等待者的条目不删")
        local me = srey.task_handle()
        srey.response(me, 0, sessA, 0, "a")
        srey.response(me, 0, sessB, 0, "b")
        srey.sleep(150)
        t:eq(srey.MSG_TYPE.RESPONSE, woke[1], "sessA 剩下的等待者被 RESPONSE 叫醒")
        t:eq(srey.MSG_TYPE.RESPONSE, woke[4], "sessB 剩下的等待者被 RESPONSE 叫醒")
        t:eq(s0, _sessions(), "等待者全摘空后两个条目都删了")
    end

    -- ── 不可 yield 处调 srey.sleep：立即抛错，不登记 ────────────────────
    -- 以前先登记 coro_sess 再 yield 失败：nyield / nwait 只加不减，到点后旧条目带着旧 sess
    -- 唤醒同一协程后来的无关等待，报 different session 把它打断
    do
        local c0 = _counts()
        local ok, err, after
        srey.fork(function()
            ok, err = pcall(table.sort, { 2, 1 }, function(a, b)
                srey.sleep(50)
                return a < b
            end)
            srey.sleep(200)-- 同一协程随后的正常等待；旧行为下 50ms 时被旧条目打断
            after = true
        end)
        srey.sleep(400)
        t:eq(false, ok, "sort 比较器里 sleep 抛错")
        t:check(nil ~= string.find(tostring(err), "cannot yield", 1, true),
                "登记前就报 cannot yield（实际 " .. tostring(err) .. "）")
        t:eq(true, after, "随后的 sleep 正常走完，没被旧唤醒打断")
        local c1 = _counts()
        t:eq(c0.susp, c1.susp, "coro_sess 等待者不残留")
        t:eq(c0.nyield, c1.nyield, "nyield 不残留")
        local hit = 0
        for _ = 1, 4 do
            srey.fork(function() srey.sleep(20); hit = hit + 1 end)
        end
        srey.sleep(100)
        t:eq(4, hit, "之后的 fork 全部执行")
    end

    -- ── 不可 yield 处调各挂起接口：报错带接口名，不登记 ────────────────────
    -- 守卫在各挂起接口入口，报的是业务调的那个接口，不是内部的 _coro_wait。
    -- 连接类传失效连接：守卫要排在 wait_skid / sock_session 之前，否则直接返回、根本不报错。
    -- 末尾在裸 coroutine.wrap 里调 sleep：不在框架协程里，走守卫的另一条分支
    do
        local sk = srey.sock_invalid()
        local me = srey.task_handle()
        local cases = {
            { "srey.request", function() srey.request(me, 0x100, "x") end },
            { "srey.syn_send", function() srey.syn_send(sk, "x") end },
            { "srey.syn_recv", function() srey.syn_recv(sk) end },
            { "srey.syn_slice", function() srey.syn_slice(sk) end },
            { "srey.wait_connect", function() srey.wait_connect(sk) end },
            { "srey.wait_ssl_exchanged", function() srey.wait_ssl_exchanged(sk) end },
            { "srey.wait_handshaked", function() srey.wait_handshaked(sk) end },
            { "srey.sync_close", function() srey.sync_close(sk) end },
            { "srey.syn_sendto", function() srey.syn_sendto(sk, "127.0.0.1", UDP_PORT, "x") end },
            { "srey.connect", function() srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT) end },
        }
        local c0 = _counts()
        local name, call, ok, err
        for i = 1, #cases do
            name, call = cases[i][1], cases[i][2]
            ok, err = pcall(table.sort, { 2, 1 }, function(a, b)
                call()
                return a < b
            end)
            t:eq(false, ok, "sort 比较器里 " .. name .. " 抛错")
            -- 接口名紧跟原因，顺带区分 syn_send 与 syn_sendto
            t:check(nil ~= string.find(tostring(err), name .. " cannot yield", 1, true),
                    name .. " 报错带接口名且说明挂不起（实际 " .. tostring(err) .. "）")
        end
        ok, err = pcall(coroutine.wrap(function() srey.sleep(10) end))
        t:eq(false, ok, "裸 coroutine.wrap 里 sleep 抛错")
        t:check(nil ~= string.find(tostring(err), "srey.sleep must be called from within", 1, true),
                "报错说明不在框架协程里（实际 " .. tostring(err) .. "）")
        local c1 = _counts()
        t:eq(c0.sess, c1.sess, "coro_sess 条目数不变")
        t:eq(c0.susp, c1.susp, "coro_sess 等待者不残留")
        t:eq(c0.nyield, c1.nyield, "nyield 不残留")
    end

    -- ── 超时合成消息的 msg() 取法 ─────────────────────────────────────
    -- _timeout_wake 合成的 TIMEOUT 消息是普通表，msg() 靠 TIMEOUT_MSG_MT.__call；
    -- srey.request / syn_sendto / 各 wait_* 的超时分支都经过它
    do
        local sess = srey.id()
        local msg = srey._coro_wait(sess, srey.MSG_TYPE.RESPONSE, 100)
        local mtype, msess = msg()
        t:eq(srey.MSG_TYPE.TIMEOUT, mtype, "合成消息 msg() 第一个值是 TIMEOUT")
        t:eq(sess, msess, "合成消息 msg() 第二个值是 sess")
        t:eq(2, select("#", msg()), "合成消息 msg() 只返回 mtype、sess 两个值")

        -- srey.request 的超时分支：收到请求不回
        srey.on_requested(function() end)
        local old = srey.get_request_timeout()
        srey.set_request_timeout(100)
        local t0 = srey.timer_ms()
        local rok, rdata, rsize = pcall(srey.request, srey.task_handle(), 0x100, "x")
        srey.set_request_timeout(old)
        t:eq(true, rok, "request 超时分支不抛错（实际 " .. tostring(rdata) .. "）")
        t:eq(nil, rdata, "request 超时 rdata 为 nil")
        t:eq(nil, rsize, "request 超时 rsize 为 nil")
        t:check(srey.timer_ms() - t0 >= 100, "等满请求超时才返回")
    end

    -- ── 回调载荷活到回调结束：挂起之后再读 data ──────────────────────────
    -- 处理端都在挂起之后才读 data 并原样回给请求端比对。挂起期间载荷若被提前放掉，
    -- ASan 构建当场报 heap-use-after-free，release 构建多半读到被别的分配覆盖的内容。
    -- 内容是几 KB 到 70000 字节的长串、各条首尾不同，比对用 check 不用 eq，免得失败时把整串打进日志
    local inflight, peak = 0, 0
    local ser = srey.serial()
    local exec = _find_exec()
    local held = {}-- RT_HELD 回调记下的观察值
    srey.on_requested(function(reqtype, sess, src, data, size)
        if reqtype >= RT_ECHO and reqtype <= RT_ECHO + 3 then
            for _ = 1, reqtype - RT_ECHO do
                srey.sleep(5)
            end
            srey.response(src, reqtype, sess, ERR_OK, srey.ud_str(data, size))
        elseif RT_NEST == reqtype then
            local s0 = srey.ud_str(data, size)
            -- serial 里挂起：并发的第二条在队列里裸 yield，由 serial_wakes 叫醒
            local ok, s1 = ser(function()
                srey.sleep(10)
                return srey.ud_str(data, size)
            end)
            -- fork_wait：一个子协程同步读，一个 sleep 过后（已是别的消息在分发）再读父回调的 data
            local res = srey.fork_wait({
                function() return srey.ud_str(data, size) end,
                function()
                    srey.sleep(5)
                    return srey.ud_str(data, size)
                end,
            })
            -- fork 的子协程在本次分发末尾起跑，此时本回调正挂在下面的 sleep 上
            local s2
            srey.fork(function() s2 = srey.ud_str(data, size) end)
            srey.sleep(5)
            local good = ok and s1 == s0 and res[1].ok and res[1].val == s0 and res[2].ok and res[2].val == s0
                and s2 == s0 and srey.ud_str(data, size) == s0
            srey.response(src, reqtype, sess, ERR_OK, good and s0 or "BAD")
        elseif RT_THROW == reqtype then
            srey.response(src, reqtype, sess, ERR_OK, srey.ud_str(data, size))
            error("unit_coro: sync handler throws on purpose")
        elseif RT_THROW_Y == reqtype then
            srey.sleep(5)
            srey.response(src, reqtype, sess, ERR_OK, srey.ud_str(data, size))
            error("unit_coro: handler throws after yield on purpose")
        elseif RT_GATE == reqtype then
            -- 到齐 NPOOL 条才往下走，同一时刻挂着的回调数就是 NPOOL；2s 只是失败时的兜底
            inflight = inflight + 1
            if inflight > peak then
                peak = inflight
            end
            local deadline = srey.timer_ms() + 2000
            while peak < NPOOL and srey.timer_ms() < deadline do
                srey.sleep(10)
            end
            inflight = inflight - 1
            srey.response(src, reqtype, sess, ERR_OK, srey.ud_str(data, size))
        elseif RT_HELD == reqtype then
            -- debug.getmetatable 绕过 __metatable，看得到挂的是哪张元表
            local m = exec and _exec_msg(exec)
            held.msg = m
            if m then
                held.mt0 = getmetatable(m)
                held.gc0 = nil ~= debug.getmetatable(m).__gc
                srey.sleep(5)
                held.mt1 = getmetatable(m)
                held.gc1 = nil ~= debug.getmetatable(m).__gc
                held.same = nil ~= m.data and data == m.data
            end
            srey.response(src, reqtype, sess, ERR_OK, srey.ud_str(data, size))
        end
    end)
    do
        local p = "y1<" .. string.rep("a", 70000) .. ">"
        t:check(p == _req(RT_ECHO + 1, p), "回调 sleep 一次后读 data，内容不变")
        p = "y3<" .. string.rep("b", 70000) .. ">"
        t:check(p == _req(RT_ECHO + 3, p), "回调 sleep 三次后读 data，内容不变")
    end

    -- 回调拿着自己那条 msg 挂起：消息先挂不带 __gc 的元表，分发返回时还被拿着才补挂带 __gc 的那张；
    -- 两张的 __metatable 都是 "msg"。回调结束由 _coro_exec 交还载荷，之后 msg.data 为 nil
    do
        local p = "hd<" .. string.rep("j", 70000) .. ">"
        t:check(p == _req(RT_HELD, p), "拿着 msg 挂起后回显，内容不变")
        if t:check(nil ~= held.msg, "摸到回调自己那条 msg") then
            t:eq("msg", held.mt0, "挂起前 getmetatable(msg) 是 msg")
            t:eq("msg", held.mt1, "挂起后 getmetatable(msg) 仍是 msg")
            t:eq(false, held.gc0, "首次挂起前挂的是不带 __gc 的元表")
            t:eq(true, held.gc1, "分发返回时还被拿着，补挂了带 __gc 的元表")
            t:eq(true, held.same, "挂起后 msg.data 仍是原载荷")
            t:eq(nil, held.msg.data, "回调结束交还后 msg.data 为 nil")
            t:eq("msg", getmetatable(held.msg), "交还后元表不变")
        end
        held.msg = nil
    end

    -- 两条并发：先到的在 serial 里 sleep，后到的在 serial 队列里等；各自再 fork_wait、fork
    do
        local pa = "na<" .. string.rep("c", 70000) .. ">"
        local pb = "nb<" .. string.rep("d", 70000) .. ">"
        local res = srey.fork_wait({
            function() return _req(RT_NEST, pa) end,
            function() return _req(RT_NEST, pb) end,
        })
        t:check(pa == res[1].val, "嵌套 serial / fork_wait / fork：先进锁那条的 data 一直有效")
        t:check(pb == res[2].val, "嵌套 serial / fork_wait / fork：排过 serial 队那条的 data 一直有效")
    end

    -- 回调抛错：同步抛、挂起后抛，之后的请求照常
    do
        local p = "ts<" .. string.rep("e", 70000) .. ">"
        t:check(p == _req(RT_THROW, p), "同步回调回显后抛错，响应已发出")
        p = "ty<" .. string.rep("f", 70000) .. ">"
        t:check(p == _req(RT_THROW_Y, p), "挂起后读 data 回显再抛错，内容不变")
        p = "a1<" .. string.rep("g", 70000) .. ">"
        t:check(p == _req(RT_ECHO + 1, p), "抛错之后挂起型回调照常")
        p = "a0<" .. string.rep("h", 70000) .. ">"
        t:check(p == _req(RT_ECHO, p), "抛错之后同步回调照常")
    end

    -- 池满：NPOOL 条请求的回调同时挂着，跑完回池时超出 CORO_POOL_MAX 的协程直接退出
    do
        local funcs = {}
        for i = 1, NPOOL do
            local p = string.format("pool%03d<", i) .. string.rep(string.char(97 + i % 26), 4096) .. ">"
            funcs[i] = function() return p == _req(RT_GATE, p) end
        end
        local res = srey.fork_wait(funcs)
        local nok = 0
        for i = 1, NPOOL do
            if res[i].ok and res[i].val then
                nok = nok + 1
            end
        end
        t:check(peak > CORO_POOL_MAX, "同时挂着的回调超过协程池上限（实际 " .. peak .. "）")
        t:eq(NPOOL, nok, "池满时每条回调读到的 data 都对、都响应了")
        t:eq(0, inflight, "回调全部跑完")
    end

    -- ── 等待者拿到的 msg：下次挂起前 data 可读，之后为 nil ────────────────
    -- 等待者被唤醒那次分发里没有回调协程留着它，分发一返回 C 就释放载荷
    do
        local me = srey.task_handle()
        local sess = srey.id()
        local p = "w<" .. string.rep("w", 70000) .. ">"
        srey.response(me, 0, sess, ERR_OK, p)
        local msg = srey._coro_wait(sess, srey.MSG_TYPE.RESPONSE, 2000)
        t:eq(srey.MSG_TYPE.RESPONSE, msg.mtype, "等到 RESPONSE")
        t:check(p == (msg.data and srey.ud_str(msg.data, msg.size)), "RESPONSE：下次挂起前 msg.data 可读、内容对")
        srey.sleep(1)
        t:eq(nil, msg.data, "RESPONSE：下次挂起之后 msg.data 为 nil")
    end
    do
        local lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT, NET_EV.ACCEPT)
        local sk = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT)
        if t:check(sk and sk.valid, "connect 回自己的监听口") then
            -- 发完不挂起就登记等待，回显的 RECV 不会先落到 on_recved 去
            local p = "recv-waiter"
            srey.send(sk, p, #p, 1)
            local msg = srey._coro_wait(sk.skid, srey.MSG_TYPE.RECV, 3000)
            t:eq(srey.MSG_TYPE.RECV, msg.mtype, "等到 RECV")
            t:eq(p, msg.data and srey.ud_str(msg.data, msg.size), "RECV：下次挂起前 msg.data 可读、内容对")
            srey.sleep(1)
            t:eq(nil, msg.data, "RECV：下次挂起之后 msg.data 为 nil")
            srey.close(sk)
        end
        srey.unlisten(lid)
    end

    -- ── 同步回调不调 msg_release，挂起过的回调各调一次 ─────────────────────
    -- _coro_exec 的 msg_release 是 srey.lua 加载时取的局部别名，换 task.msg_release 换不到它，
    -- 只能用前面 _find_exec 摸到的 _coro_exec 临时换成计数包装，测完还原
    do
        local orig, idx
        if exec then
            orig, idx = _upvalue(exec, "msg_release")
        end
        if t:check(nil ~= idx, "摸到 _coro_exec 的 msg_release") then
            local calls, live = 0, 0
            debug.setupvalue(exec, idx, function(m)
                calls = calls + 1
                if nil ~= m.data then
                    live = live + 1
                end
                return orig(m)
            end)
            local p = "c<" .. string.rep("i", 70000) .. ">"
            local good = 0
            for _ = 1, 20 do
                if p == _req(RT_ECHO, p) then
                    good = good + 1
                end
            end
            if p == _req(RT_THROW, p) then
                good = good + 1
            end
            local sync_calls = calls
            -- 挂起过的：sleep 一次、三次、挂起后抛错、两条并发的嵌套，共 5 条
            calls, live = 0, 0
            for _, rt in ipairs({ RT_ECHO + 1, RT_ECHO + 3, RT_THROW_Y }) do
                if p == _req(rt, p) then
                    good = good + 1
                end
            end
            local res = srey.fork_wait({
                function() return _req(RT_NEST, p) end,
                function() return _req(RT_NEST, p) end,
            })
            debug.setupvalue(exec, idx, orig)
            if p == res[1].val and p == res[2].val then
                good = good + 2
            end
            t:eq(26, good, "计数期间的请求全部回显正确")
            t:eq(0, sync_calls, "同步回调（含同步抛错）一次 msg_release 都不调")
            t:eq(5, calls, "挂起过的回调每条调一次 msg_release")
            t:eq(5, live, "每次调用时载荷都还在（没被提前放掉，也没重复放）")
        end
    end
end)
end)
