-- 本地假 DNS 服务端 + nslookup 用例：lib/dns.lua 的用例不再依赖公网（8.8.8.8 与真实域名）。
-- 同一 task 内起 UDP 与 TCP(PACK_TYPE.DNS) 两个监听，端口号相同，经 nslookup 的测试参数 ip / port 指过来。
-- 按查询名分流：ok 正常应答；nodata 应答段为空(NOERROR/NODATA)；nx 回 NXDOMAIN；tc 置 TC 位；spoof 从另一个端口回一条
-- 事务 ID 正确的伪应答。UDP 与 TCP 给不同的地址，看结果就知道走的是哪条路；另数两边收到的查询数。
-- 服务端回调与查询协程同属一个 task，单线程依次执行，计数用普通变量即可。
-- 注册：task.register("test.dns_fake", "dns_fake", 0, port)

local srey   = require("lib.srey")
local runner = require("test.runner")
require("lib.dns")

local _PORT = ... or 12553
local IP = "127.0.0.1"
local FLAGS = 0x8180-- QR | RD | RA，RCODE 0
local FLAG_TC = 0x0200
local UDP_A = "10.0.0.1"-- UDP 应答给的地址
local TCP_A = "10.0.0.2"-- TCP 应答给的地址
local SPOOF_A = "10.0.0.6"-- 伪应答给的地址
local nudp, ntcp, nspoof = 0, 0, 0-- 服务端收到的 UDP / TCP 查询数、发出的伪应答数

-- 取查询名(点分)与问题段末字节的下标(QTYPE / QCLASS 之后)
local function _question(q)
    local pos, labels = 13, {}
    local n = q:byte(pos)
    while n and n > 0 do
        labels[#labels + 1] = q:sub(pos + 1, pos + n)
        pos = pos + 1 + n
        n = q:byte(pos)
    end
    return table.concat(labels, "."), pos + 4
end
-- 组应答：事务 ID 与问题段照抄；ip 非 nil 时带一条 A 记录，名字用指针指回问题段
local function _reply(q, qend, flags, ip)
    local an = ""
    if ip then
        local a, b, c, d = ip:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
        an = string.pack(">I2I2I2I4I2BBBB", 0xC00C, 1, 1, 60, 4,
                         tonumber(a), tonumber(b), tonumber(c), tonumber(d))
    end
    return q:sub(1, 2) .. string.pack(">I2I2I2I2I2", flags, 1, ip and 1 or 0, 0, 0) .. q:sub(13, qend) .. an
end

srey.startup(function()
runner.run(function(t)
    -- 监听守卫在 run 体内，理由同 smtp_fake
    local usk = srey.udp(PACK_TYPE.NONE, IP, _PORT)
    local ssk = srey.udp(PACK_TYPE.NONE, IP, 0)-- 发伪应答用，端口由系统分配
    local lid = srey.listen(PACK_TYPE.DNS, SSL_NAME.NONE, IP, _PORT)
    if not usk.valid or not ssk.valid or ERR_FAILED == lid then
        t:fail("fake dns: udp / listen " .. _PORT .. " failed")
        return
    end
    srey.on_recvedfrom(function(_, sk, ip, port, data, size)
        if sk.skid ~= usk.skid then
            return
        end
        nudp = nudp + 1
        local q = srey.ud_str(data, size)
        local name, qend = _question(q)
        local r
        if "ok.test" == name then
            r = _reply(q, qend, FLAGS, UDP_A)
        elseif "tc.test" == name then
            r = _reply(q, qend, FLAGS | FLAG_TC)
        elseif "nx.test" == name then
            r = _reply(q, qend, FLAGS | 3)-- RCODE 3(NXDOMAIN)，应答段为空
        elseif "sf.test" == name then
            r = _reply(q, qend, FLAGS | 2)-- RCODE 2(SERVFAIL)，应答段为空
        elseif "spoof.test" == name then
            nspoof = nspoof + 1
            r = _reply(q, qend, FLAGS, SPOOF_A)
            srey.sendto(ssk, ip, port, r, #r, 1)
            return
        else
            r = _reply(q, qend, FLAGS)-- nodata.test：应答段为空
        end
        srey.sendto(sk, ip, port, r, #r, 1)
    end)
    -- TCP 一律回 TCP_A，前面带 2 字节大端长度
    srey.on_recved(function(pktype, sk, client, _, data, size)
        if 0 ~= client or PACK_TYPE.DNS ~= pktype then
            return
        end
        ntcp = ntcp + 1
        local q = srey.ud_str(data, size)
        local _, qend = _question(q)
        local r = string.pack(">s2", _reply(q, qend, FLAGS, TCP_A))
        srey.send(sk, r, #r, 1)
    end)

    -- { 查询名, udp 参数, 期望的首个地址, 期望新增的 UDP 查询数, 期望新增的 TCP 查询数, 说明 }
    local cases = {
        { "ok.test", true, UDP_A, 1, 0, "UDP 正常应答，不碰 TCP" },
        { "ok.test", false, TCP_A, 0, 1, "udp 省略时只走 TCP" },
        { "nodata.test", true, nil, 1, 0, "NODATA 不回退 TCP" },
        { "tc.test", true, TCP_A, 1, 1, "TC 置位回退 TCP" },
        { "nx.test", true, nil, 1, 0, "NXDOMAIN 是明确答复，不回退 TCP" },
        { "sf.test", true, TCP_A, 1, 1, "SERVFAIL 可能是暂时故障，回退 TCP" },
        { "spoof.test", true, TCP_A, 1, 1, "来源端口不对的伪应答被丢弃，回退 TCP" },
        { "a..b", true, nil, 0, 0, "非法域名组包失败，不发查询" },
    }
    local u0, t0, ips
    for _, c in ipairs(cases) do
        u0, t0 = nudp, ntcp
        ips = nslookup(c[1], false, c[2], IP, _PORT)
        t:eq(c[3], ips and ips[1], c[6])
        t:eq(c[3] and 1 or nil, ips and #ips, c[6] .. "：地址个数")
        t:eq(c[4], nudp - u0, c[6] .. "：UDP 查询数")
        t:eq(c[5], ntcp - t0, c[6] .. "：TCP 查询数")
    end
    t:eq(1, nspoof, "伪应答确实发出过")

    srey.close(usk)
    srey.close(ssk)
    srey.unlisten(lid)
end)
end)
