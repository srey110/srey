-- srey.udp_join/udp_leave/udp_ttl/udp_loop 多播绑定层测试：
-- 验证 4 个 setsockopt 路径不崩 + UDP socket 单播 loopback 收发正常。
-- 多播实际 loopback 行为跨 OS 差异较大,本测试不验证多播传输,只验证 API 调用路径。

local srey   = require("lib.srey")
local runner = require("test.runner")

local PORT = 15015
local EXCL_PORT = 15016 -- 端口复用默认关的用例
local GROUP = "239.99.99.98"
local UNI_MSG = "UNI_LUA"

srey.startup(function()
runner.run(function(t)
    local received = 0
    local from_ip, from_port
    srey.on_recvedfrom(function(pktype, sk, addr, data, size)
        if size == #UNI_MSG then
            local s = srey.ud_str(data, size)
            if s == UNI_MSG then
                received = received + 1
                from_ip, from_port = srey.remote_addr(addr)-- 回调只给原样地址，要串自己转
            end
        end
    end)
    -- 多播接收端按需打开端口复用
    local sk = srey.udp(PACK_TYPE.NONE, "0.0.0.0", PORT, 1)
    t:check(sk and sk.valid, "udp create 成功")
    if not sk or not sk.valid then return end

    -- 端口复用默认关：不带 reuse 的绑不上已被占的端口，先绑的没带 reuse 时后来带了也绑不上
    local dup = srey.udp(PACK_TYPE.NONE, "0.0.0.0", PORT)
    t:eq(false, dup.valid, "不带 reuse 绑不上共用中的端口")
    local dup2 = srey.udp(PACK_TYPE.NONE, "0.0.0.0", PORT, 1)
    t:check(dup2.valid, "都带 reuse 能共用端口")
    local ex = srey.udp(PACK_TYPE.NONE, "127.0.0.1", EXCL_PORT)
    t:check(ex.valid, "默认独占绑定成功")
    local ex2 = srey.udp(PACK_TYPE.NONE, "127.0.0.1", EXCL_PORT, 1)
    t:eq(false, ex2.valid, "独占的端口带 reuse 也绑不上")
    for _, s in ipairs({ dup, dup2, ex, ex2 }) do
        if s.valid then
            srey.close(s)
        end
    end
    t:eq(false, pcall(srey.udp, PACK_TYPE.NONE, "0.0.0.0", 0, 2), "reuse 只许 0 / 1")

    -- 4 个多播 API 路径验证
    t:eq(true, srey.udp_ttl(sk, 1), "udp_ttl 返回 true")
    t:eq(true, srey.udp_ttl(sk, 255), "udp_ttl 上界 255 合法")
    -- 0 是 host-local 作用域(只到本机)，合法值，不能因为要挡 256 就连它一起拒
    t:eq(true, srey.udp_ttl(sk, 0), "udp_ttl 0(仅本机)合法")
    -- 直接窄化到 uint8_t 的话 256 会静默变成 0，多播从此出不了本机——是语义反转而非"值不对"
    t:eq(false, pcall(function() srey.udp_ttl(sk, 256) end), "udp_ttl 256 抛 error")
    -- 报错要指向第 2 个参数(ttl)：下标写错会报 "bad argument #3"，按报错去查只会扑空
    local _, ttlerr = pcall(srey.udp_ttl, sk, 256)
    t:check(nil ~= string.find(tostring(ttlerr), "#2", 1, true), "udp_ttl 越界报错指向第 2 个参数")
    t:eq(false, pcall(function() srey.udp_ttl(sk, -1) end),  "udp_ttl 负值抛 error")
    t:eq(true, srey.udp_ttl(sk, 1), "还原 TTL 1 供后续用例")
    t:eq(true, srey.udp_loop(sk, 1), "udp_loop 返回 true")
    t:eq(true, srey.udp_join(sk, GROUP), "udp_join 返回 true")
    -- 组地址与 socket 不同族属调用方契约违反,在调用方线程就该被拒(_ev_udp_group),
    -- 不能等到事件线程只落一条日志——那样业务判不出自己根本没加进组
    t:eq(false, srey.udp_join(sk, "ff02::1"), "IPv6 组加到 0.0.0.0 socket 上返 false")
    t:eq(false, srey.udp_leave(sk, "ff02::1"), "leave 同样按同族判定拒绝")
    t:eq(false, srey.udp_join(sk, "not-an-ip"), "非法组地址返 false")
    srey.sleep(200) -- 等 4 cmd 投递到事件线程执行 setsockopt

    -- 单播 loopback 验证 recvfrom 路径
    t:eq(true, srey.sendto(sk, "127.0.0.1", PORT, UNI_MSG, #UNI_MSG, 1), "sendto unicast 自己")
    for _ = 1, 40 do
        srey.sleep(50)
        if received >= 1 then break end
    end
    t:check(received >= 1, "unicast 自收(" .. received .. "/1+)")
    t:eq("127.0.0.1", from_ip, "remote_addr 取到的源 IP")
    t:eq(PORT, from_port, "remote_addr 取到的源端口(自己发给自己)")

    t:eq(true, srey.udp_leave(sk, GROUP), "udp_leave 返回 true")
    srey.sleep(50)
    srey.close(sk)
end)
end)
