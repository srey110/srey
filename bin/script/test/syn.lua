local srey = require("lib.srey")
local custz = require("srey.custz")
require("lib.dns")

local function _timeout()
    local fd, skid = srey.connect(PACK_TYPE.CUSTZ, 0, "127.0.0.1", 15000)
    assert(INVALID_SOCK ~= fd)
    local sdata, ssize = custz.pack("this is syn send test.")
    local rdata, rsize = srey.syn_send(fd, skid, sdata, ssize, 0)
    assert(rdata)
    sdata, ssize = custz.unpack(rdata, rsize)
    assert(srey.hex("this is syn send test.") == srey.hex(sdata, ssize))
    srey.close(fd, skid)

    fd,skid = srey.udp()
    assert(INVALID_SOCK ~= fd)
    local udpstr = "this is syn sendto test."
    rdata, rsize =  srey.syn_sendto(fd, skid, "127.0.0.1", 15002, udpstr, #udpstr)
    assert(rdata and srey.hex(udpstr) == srey.hex(rdata, rsize))
    srey.close(fd, skid)
    srey.timeout(1000, _timeout)
end
srey.startup(
    function ()
        printd("nslookup www.google.com:")
        local ips = nslookup("www.google.com", false)
        printd(dump(ips))
        ips = nslookup("www.google.com", true)
        printd(dump(ips))
        srey.timeout(1000, _timeout)
    end
)
