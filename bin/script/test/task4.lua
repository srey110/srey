local websock = require("lib.websock")
local srey = require("lib.srey")
local function onstarted()
    srey.listen("0.0.0.0", 15004, PACK_TYPE.WEBSOCK)
    srey.listen("::", 15004, PACK_TYPE.WEBSOCK)
    local bg = os.time()
    srey.sleep(1000)
    local ed = os.time()
    if ed - bg ~= 1 then
        printd("sleep 1000 ms error")
    end
end
srey.started(onstarted)
local function handshaked(pktype, fd, skid)
    websock.text(fd, skid, "welcome! this is websocket.")
end
srey.handshaked(handshaked)
local function onhandshaked(fd, skid)
    websock.text(fd, skid, "welcome! this is http upgrade to websocket.")
end
srey.regrpc("handshaked", onhandshaked)
local function continuationcb(cnt)
    cnt.cnt = cnt.cnt + 1
    if cnt.cnt >= 3 then
        return true, " this is continuation "..tostring(cnt.cnt)
    end
    return false, " this is continuation "..tostring(cnt.cnt)
end
local function onrecv(pktype, fd, skid, data, size)
    local proto, fin, pack, plens = websock.unpack(data)
    if WEBSOCK_PROTO.PING == proto then
        websock.pong(fd, skid)
        --printd("PING")
    elseif WEBSOCK_PROTO.CLOSE == proto then
        srey.close(fd, skid)
        --printd("CLOSE")
    elseif WEBSOCK_PROTO.TEXT == proto  then
        if 0 == fin then
            --printd("CONTINUE start")
        end
        --local msg = srey.utostr(pack, plens)
        --printd("TEXT size: %d", plens)
        local now = os.date(FMT_TIME, os.time())
        if "CONTINUE" == srey.utostr(pack, plens) then
            local cnt = {
                cnt = 0;
            }
            websock.text(fd, skid, now, nil, nil, continuationcb, cnt)
        else
            websock.text(fd, skid, now)
        end
    elseif WEBSOCK_PROTO.BINARY == proto  then
        if 0 == fin then
            --printd("CONTINUE start")
        end
        --local msg = srey.utostr(pack, plens)
        --printd("BINARY size: %d", plens)
        local now = os.date(FMT_TIME, os.time())
        websock.binary(fd, skid, now)
    elseif WEBSOCK_PROTO.CONTINUE == proto then
        if 1 == fin then
            --printd("CONTINUE end")
        else
            --printd("CONTINUE size: %d", plens)
        end
    end
end
srey.recved(onrecv)
