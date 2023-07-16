local sutils = require("srey.utils")
local core = require("lib.core")
local log = require("lib.log")
local syn = require("lib.synsl")
local dns_pool_ipv6 = {}
local dns_pool_ipv4 = {}
local fd = INVALID_SOCK
local skid = 0
local dns = {}

-- return 'ipv4', 'ipv6', or 'hostname'
local function host_type(host)
	if host:match("^[%d%.]+$") then
		return "ipv4"
	end
	if host:find(":") then
		return "ipv6"
	end
	return "hostname"
end
function dns.lookup(dnsip, domain, ipv6)
    if ipv6 then
        local ips = dns_pool_ipv6[domain]
        if ips then
            return ips
        end
    else
        local ips = dns_pool_ipv4[domain]
        if ips then
            return ips
        end
    end
    if INVALID_SOCK == fd then
        fd, skid = core.udp("0.0.0.0", 0)
        if INVALID_SOCK == fd then
            log.ERROR("dns coreate udp failed.")
            return {}
        end
    end
    local req = sutils.dns_pack(domain, ipv6 and 1 or 0)
    local resp,_ = syn.sendto(fd, skid, dnsip, DNS_PORT, req, #req)
    if resp then
        local ips = sutils.dns_unpack(resp)
        if ipv6 then
            dns_pool_ipv6[domain] = ips
        else
            dns_pool_ipv4[domain] = ips
        end
        return ips
    else
        log.ERROR("dns qury domain ip failed.")
        return {}
    end
end
function dns.qury(host, ipv6)
    if "hostname" == host_type(host) then
        local ips = dns.lookup(DNS_IP, host, ipv6)
        if 0 ~= #ips then
            return ips[1]
        else
            return nil
        end
    else
        return host
    end
end

return dns
