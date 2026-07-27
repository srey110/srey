-- KCP 绑定层测试(lkcp.c + kcp.lua):同一 task 内建 server / client 两个 UDP socket(PACK_UDP_KCP),
-- 两端约定固定 conv(不走 TCP 握手,聚焦绑定通路 + 数据往返)。
-- client send(同步) -> server on_recvedfrom echo -> sess 唤醒 client 协程 -> 校验 echo 内容。
-- ikcp_update 由 event 线程 tick 自动驱动,业务无需轮询。

local srey   = require("lib.srey")
local runner = require("test.runner")
local kcp    = require("lib.kcp")

local SV_PORT  = 15042
local CLI_PORT = 15043
local DEAD_PORT = 15044   -- 专供 CLOSE 分支用例的 socket
local NOBODY_PORT = 15045 -- 无人监听的对端,令 send 必然挂起等超时
local CONV     = 1
local MSG      = "kcp_hello_lua"

srey.startup(function()
runner.run("kcp", function(t)
    -- server / client 两个 UDP socket(不同端口,各自独立 conv 空间)
    local sv_fd, sv_skid = srey.udp(PACK_TYPE.UDP_KCP, "0.0.0.0", SV_PORT)
    t:check(sv_fd and sv_fd ~= INVALID_SOCK, "server udp 创建")
    if not sv_fd or sv_fd == INVALID_SOCK then return end
    local cli_fd, cli_skid = srey.udp(PACK_TYPE.UDP_KCP, "0.0.0.0", CLI_PORT)
    t:check(cli_fd and cli_fd ~= INVALID_SOCK, "client udp 创建")
    if not cli_fd or cli_fd == INVALID_SOCK then
        srey.close(sv_fd, sv_skid)
        return
    end

    -- 两端 kcp 会话:conv 一致,互指对方端口;server 只异步 echo,client 要同步等响应
    local sv_kcp = kcp.new(sv_fd, sv_skid, CONV)
    t:eq(true, sv_kcp:start("127.0.0.1", CLI_PORT), "server kcp start")
    local cli_kcp = kcp.new(cli_fd, cli_skid, CONV, true)
    t:eq(true, cli_kcp:start("127.0.0.1", SV_PORT), "client kcp start")

    -- 同一底层 socket 上用相同 conv 再注册一次,同步 start 应因 conv 冲突返回 false
    local dup_kcp = kcp.new(cli_fd, cli_skid, CONV, true)
    t:eq(false, dup_kcp:start("127.0.0.1", SV_PORT), "duplicate conv start should fail")
    -- start 失败后 sess 为 0:sync send 须立即返回 nil(而非进 _coro_wait 空等满 netread 超时);
    -- copy=0 走 _ud_free_copy 兜底,string 传入时 utils.ud_free 内部跳过,验证不误释放也不崩
    t:eq(nil, (dup_kcp:send(MSG, #MSG, 0)), "sess==0 send 返回 nil(copy=0)")
    t:eq(nil, (dup_kcp:send(MSG, #MSG, 1)), "sess==0 send 返回 nil(copy=1)")

    -- server 收到数据原样 echo(异步走回调);client send 的响应由框架按 sess 唤醒协程,不进此回调
    srey.on_recvedfrom(function(pktype, fd, skid, ip, port, data, size)
        if fd == sv_fd then
            sv_kcp:send(data, size, 1)
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

    -- sync 模式存活会话期间重复 start 由 Lua 侧守卫原地拒绝且不动 self.sess:C 侧虽会隐式 stop 旧会话
    -- 再起新的(不产生 stop 不掉的孤儿),但那会静默丢掉本对象正在等的会话,故不放行
    t:eq(false, cli_kcp:start("127.0.0.1", SV_PORT), "存活会话期间 start 被拒")
    t:check(cli_kcp.sess ~= 0, "被拒后 self.sess 未被清零")

    -- CLOSE 分支:send 挂起期间 socket 被关(_kcp_udfree 逐会话补 CLOSE),唤醒后须清 self.sess,
    -- 否则下次 send 会通过 sess 守卫投到已消失的会话被静默丢弃,再空等满一个 netread 超时
    do
        local dead_fd, dead_skid = srey.udp(PACK_TYPE.UDP_KCP, "0.0.0.0", DEAD_PORT)
        t:check(dead_fd and dead_fd ~= INVALID_SOCK, "dead udp 创建")
        if dead_fd and dead_fd ~= INVALID_SOCK then
            local dead_kcp = kcp.new(dead_fd, dead_skid, CONV, true)
            -- start 用默认超时:它自身也等 netread timeout,先收窄会让这一步变得超时敏感——
            -- 一旦它走 TIMEOUT 分支(self:stop() 顺手清了 sess),下面三条断言会全部以错误理由通过
            local started = dead_kcp:start("127.0.0.1", NOBODY_PORT)
            t:eq(true, started, "dead kcp start")
            if started then
                local saved_net = srey.get_netread_timeout()
                srey.set_netread_timeout(500)-- 仅为下面的 send 收窄:CLOSE 万一未到达也不卡满默认超时
                srey.fork(function()
                    srey.sleep(30)
                    srey.close(dead_fd, dead_skid)-- fork 协程在本条消息 dispatch 末尾才起,故必晚于下面的 send 挂起
                end)
                local bgts = srey.timer_ms()
                t:eq(nil, (dead_kcp:send(MSG, #MSG, 1)), "CLOSE 唤醒时 send 返回 nil")
                -- 必须由 CLOSE(约 30ms)唤醒:若落到 TIMEOUT 分支(500ms)则它的 self:stop() 也会清 sess,
                -- 下面那条断言就形同虚设,故先用耗时把路径钉死
                t:check(srey.timer_ms() - bgts < 300, "由 CLOSE 唤醒而非 TIMEOUT")
                t:eq(0, dead_kcp.sess, "CLOSE 后 self.sess 已清")
                srey.set_netread_timeout(saved_net)
            else
                srey.close(dead_fd, dead_skid)-- start 失败:后续断言前提不成立,直接收尾免 socket 泄漏
            end
        end
    end

    local async_kcp = kcp.new(sv_fd, sv_skid, CONV + 1)
    t:eq(true, async_kcp:start("127.0.0.1", CLI_PORT), "async 模式 start")
    t:eq(true, async_kcp:start("127.0.0.1", CLI_PORT), "async 存活会话期间重复 start 仍成功(C 侧隐式 stop 旧会话)")
    async_kcp:stop()

    -- 收尾:关 socket(会话随 socket 关闭释放;kcp 句柄 GC 时 __gc 兜底 stop)
    srey.close(cli_fd, cli_skid)
    srey.close(sv_fd, sv_skid)
end)
end)
