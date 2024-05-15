require("lib.log")
local srey = require("lib.srey")
local dns = require("srey.dns")
local dns_ip = dns.ip()
local isipv6 = ("ipv6" == host_type(dns_ip))

function nslookup(domain, ipv6)
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
    local req = dns.pack(domain, ipv6 and 1 or 0)
    local resp, _ = srey.syn_sendto(fd, skid, dns_ip, 53, req, #req)
    srey.close(fd, skid)
    if not resp then
        return nil
    end
    return dns.unpack(resp)
end
