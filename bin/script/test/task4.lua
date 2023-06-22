local websock = require("lib.websock")
local srey = require("lib.srey")
local function onstarted()
    srey.listen("0.0.0.0", 15004, PACK_TYPE.WEBSOCK)
end
srey.started(onstarted)
local function onhandshaked(fd)
    websock.text(fd, "welcome! http upgrade to websocket.")
end
srey.regrpc("handshaked", onhandshaked)
local function onrecv(pktype, fd, data, size)
    local proto, fin, pack, plens = websock.unpack(data)
    if WEBSOCK_PROTO.PING == proto then
        websock.pong(fd)
        --printd("PING")
    elseif WEBSOCK_PROTO.CLOSE == proto then
        srey.close(fd)
        --printd("CLOSE")
    elseif WEBSOCK_PROTO.TEXT == proto  then
        --local msg = srey.utostr(pack, plens)
        --printd("TEXT size: %d", plens)
        local now = os.date(FMT_TIME, os.time())
        websock.text(fd, now)
    elseif WEBSOCK_PROTO.BINARY == proto  then
        --local msg = srey.utostr(pack, plens)
        --printd("BINARY size: %d", plens)
        local now = os.date(FMT_TIME, os.time())
        websock.binary(fd, now)
    elseif WEBSOCK_PROTO.CONTINUE == proto then
        --local msg = srey.utostr(pack, plens)
        --printd("CONTINUE fin: %d size: %d", fin, plens)
    end
end
srey.recved(onrecv)
