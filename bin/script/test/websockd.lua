local wbsk = require("lib.websock")
local srey = require("lib.srey")
local ncont = 0

local function _cont(tag)
    ncont = ncont + 1
    if ncont > 5 then
        return
    end
    local msg = tag .. " " .. ncont .. ";"
    return msg, #msg
end
local function _wbsock_test(ws)
    local fd, skid = wbsk.connect(ws, 0, 2000, 0)
    if INVALID_SOCK == fd then
        printd("websock.connect error")
        return
    end
    wbsk.ping(fd, skid, 1)
    wbsk.text(fd, skid, 1, "this is text test.")
    wbsk.binary(fd, skid, 1, "this is binary test.")
    ncont = 0
    wbsk.text_continua(fd, skid, 1, _cont, "text continuation")
    ncont = 0
    wbsk.binary_continua(fd, skid, 1, _cont, "binary continuation")
    wbsk.close(fd, skid, 1)
end
srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, sess, slice, data, size)
                local wbskpk = wbsk.unpack(data)
                --printd("websock proto %s", wbsk.protostr(wbskpk.proto))
                if WEBSOCK_PROTO.CLOSE == wbskpk.proto then
                    srey.close(fd, skid)
                end
                if wbskpk.data then
                    --printd("%s", srey.ud_str(wbskpk.data, wbskpk.size))
                end
            end
        )
        --_wbsock_test("ws://124.222.224.186:8800")
        _wbsock_test("ws://127.0.0.1:15004")
    end
)
