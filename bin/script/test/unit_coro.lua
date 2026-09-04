-- coro_sess 契约测试（第一批）：CLOSE 广播期间重新注册、并发 sendto 的等待者配对。
-- 对应 CLAUDE.md「Architecture Invariants」里 coro_sess 那一行的 ④ 与 ②。
-- 这套调度表与 lib/srey/coro.c 是镜像实现（见 feedback_coro_c_lua_mirror），
-- C 侧同场景在 test/task_coro_extra.c，两边测同一组场景、各自断言。
--
-- 单独挑这两条的理由：④ 此前只有静态代码走查确认过，从没被运行时驱动；
-- ② 的并发面只靠 test/task_kcp.c 的 fifo 用例间接覆盖（kcp_synsend 复用同一套底层），
-- 那个用例一旦被跳过、或两者调用路径分叉，这块就没人看着了。

local srey   = require("lib.srey")
local runner = require("test.runner")
local seri   = require("srey.seri")

local TCP_PORT = 15071
local UDP_PORT = 15072
local NSEND    = 4
-- 等超时唤醒要多给的富余：超时监控每 1s 才扫一次到期表(lib/srey 的 _coro_timeout)，
-- 所以 N 毫秒的等待最坏 N+1000 才被观察到。凡断言超时的地方一律等 N + 本值
local SETTLE   = 1300

-- 取 coro_sess 的条目数：向自己发 coros 调试命令，从汇总行里抠出来。
-- 发起请求的协程自己也占着一个条目（等 RESPONSE），所以这个值恒含一个常量 +1；
-- 下面一律比较前后差值，不看绝对值
local function _sessions()
    local ptr, sz = seri.pack("coros")
    local rdata, rsize = srey.request(srey.task_handle(), REQUEST_TYPE.REQ_DEBUG, ptr, sz, 0)
    local txt = rdata and srey.ud_str(rdata, rsize) or ""
    return tonumber(txt:match("(%d+) sessions")) or -1
end

srey.startup(function()
runner.run(function(t)
    -- 服务端侧回显：keep 那段要让客户端 skid 上真收到一条 RECV（走正常摘空，不是超时）。
    -- 只回显 accept 来的那条（client==0）：客户端那条的 RECV 有协程等着，先被 _resume_waiter
    -- 摘走、根本到不了这里，等它没人等了再回显就成互相打乒乓
    srey.on_recved(function(_, fd, skid, client, _, data, size)
        if 0 == client and data then
            srey.send(fd, skid, data, size, 1)
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
        local fd, skid = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT)
        t:check(fd and INVALID_SOCK ~= fd, "connect 回自己的监听口")
        if fd and INVALID_SOCK ~= fd then
            local first, second
            srey.fork(function()
                -- 3s 只是失败时的兜底上界，正常路径是被下面那句 close 的 CLOSE 广播唤醒
                first = srey._coro_wait(skid, srey.MSG_TYPE.RECV, 3000).mtype
                -- 关键动作：就在 CLOSE 的排空循环里，同一个 sess 上再注册一次
                second = srey._coro_wait(skid, srey.MSG_TYPE.RECV, 200).mtype
            end)
            srey.sleep(30)
            srey.close(fd, skid)
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
        local fd, skid = srey.udp(PACK_TYPE.NONE, "0.0.0.0", UDP_PORT)
        t:check(fd and INVALID_SOCK ~= fd, "udp 建 socket")
        if fd and INVALID_SOCK ~= fd then
            -- 默认 netread 超时 10s，失败时会把用例拖满；缩短后还原
            local old = srey.get_netread_timeout()
            srey.set_netread_timeout(500)

            local done, got = 0, {}
            for i = 1, NSEND do
                srey.fork(function()
                    local payload = "p" .. i
                    local d, s = srey.syn_sendto(fd, skid, "127.0.0.1", UDP_PORT, payload, #payload, 1)
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

            srey.close(fd, skid)
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
        local fd, skid = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT)
        t:check(fd and INVALID_SOCK ~= fd, "connect 回自己的监听口")
        if fd and INVALID_SOCK ~= fd then
            -- srey.connect 内部就在 skid 上等过 CONNECT，而 CONNECT 也在 may_keep 那六个里，
            -- 所以走到这里条目已经建好、keep 已经是 true 了
            t:eq(s0 + 1, _sessions(), "connect 之后 skid 上留下 keep=true 的条目")

            local woke
            srey.fork(function()
                woke = srey._coro_wait(skid, srey.MSG_TYPE.RECV, 3000).mtype
            end)
            srey.sleep(30)
            t:eq(s0 + 1, _sessions(), "RECV 等待者追加到已有条目，不新建第二个")

            -- 从对端回写让 RECV 正常到达，把 waiters 摘空（不是超时路径）
            srey.send(fd, skid, "z", 1, 1)
            srey.sleep(200)
            t:eq(srey.MSG_TYPE.RECV, woke, "等待者被 RECV 唤醒")
            t:eq(s0 + 1, _sessions(), "keep=true 的条目摘空 waiters 后仍留着")

            srey.close(fd, skid)
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
        local fd, skid = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", TCP_PORT)
        t:check(fd and INVALID_SOCK ~= fd, "connect 回自己的监听口")
        if fd and INVALID_SOCK ~= fd then
            t:eq(s0 + 1, _sessions(), "connect 之后条目已在（keep=true）")
            local woke
            srey.fork(function()
                woke = srey._coro_wait(skid, srey.MSG_TYPE.RECV, 200).mtype
            end)
            srey.sleep(200 + SETTLE)
            t:eq(srey.MSG_TYPE.TIMEOUT, woke, "等待者超时唤醒")
            -- 与上一段对照：同样是 keep=true 的条目，走摘空活着、走超时就删
            t:eq(s0, _sessions(), "超时摘空后条目被删，不受 keep=true 保护")
            srey.close(fd, skid)
        end
        srey.unlisten(lid)
    end
end)
end)
