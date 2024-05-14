local srey = require("lib.srey")
local dns = require("srey.dns")

function nslookup(dnsip, domain, ipv6, ms)
    local fd, skid
    if "ipv6" == host_type(dnsip) then
        fd, skid = srey.udp("::", 0)
    else
        fd, skid = srey.udp()
    end
    if INVALID_SOCK == fd then
        return {}
    end
    if not ms then
        ms = 3000
    end
    local req = dns.pack(domain, ipv6 and 1 or 0)
    local resp, _ = srey.syn_sendto(fd, skid, dnsip, 53, req, #req, ms)
    srey.close(fd, skid)
    if not resp then
        return {}
    end
    return dns.unpack(resp)
end
