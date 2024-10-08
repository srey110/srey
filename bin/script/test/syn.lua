local srey = require("lib.srey")
local custz = require("srey.custz")
require("lib.dns")

local function _timeout()
    local fd, skid = srey.connect(PACK_TYPE.CUSTZ, SSL_NAME.NONE, "127.0.0.1", 15000)
    assert(INVALID_SOCK ~= fd)
    local sdata, ssize = custz.pack("this is syn send test.")
    local rdata, rsize = srey.syn_send(fd, skid, sdata, ssize, 0)
    assert(rdata)
    assert(srey.hex("this is syn send test.") == srey.hex(rdata, rsize))
    srey.close(fd, skid)

    fd, skid = srey.connect(PACK_TYPE.CUSTZ, SSL_NAME.NONE, "127.0.0.1", 15001, NET_EV.AUTHSSL)
    assert(INVALID_SOCK ~= fd)
    local auth = srey.syn_ssl_exchange(fd, skid, 1, SSL_NAME.CLIENT)
    assert(auth)
    sdata, ssize = custz.pack("this is ssl syn send test.")
    rdata, rsize = srey.syn_send(fd, skid, sdata, ssize, 0)
    assert(rdata)
    assert(srey.hex("this is ssl syn send test.") == srey.hex(rdata, rsize))
    srey.close(fd, skid)

    fd, skid = srey.connect(PACK_TYPE.CUSTZ, SSL_NAME.CLIENT, "127.0.0.1", 15001, NET_EV.AUTHSSL)
    assert(INVALID_SOCK ~= fd)
    sdata, ssize = custz.pack("this is ssl syn send test.")
    rdata, rsize = srey.syn_send(fd, skid, sdata, ssize, 0)
    assert(rdata)
    assert(srey.hex("this is ssl syn send test.") == srey.hex(rdata, rsize))
    srey.close(fd, skid)

    fd,skid = srey.udp()
    assert(INVALID_SOCK ~= fd)
    local udpstr = "this is syn sendto test."
    rdata, rsize =  srey.syn_sendto(fd, skid, "127.0.0.1", 15002, udpstr, #udpstr)
    assert(rdata and srey.hex(udpstr) == srey.hex(rdata, rsize))
    srey.close(fd, skid)
    printd("client syn send/sendto tested.")
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
