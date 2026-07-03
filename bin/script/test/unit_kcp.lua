-- KCP 绑定层测试(lkcp.c + kcp.lua):同一 task 内建 server / client 两个 UDP socket(PACK_UDP_KCP),
-- 两端约定固定 conv(不走 TCP 握手,聚焦绑定通路 + 数据往返)。
-- client send(同步) -> server on_recvedfrom echo -> sess 唤醒 client 协程 -> 校验 echo 内容。
-- ikcp_update 由 event 线程 tick 自动驱动,业务无需轮询。

local srey   = require("lib.srey")
local runner = require("test.runner")
local kcp    = require("lib.kcp")

local SV_PORT  = 15042
local CLI_PORT = 15043
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

    -- 两端 kcp 会话:conv 一致,互指对方端口;server 只异步 echo(sess=0),client 要同步等响应(sess≠0)
    local sv_kcp = kcp.new(sv_fd, sv_skid, CONV)
    t:eq(true, sv_kcp:start("127.0.0.1", CLI_PORT), "server kcp start")
    local cli_kcp = kcp.new(cli_fd, cli_skid, CONV, srey.id())
    t:eq(true, cli_kcp:start("127.0.0.1", SV_PORT), "client kcp start")

    -- 同一底层 socket 上用相同 conv 再注册一次,同步 start 应因 conv 冲突返回 false
    local dup_kcp = kcp.new(cli_fd, cli_skid, CONV, srey.id())
    t:eq(false, dup_kcp:start("127.0.0.1", SV_PORT), "duplicate conv start should fail")

    -- server 收到数据原样 echo(sess=0 走回调);client send 响应 sess≠0 由框架唤醒协程,不进此回调
    srey.on_recvedfrom(function(fd, skid, ip, port, data, size)
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

    -- 收尾:关 socket(会话随 socket 关闭释放;kcp 句柄 GC 时 __gc 兜底 stop)
    srey.close(cli_fd, cli_skid)
    srey.close(sv_fd, sv_skid)
end)
end)
