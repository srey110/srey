-- DNS 解析工具：通过 UDP / TCP 向配置的 DNS 服务器发起 A/AAAA 查询。
-- 依赖：lib.srey（UDP/TCP 收发）、srey.dns（C 层 DNS 报文打包/解包）。

require("lib.log")
local srey = require("lib.srey")
local dns = require("srey.dns")
local MSG_RECVFROM = srey.MSG_TYPE.RECVFROM

-- DNS 服务器 IP：取 config.json 的 dns 字段（缺省 8.8.8.8）。
local dns_ip = dns.ip()

---通过 UDP 查询 domain 的 IP 地址列表；失败、无结果、响应被截断(TC 位置位)或应答不是 ip:port 发来的
---都返回 nil，供上层回退 TCP
---@param domain string 待解析的域名
---@param ipv6 boolean true 时查询 AAAA 记录，否则 A 记录
---@param ip string DNS 服务器 IP
---@param port integer DNS 服务器端口
---@return string[]|nil ips IP 字符串数组；失败、无结果或响应被截断时返回 nil
---@return boolean? nodata 第二返回值：true 表示服务端已明确答复"无此记录"或"名字不存在"(NXDOMAIN)，回退 TCP 也是同一结果
local function nslookup_udp(domain, ipv6, ip, port)
    -- 先组包再建 socket：domain 非字符串或含 NUL 时组包直接抛错，此时还没有要关的 socket
    local req, id = dns.pack(domain, ipv6 and 1 or 0)
    if not req then
        return nil
    end
    -- 根据 DNS 服务器类型创建对应的 UDP socket（IPv6 本地地址为 "::"）
    local sk
    if "ipv6" == host_type(ip) then
        sk = srey.udp(PACK_TYPE.NONE, "::", 0)
    else
        sk = srey.udp(PACK_TYPE.NONE)
    end
    if not sk.valid then
        WARN("init udp error.")
        return nil
    end
    -- 不走 srey.syn_sendto：它只交出载荷，这里要核对应答来源
    if not srey.sock_session(sk) or not srey.sendto(sk, ip, port, req, #req, 1) then
        srey.close(sk)
        return nil
    end
    local msg = srey._coro_wait(sk.skid, MSG_RECVFROM, srey.get_netread_timeout())
    srey.close(sk)
    -- 超时、断开、来源不是 ip:port 都按失败回退 TCP(同 TC 位)。来源按串比，配置的 IPv6 不是规范写法时一律走 TCP
    if MSG_RECVFROM ~= msg.mtype then
        return nil
    end
    local fip, fport = srey.remote_addr(msg.addr)
    if ip ~= fip or port ~= fport then
        return nil
    end
    -- 解包响应，提取 IP 地址列表；TC 位置位时 dns.unpack 内部已按失败处理返回 nil
    return dns.unpack(msg.udata, msg.size, id)
end

---通过 TCP 查询 domain 的 IP 地址列表（RFC 1035 §4.2.2 / RFC 7766），适用于响应超 512 字节或 UDP 被屏蔽场景
---@param domain string 待解析的域名
---@param ipv6 boolean true 时查询 AAAA 记录，否则 A 记录
---@param ip string DNS 服务器 IP
---@param port integer DNS 服务器端口
---@return string[]|nil ips IP 字符串数组；失败或无结果时返回 nil
local function nslookup_tcp(domain, ipv6, ip, port)
    -- 先组包再连接，理由同 nslookup_udp
    local req, id = dns.pack_tcp(domain, ipv6 and 1 or 0)
    if not req then
        return nil
    end
    local sk = srey.connect(PACK_TYPE.DNS, SSL_NAME.NONE, ip, port)
    if not sk.valid then
        return nil
    end
    -- C 层 dns_unpack 已剥离 2 字节长度前缀，resp 即裸 DNS 报文，可直接走 dns.unpack
    local resp, resplens = srey.syn_send(sk, req, #req, 1)
    srey.close(sk)
    if not resp then
        return nil
    end
    local ips = dns.unpack(resp, resplens, id)
    return ips
end

---查询 domain 的 IP 地址列表：udp=true 时先 UDP、失败回退 TCP；否则（默认）直接 TCP
---@param domain string 待解析的域名
---@param ipv6 boolean true 时查询 AAAA 记录，否则 A 记录
---@param udp boolean? true 时优先 UDP（失败回退 TCP），否则（默认）仅 TCP
---@param ip string? DNS 服务器 IP，缺省取配置；与 port 一样只给测试指向本地假服务端用
---@param port integer? DNS 服务器端口，缺省 53
---@return string[]|nil ips IP 字符串数组；失败或无结果时返回 nil。
---服务端明确答复"无此记录"（NOERROR/NODATA）或名字不存在（NXDOMAIN）时不再回退 TCP，与 C 侧 dns_lookup 行为一致
function nslookup(domain, ipv6, udp, ip, port)
    srey._check_wait("nslookup")-- 先查：UDP 那支建 socket、发查询都在挂起之前
    ip = ip or dns_ip
    port = port or 53
    if udp then
        local ips, nodata = nslookup_udp(domain, ipv6, ip, port)
        if ips then
            return ips
        end
        -- 服务端已明确答复"该域名没有这种记录"，换 TCP 重查拿到的是同一个答复，白跑一趟
        if nodata then
            return nil
        end
    end
    return nslookup_tcp(domain, ipv6, ip, port)
end
