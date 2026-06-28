-- DNS 解析工具：通过 UDP / TCP 向系统默认 DNS 服务器发起 A/AAAA 查询。
-- 依赖：lib.srey（UDP/TCP 收发）、srey.dns（C 层 DNS 报文打包/解包）。

require("lib.log")
local srey = require("lib.srey")
local dns = require("srey.dns")

-- 获取系统默认 DNS 服务器 IP（由 C 层 srey.dns.ip() 读取 /etc/resolv.conf 或 Windows 注册表）。
local dns_ip = dns.ip()
-- 根据 DNS 服务器地址判断是否需要使用 IPv6 UDP socket。
local isipv6 = ("ipv6" == host_type(dns_ip))

---通过 UDP 查询 domain 的 IP 地址列表
---@param domain string 待解析的域名
---@param ipv6 boolean true 时查询 AAAA 记录，否则 A 记录
---@return string[]|nil ips IP 字符串数组；失败或无结果时返回 nil
local function nslookup_udp(domain, ipv6)
    -- 根据 DNS 服务器类型创建对应的 UDP socket（IPv6 本地地址为 "::"）
    local fd, skid
    if isipv6 then
        fd, skid = srey.udp("::", 0)
    else
        fd, skid = srey.udp()
    end
    if INVALID_SOCK == fd then
        WARN("init udp error.")
        return nil
    end
    -- 打包 DNS 查询报文并同步发送到 DNS 服务器的 53 端口，等待响应
    local req = dns.pack(domain, ipv6 and 1 or 0)
    local resp, resplens = srey.syn_sendto(fd, skid, dns_ip, 53, req, #req, 1)
    srey.close(fd, skid)
    if not resp then
        return nil
    end
    -- 解包响应，提取 IP 地址列表
    return dns.unpack(resp, resplens)
end

---通过 TCP 查询 domain 的 IP 地址列表（RFC 1035 §4.2.2 / RFC 7766），适用于响应超 512 字节或 UDP 被屏蔽场景
---@param domain string 待解析的域名
---@param ipv6 boolean true 时查询 AAAA 记录，否则 A 记录
---@return string[]|nil ips IP 字符串数组；失败或无结果时返回 nil
local function nslookup_tcp(domain, ipv6)
    local fd, skid = srey.connect(PACK_TYPE.DNS, SSL_NAME.NONE, dns_ip, 53)
    if INVALID_SOCK == fd then
        return nil
    end
    local req = dns.pack_tcp(domain, ipv6 and 1 or 0)
    if not req then
        srey.close(fd, skid)
        return nil
    end
    -- C 层 dns_unpack 已剥离 2 字节长度前缀，resp 即裸 DNS 报文，可直接走 dns.unpack
    local resp, resplens = srey.syn_send(fd, skid, req, #req, 1)
    srey.close(fd, skid)
    if not resp then
        return nil
    end
    return dns.unpack(resp, resplens)
end

---查询 domain 的 IP 地址列表：udp=true 时先 UDP、失败回退 TCP；否则（默认）直接 TCP
---@param domain string 待解析的域名
---@param ipv6 boolean true 时查询 AAAA 记录，否则 A 记录
---@param udp boolean? true 时优先 UDP（失败回退 TCP），否则（默认）仅 TCP
---@return string[]|nil ips IP 字符串数组；失败或无结果时返回 nil
function nslookup(domain, ipv6, udp)
    if udp then
        local ips = nslookup_udp(domain, ipv6)
        if ips then
            return ips
        end
    end
    return nslookup_tcp(domain, ipv6)
end
