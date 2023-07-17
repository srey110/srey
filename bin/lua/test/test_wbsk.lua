require("lib.funcs")
local srey = require("lib.srey")
local cbs = require("lib.cbs")
local wbsk = require("lib.websock")
local continuasize

local function startup()
    srey.listen("0.0.0.0", 15003, PACK_TYPE.WEBSOCK)
end
cbs.cb_startup(startup)
local function handshaked(msg)
    wbsk.text(msg.fd, msg.skid, string.format("%s welcome!", os.date(FMT_TIME, os.time())));
end
cbs.cb_handshake(handshaked)
local function continua(cnt)
    cnt.n = cnt.n + 1
    if cnt.n <= 3 then
        return string.format(" %s %d", os.date(FMT_TIME, os.time()), cnt.n)
    else
        return nil
    end
end
local function recv(msg)
    local pack = wbsk.unpack(msg.data)
    if WEBSOCK_PROTO.PING == pack.proto then
        wbsk.pong(msg.fd,  msg.skid)
    elseif WEBSOCK_PROTO.CLOSE == pack.proto then
        srey.close(msg.fd,  msg.skid)
    elseif WEBSOCK_PROTO.TEXT == pack.proto then
        if pack.fin then
            local data = srey.utostr(pack.data, pack.size)
            if "chunck" == data then
                local cnt = {n = 0}
                wbsk.text(msg.fd, msg.skid, continua, 0, false, cnt)
            else
                local rtn = string.format("%s %s", os.date(FMT_TIME, os.time()), data);
                wbsk.text(msg.fd, msg.skid, rtn)
            end
        else
            continuasize = 0
            if pack.data then
                continuasize = continuasize + pack.size
            end
            --printd("continua start.")
        end
    elseif WEBSOCK_PROTO.BINARY == pack.proto then
        local data = srey.utostr(pack.data, pack.size)
        local rtn = string.format("%s %s", os.date(FMT_TIME, os.time()), data);
        wbsk.binary(msg.fd, msg.skid, rtn)
    elseif WEBSOCK_PROTO.CONTINUA == pack.proto then
        continuasize = continuasize + pack.size
        if pack.fin then
            --printd("continua end, size %d", continuasize)
            wbsk.text(msg.fd, msg.skid, "continua size " .. continuasize)
        end
    end
end
cbs.cb_recv(recv)
