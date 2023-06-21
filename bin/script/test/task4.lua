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
    local frame = websock.unpack(data, 1)
    if WEBSOCK_PROTO.PING == frame.proto then
        websock.pong(fd)
        --printd("PING")
    elseif WEBSOCK_PROTO.CLOSE == frame.proto then
        srey.close(fd)
        --printd("CLOSE")
    elseif WEBSOCK_PROTO.TEXT == frame.proto  then
        --local msg = srey.utostr(frame.data, frame.size)
        --printd("TEXT size: %d", frame.size)
        local now = os.date(FMT_TIME, os.time())
        websock.text(fd, now)
    elseif WEBSOCK_PROTO.BINARY == frame.proto  then
        --local msg = srey.utostr(frame.data, frame.size)
        --printd("BINARY size: %d", frame.size)
        local now = os.date(FMT_TIME, os.time())
        websock.binary(fd, now)
    elseif WEBSOCK_PROTO.CONTINUE == frame.proto then
        --local msg = srey.utostr(frame.data, frame.size)
        --printd("CONTINUE fin: %d size: %d", frame.fin, frame.size)
    end
end
srey.recved(onrecv)
