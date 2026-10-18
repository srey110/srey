-- KCP 绑定层测试(lkcp.c + kcp.lua):同一 task 内建 server / client 两个 UDP socket(PACK_UDP_KCP),
-- 两端约定固定 conv(不走 TCP 握手,聚焦绑定通路 + 数据往返)。
-- client send(同步) -> server on_recvedfrom echo -> sess 唤醒 client 协程 -> 校验 echo 内容。
-- ikcp_update 由 event 线程 tick 自动驱动,业务无需轮询。
-- 另验:会话关闭时 on_closed 第 5 参等于 k.sess(socket 本身关闭等于 skid);port 0 学习模式。

local srey   = require("lib.srey")
local runner = require("test.runner")
local kcp    = require("lib.kcp")

local SV_PORT  = 15042
local CLI_PORT = 15043
local DEAD_PORT = 15044 -- 专供 CLOSE 分支用例的 socket
local NOBODY_PORT = 15045 -- 无人监听的对端,令 send 必然挂起等超时
local RACE_PORT = 15050 -- 专供"并发换会话"用例的 socket
local CONV     = 1
local MSG      = "kcp_hello_lua"
local ASYNC_MSG = "kcp_async_lua" -- 发给 async 会话的消息,server 按内容挑它回显
local LEARN_MSG = "kcp_learn_lua" -- 发给学习模式会话的消息,同上

