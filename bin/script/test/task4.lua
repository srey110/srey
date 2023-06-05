local websock = require("lib.websock")
local srey = require("lib.srey")

local function onstarted()
    printd(srey.name() .. " onstarted....")
    srey.listen("0.0.0.0", 15004, PACK_TYPE.WEBSOCK)
end
srey.started(onstarted)

local function onaccept(pktype, fd)
    if pktype == PACK_TYPE.WEBSOCK then
        printd("websocket accpeted")
        websock.text(fd, "welcome! this is websocket.")
    end
end
srey.accepted(onaccept)
local function onsockclose(pktype, fd)
    if pktype == PACK_TYPE.WEBSOCK then
        printd("websocket closed")
    end
end
srey.closed(onsockclose)

local function onhandshaked(fd)
    websock.text(fd, "welcome! this is http upgrade to websocket.")
end
srey.regrpc("handshaked", onhandshaked)
local function onrecv(pktype, fd, data, size)
    local frame = websock.frame(data)
    if WEBSOCK_PROTO.PING == frame.proto then
        websock.pong(fd)
        --printd("PING")
    elseif WEBSOCK_PROTO.CLOSE == frame.proto then
        srey.close(fd)
        printd("CLOSE")
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
