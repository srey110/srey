local srey = require("lib.srey")
local simple = require("srey.simple")

local function _timeout()
    local fd,skid = srey.syn_connect(PACK_TYPE.SIMPLE, 0, "127.0.0.1", 15000, 1000)
    assert(INVALID_SOCK ~= fd)
    local sdata, ssize = simple.pack("this is syn send test.")
    local rdata, rsize = srey.syn_send(fd, skid, srey.id(), sdata, ssize, 1000, 0)
    assert(rdata)
    sdata, ssize = simple.unpack(rdata, rsize)
    assert(srey.hex("this is syn send test.") == srey.hex(sdata, ssize))
    srey.close(fd, skid)

    fd,skid = srey.udp()
    assert(INVALID_SOCK ~= fd)
    local udpstr = "this is syn sendto test."
    rdata, rsize =  srey.syn_sendto(fd, skid, "127.0.0.1", 15003, udpstr, #udpstr, 1000)
    assert(rdata and srey.hex(udpstr) == srey.hex(rdata, rsize))
    srey.close(fd, skid)
    srey.timeout(1000, _timeout)
end
srey.startup(
    function ()
        srey.timeout(1000, _timeout)
    end
)