srey.startup(function()
runner.run(function(t)
    -- server / client 两个 UDP socket(不同端口,各自独立 conv 空间)
    local sv_sk = srey.udp(PACK_TYPE.UDP_KCP, "0.0.0.0", SV_PORT)
    t:check(sv_sk and sv_sk.valid, "server udp 创建")
    if not sv_sk or not sv_sk.valid then return end
    local cli_sk = srey.udp(PACK_TYPE.UDP_KCP, "0.0.0.0", CLI_PORT)
    t:check(cli_sk and cli_sk.valid, "client udp 创建")
    if not cli_sk or not cli_sk.valid then
        srey.close(sv_sk)
        return
    end

    -- on_closed 按第 5 参 sess 记下 erro:会话级关闭的 sess 是 k.sess,socket 本身关闭的是 sk.skid
    local closed = {}
    srey.on_closed(function(_, _, _, erro, sess)
        closed[sess] = erro
    end)
    -- on_closed 排在被唤醒的等待者之后跑,每 50ms 看一次,至多 1 秒
    local function wait_closed(sess)
        for _ = 1, 20 do
            if nil ~= closed[sess] then
                break
            end
            srey.sleep(50)
        end
        return closed[sess]
    end

    -- 两端 kcp 会话:conv 一致,互指对方端口;server 只异步 echo,client 要同步等响应
    local sv_kcp = kcp.new(sv_sk, CONV)
    t:eq(true, sv_kcp:start("127.0.0.1", CLI_PORT), "server kcp start")
    local cli_kcp = kcp.new(cli_sk, CONV, true)
    t:eq(true, cli_kcp:start("127.0.0.1", SV_PORT), "client kcp start")

    -- 同一底层 socket 上用相同 conv 再注册一次,同步 start 应因 conv 冲突返回 false
    local dup_kcp = kcp.new(cli_sk, CONV, true)
    t:eq(false, dup_kcp:start("127.0.0.1", SV_PORT), "duplicate conv start should fail")
    -- start 失败后会话已停(live 为假):sync send 须立即返回 nil(而非进 _coro_wait 空等满 netread 超时);
    -- copy=0 走 _ud_free_copy 兜底,string 传入时 utils.ud_free 内部跳过,验证不误释放也不崩
    t:eq(nil, (dup_kcp:send(MSG, #MSG, 0)), "已停 send 返回 nil(copy=0)")
    t:eq(nil, (dup_kcp:send(MSG, #MSG, 1)), "已停 send 返回 nil(copy=1)")
    -- start 失败后 C 侧 stopped 须已置 1(靠 ctx:start 的 self:stop(),与 kcp_synstart 同分支对齐)。
    -- live 只挡得住 Lua 侧的 send,挡不住 handle:stopped 留在 0 则它绕过守卫投到不存在的会话,
    -- 被 _kcp_resolve 静默丢弃,绑定层却照样返 true
    t:eq(false, dup_kcp:handle(srey.task_handle()), "start 失败后 handle 被 stopped 守卫拒绝")

    -- 数字句柄不探存在性的话,kcp_handle 会把推送目标指到一个已退出的 task 上,之后该会话每条
    -- 消息都被取出丢弃(限频告警)而绑定层照样返 true。用一个必然没分配过的句柄验探测生效
    -- (INVALID_TNAME 是 0,加个大偏移确保不是靠旧那道 0 判定过的)
    t:eq(false, cli_kcp:handle(srey.task_handle() + 1000000), "不存在的数字句柄被拒")
    t:eq(true, cli_kcp:handle(srey.task_handle()), "本 task 句柄仍可设")

    -- config 字段越界须报错并点出字段名,不能静默截断成另一个合法值：
    -- mtu = 2^32+1480 截成 int32 正好是 1480,ikcp_setmtu 会成功且不打 WARN,
    -- 调用方以为自己设的是别的值。报错发生在 kcp_start 之前,这几条都不碰网络
    do
        local ck = kcp.new(cli_sk, CONV + 90)
        local cfgok, cfgerr = pcall(ck.start, ck, "127.0.0.1", SV_PORT, { mtu = 4294968776 })
        t:eq(false, cfgok, "mtu 超 int32 被拒而非截成 1480")
        t:check(cfgerr and nil ~= tostring(cfgerr):find("mtu", 1, true),
                "报错点出字段名 mtu: " .. tostring(cfgerr))
        t:eq(false, (pcall(ck.start, ck, "127.0.0.1", SV_PORT, { nodelay = 4294967296 })),
             "nodelay 超界被拒(截断后是 0,开关会反)")
        t:eq(false, (pcall(ck.start, ck, "127.0.0.1", SV_PORT, { sndwnd = 2147483648 })),
             "sndwnd 超界被拒(截断后是 INT32_MIN)")
        t:eq(false, (pcall(ck.start, ck, "127.0.0.1", SV_PORT, { interval = "x" })),
             "非整数字段被拒")
        -- 窗口上界取 65535 而非 INT32_MAX：线上 wnd 字段是 16 位(ikcp.c 的 ikcp_encode16u),
        -- 更大的值既表达不出去,又会让收包窗口判定在半个序号空间上恒成立、rcv_buf 无上界涨
        t:eq(false, (pcall(ck.start, ck, "127.0.0.1", SV_PORT, { rcvwnd = 65536 })),
             "rcvwnd 超 16 位被拒")
        t:eq(false, (pcall(ck.start, ck, "127.0.0.1", SV_PORT, { sndwnd = 65536 })),
             "sndwnd 超 16 位被拒")
        -- 非整数与越界必须报不同的话:3.5 落在 [0, 65507] 里,报"out of range"会让人去查值域
        local fok, ferr = pcall(ck.start, ck, "127.0.0.1", SV_PORT, { mtu = 3.5 })
        t:eq(false, fok, "非整值浮点被拒")
        t:check(ferr and nil ~= tostring(ferr):find("must be an integer", 1, true),
                "浮点报非整数而不是越界: " .. tostring(ferr))
        -- config 传成非 table 时不能静默丢掉整份调参:那样会返 true、日志一行不打,
        -- 现场表现是"参数明明配了却没生效",没有任何线索
        t:eq(false, (pcall(ck.start, ck, "127.0.0.1", SV_PORT, "nodelay=1")),
             "config 传字符串被拒而非静默忽略")
        -- nodelay 有 0 / 1 / 2 三档;mtu 上界是 UDP 单报最大载荷 65507,再大满段发不出去
        t:eq(false, (pcall(ck.start, ck, "127.0.0.1", SV_PORT, { nodelay = 3 })), "nodelay 超 2 被拒")
        t:eq(false, (pcall(ck.start, ck, "127.0.0.1", SV_PORT, { mtu = 65508 })), "mtu 超 65507 被拒")
        t:eq(true, ck:start("127.0.0.1", SV_PORT, { nodelay = 2, mtu = 65507 }), "nodelay=2 与 mtu=65507 放行")
        ck:stop()
        -- 合法值照常放行,别把哨兵档一起卡死
        t:eq(true, ck:start("127.0.0.1", SV_PORT,
                            { nodelay = 1, interval = 10, resend = 2, nc = 1,
                              sndwnd = 32, rcvwnd = 128, mtu = 1400 }),
             "七个字段的合法值放行")
        ck:stop()
        t:eq(true, ck:start("127.0.0.1", SV_PORT), "config 不传仍走库默认")
        ck:stop()
    end

    -- server 收到数据原样 echo(异步走回调);client send 的响应由框架按 sess 唤醒协程,不进此回调。
    -- 回调拿不到 conv,ASYNC_MSG 按内容交给末尾那个 async 会话回
    local async_kcp
    local learn_kcp
    local learn_hits = 0
    srey.on_recvedfrom(function(pktype, sk, ip, port, data, size)
        if sk.fd == sv_sk.fd then
            if learn_kcp and #LEARN_MSG == size and LEARN_MSG == srey.ud_str(data, size) then
                learn_hits = learn_hits + 1
                learn_kcp:send(data, size, 1)
            elseif async_kcp and #ASYNC_MSG == size and ASYNC_MSG == srey.ud_str(data, size) then
                async_kcp:send(data, size, 1)
            else
                sv_kcp:send(data, size, 1)
            end
        end
    end)

    -- client 同步发送并校验 echo
    local rdata, rsize = cli_kcp:send(MSG, #MSG, 1)
    t:check(rdata ~= nil, "send 收到 echo")
    if rdata then
        t:eq(MSG, srey.ud_str(rdata, rsize), "echo 内容一致")
    end

    -- stop 后立即重启:每次 start 用新 sess,不会被上一会话在途的 CLOSE 击穿
    cli_kcp:stop()
    t:eq(true, cli_kcp:start("127.0.0.1", SV_PORT), "restart right after stop")

    -- sync 模式存活会话期间重复 start 由 Lua 侧守卫原地拒绝且不动 k.sess:C 侧虽会隐式 stop 旧会话
    -- 再起新的(不产生 stop 不掉的孤儿),但那会静默丢掉本对象正在等的会话,故不放行
    local live_sess = cli_kcp.sess
    t:eq(false, cli_kcp:start("127.0.0.1", SV_PORT), "存活会话期间 start 被拒")
    t:eq(live_sess, cli_kcp.sess, "被拒后 k.sess 不变")

    -- CLOSE 分支:send 挂起期间 socket 被关(_kcp_udfree 逐会话补 CLOSE),唤醒后须置停(live 为假),
    -- 否则下次 send 会通过守卫投到已消失的会话被静默丢弃,再空等满一个 netread 超时;
    -- C 句柄也须已 stop,否则 handle 绕过 stopped 守卫照样返 true。k.sess 保留,on_closed 拿它认会话
    do
        local dead_sk = srey.udp(PACK_TYPE.UDP_KCP, "0.0.0.0", DEAD_PORT)
        t:check(dead_sk and dead_sk.valid, "dead udp 创建")
        if dead_sk and dead_sk.valid then
            local dead_kcp = kcp.new(dead_sk, CONV, true)
            -- start 用默认超时:它自身也等 netread timeout,先收窄会让这一步变得超时敏感——
            -- 一旦它走 TIMEOUT 分支(self:stop() 顺手置停),下面几条断言会以错误理由通过
            local started = dead_kcp:start("127.0.0.1", NOBODY_PORT)
            t:eq(true, started, "dead kcp start")
            if started then
                local dsess = dead_kcp.sess
                local dskid = dead_sk.skid
                local saved_net = srey.get_netread_timeout()
                srey.set_netread_timeout(500)-- 仅为下面的 send 收窄:CLOSE 万一未到达也不卡满默认超时
                srey.fork(function()
                    srey.sleep(30)
                    srey.close(dead_sk)-- fork 协程在本条消息 dispatch 末尾才起,故必晚于下面的 send 挂起
                end)
                local bgts = srey.timer_ms()
                t:eq(nil, (dead_kcp:send(MSG, #MSG, 1)), "CLOSE 唤醒时 send 返回 nil")
                -- 必须由 CLOSE(约 30ms)唤醒:若落到 TIMEOUT 分支(500ms)则它的 self:stop() 也会置停,
                -- 下面那条断言就形同虚设,故先用耗时把路径钉死
                t:check(srey.timer_ms() - bgts < 300, "由 CLOSE 唤醒而非 TIMEOUT")
                t:eq(false, dead_kcp.live, "CLOSE 后已置停")
                t:eq(dsess, dead_kcp.sess, "CLOSE 后 k.sess 保留")
                t:eq(CLOSE_TYPE.LOCAL, wait_closed(dsess), "socket 关闭逐会话补的 CLOSE:on_closed 第 5 参是 k.sess")
                t:check(nil ~= wait_closed(dskid), "socket 本身关闭:on_closed 第 5 参是 skid")
                t:eq(false, dead_kcp:handle(srey.task_handle()), "CLOSE 后 handle 被 stopped 守卫拒绝")
                srey.set_netread_timeout(saved_net)
            else
                srey.close(dead_sk)-- start 失败:后续断言前提不成立,直接收尾免 socket 泄漏
            end
        end
    end

    -- 并发换会话:send 挂起期间另一协程 stop + 重启换上新会话，随后到达的是**旧**会话的 CLOSE
    -- （CLOSE 按 sess 广播给该 sess 下全部等待者，不分 mtype）。醒来的 send 若不先比对 sess 就 stop，
    -- 会把新会话停掉。stop 正好放开 ctx:start 的"存活期间不许重复 start"守卫，所以那道门挡不住这个时序
    do
        local race_sk = srey.udp(PACK_TYPE.UDP_KCP, "0.0.0.0", RACE_PORT)
        t:check(race_sk and race_sk.valid, "race udp 创建")
        if race_sk and race_sk.valid then
            local race_kcp = kcp.new(race_sk, CONV, true)
            local started = race_kcp:start("127.0.0.1", NOBODY_PORT)
            t:eq(true, started, "race kcp start")
            if started then
                local old_sess = race_kcp.sess
                local saved_net = srey.get_netread_timeout()
                srey.set_netread_timeout(500)
                local restarted
                srey.fork(function()
                    -- fork 协程在本条消息 dispatch 末尾才起，故必晚于下面 send 的挂起。
                    -- stop 投出旧会话的 CLOSE，紧接的 start 换上新会话并自己挂在 HANDSHAKED 上
                    race_kcp:stop()
                    restarted = race_kcp:start("127.0.0.1", NOBODY_PORT)
                end)
                local bgts = srey.timer_ms()
                t:eq(nil, (race_kcp:send(MSG, #MSG, 1)), "旧会话 CLOSE 唤醒时 send 返回 nil")
                -- 同上：必须由 CLOSE 唤醒。落到 TIMEOUT 分支的话它也带 sess 比对，
                -- 下面那条断言会以错误理由通过
                t:check(srey.timer_ms() - bgts < 300, "由旧会话 CLOSE 唤醒而非 TIMEOUT")
                t:check(race_kcp.live and old_sess ~= race_kcp.sess, "旧会话的 CLOSE 未停掉新会话")
                -- 另一协程的 start 是同步的,此刻可能还挂在 HANDSHAKED 上,等它落地再判
                for _ = 1, 20 do
                    if nil ~= restarted then break end
                    srey.sleep(50)
                end
                t:eq(true, restarted, "另一协程的重启成功")
                race_kcp:stop()
                srey.set_netread_timeout(saved_net)
            end
            srey.close(race_sk)
        end
    end

    -- async 重复 start 只报本地发起结果,恒为 true;隐式 stop 要靠回显看:第一次指向无人的 NOBODY_PORT,
    -- 第二次指向 client。没有隐式 stop 的话第二次因 conv 冲突被 event 线程拒掉,留下的旧会话按源地址
    -- 丢掉 client 的包,下面的 send 等满超时
    async_kcp = kcp.new(sv_sk, CONV + 1)
    t:eq(true, async_kcp:start("127.0.0.1", NOBODY_PORT), "async 模式 start")
    local first_sess = async_kcp.sess
    t:check(0 ~= first_sess, "async start 也生成非 0 sess")
    t:eq(true, async_kcp:start("127.0.0.1", CLI_PORT), "async 存活会话期间重复 start 返回 true")
    t:check(first_sess ~= async_kcp.sess, "async 重复 start 换上新 sess")
    local cli_async = kcp.new(cli_sk, CONV + 1, true)
    t:eq(true, cli_async:start("127.0.0.1", SV_PORT), "client 侧 CONV+1 start")
    local adata, asize = cli_async:send(ASYNC_MSG, #ASYNC_MSG, 1)
    t:eq(ASYNC_MSG, adata and srey.ud_str(adata, asize), "重复 start 后回显来自新会话(旧会话已被隐式 stop)")
    t:eq(CLOSE_TYPE.LOCAL, wait_closed(first_sess), "隐式 stop 的旧会话:on_closed 第 5 参是旧 sess")
    cli_async:stop()
    async_kcp:stop()
    t:eq(CLOSE_TYPE.LOCAL, wait_closed(async_kcp.sess), "async 会话 stop 后 on_closed 第 5 参等于 k.sess")

    -- 学习模式:server 以 port 0 起会话,对端从 OS 分配的临时端口先发,首包定下对端后双向照常;
    -- 之后另一个 socket 冒充同 conv 发来的被 C 层按源地址丢掉,到不了 on_recvedfrom,也顶不掉已学到的对端
    do
        local lcli_sk = srey.udp(PACK_TYPE.UDP_KCP)
        local intr_sk = srey.udp(PACK_TYPE.UDP_KCP)
        t:check(lcli_sk.valid and intr_sk.valid, "学习模式两端 udp 创建")
        if lcli_sk.valid and intr_sk.valid then
            learn_kcp = kcp.new(sv_sk, CONV + 3)
            t:eq(true, learn_kcp:start("127.0.0.1", 0), "port 0 起学习模式会话")
            local lcli = kcp.new(lcli_sk, CONV + 3, true)
            t:eq(true, lcli:start("127.0.0.1", SV_PORT), "学习模式对端 start")
            local ld, ls = lcli:send(LEARN_MSG, #LEARN_MSG, 1)
            t:eq(LEARN_MSG, ld and srey.ud_str(ld, ls), "首包学到临时端口后双向通信正常")
            local intr = kcp.new(intr_sk, CONV + 3, true)
            t:eq(true, intr:start("127.0.0.1", SV_PORT), "冒充者 start")
            local saved_net = srey.get_netread_timeout()
            srey.set_netread_timeout(500)
            t:eq(nil, (intr:send(LEARN_MSG, #LEARN_MSG, 1)), "绑定后换来源的包被丢")
            srey.set_netread_timeout(saved_net)
            ld, ls = lcli:send(LEARN_MSG, #LEARN_MSG, 1)
            t:eq(LEARN_MSG, ld and srey.ud_str(ld, ls), "冒充者没顶掉已学到的对端")
            t:eq(2, learn_hits, "server 只收到真对端的两条")
            lcli:stop()
            intr:stop()
            learn_kcp:stop()
        end
        for _, usk in ipairs({ lcli_sk, intr_sk }) do
            if usk.valid then
                srey.close(usk)
            end
        end
    end

    -- 句柄丢掉后由 __gc 兜底 stop:会话出表,同 conv 能再 start;__gc 没 stop 的话同 conv 撞冲突被拒
    local gck = kcp.new(sv_sk, CONV + 2, true)
    t:eq(true, gck:start("127.0.0.1", CLI_PORT), "待回收句柄 start")
    local weak = setmetatable({ gck.kcp }, { __mode = "v" })
    gck = nil
    collectgarbage()
    collectgarbage()
    t:eq(nil, weak[1], "kcp 句柄已被回收")
    local regc = kcp.new(sv_sk, CONV + 2, true)
    t:eq(true, regc:start("127.0.0.1", CLI_PORT), "回收后同 conv 可重新 start(__gc 兜底 stop)")
    regc:stop()

    -- 收尾:关 socket(会话随 socket 关闭释放)
    srey.close(cli_sk)
    srey.close(sv_sk)
end)
end)
