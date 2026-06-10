-- srey.udp_join/udp_leave/udp_ttl/udp_loop 多播绑定层测试：
-- 验证 4 个 setsockopt 路径不崩 + UDP socket 单播 loopback 收发正常。
-- 多播实际 loopback 行为跨 OS 差异较大,本测试不验证多播传输,只验证 API 调用路径。

local srey   = require("lib.srey")
local runner = require("test.runner")

local PORT = 15015
local GROUP = "239.99.99.98"
local UNI_MSG = "UNI_LUA"

srey.startup(function()
runner.run("udp_multicast", function(t)
    local received = 0
    srey.on_recvedfrom(function(fd, skid, ip, port, data, size)
        if size == #UNI_MSG then
            local s = srey.ud_str(data, size)
            if s == UNI_MSG then
                received = received + 1
            end
        end
    end)
    local fd, skid = srey.udp("0.0.0.0", PORT)
    t:check(fd and fd ~= INVALID_SOCK, "udp create 成功")
    if not fd or fd == INVALID_SOCK then return end

    -- 4 个多播 API 路径验证
    t:eq(true, srey.udp_ttl(fd, skid, 1), "udp_ttl 返回 true")
    t:eq(true, srey.udp_loop(fd, skid, 1), "udp_loop 返回 true")
    t:eq(true, srey.udp_join(fd, skid, GROUP), "udp_join 返回 true")
    srey.sleep(200)  -- 等 4 cmd 投递到事件线程执行 setsockopt

    -- 单播 loopback 验证 recvfrom 路径
    t:eq(true, srey.sendto(fd, skid, "127.0.0.1", PORT, UNI_MSG, #UNI_MSG, 1), "sendto unicast 自己")
    for _ = 1, 40 do
        srey.sleep(50)
        if received >= 1 then break end
    end
    t:check(received >= 1, "unicast 自收(" .. received .. "/1+)")

    t:eq(true, srey.udp_leave(fd, skid, GROUP), "udp_leave 返回 true")
    srey.sleep(50)
    srey.close(fd, skid)
end)
end)
