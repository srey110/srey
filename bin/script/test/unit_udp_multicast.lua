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
    srey.on_recvedfrom(function(pktype, fd, skid, ip, port, data, size)
        if size == #UNI_MSG then
            local s = srey.ud_str(data, size)
            if s == UNI_MSG then
                received = received + 1
            end
        end
    end)
    local fd, skid = srey.udp(PACK_TYPE.NONE, "0.0.0.0", PORT)
    t:check(fd and fd ~= INVALID_SOCK, "udp create 成功")
    if not fd or fd == INVALID_SOCK then return end

    -- 4 个多播 API 路径验证
    t:eq(true, srey.udp_ttl(fd, skid, 1), "udp_ttl 返回 true")
    t:eq(true, srey.udp_ttl(fd, skid, 255), "udp_ttl 上界 255 合法")
    -- 0 是 host-local 作用域(只到本机)，合法值，不能因为要挡 256 就连它一起拒
    t:eq(true, srey.udp_ttl(fd, skid, 0), "udp_ttl 0(仅本机)合法")
    -- 直接窄化到 uint8_t 的话 256 会静默变成 0，多播从此出不了本机——是语义反转而非"值不对"
    t:eq(false, pcall(function() srey.udp_ttl(fd, skid, 256) end), "udp_ttl 256 抛 error")
    t:eq(false, pcall(function() srey.udp_ttl(fd, skid, -1) end),  "udp_ttl 负值抛 error")
    t:eq(true, srey.udp_ttl(fd, skid, 1), "还原 TTL 1 供后续用例")
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
