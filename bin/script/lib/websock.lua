require("lib.dns")
local srey = require("lib.srey")
local websock = require("srey.websock")
local srey_url = require("srey.url")
local PACK_TYPE = PACK_TYPE
local wbsk = {}
--websock proto
WEBSOCK_PROTO = {
    CONTINUA = 0x00,
    TEXT =     0x01,
    BINARY =   0x02,
    CLOSE =    0x08,
    PING =     0x09,
    PONG =     0x0A
}

function wbsk.protostr(proto)
    if WEBSOCK_PROTO.CONTINUA == proto then
        return "CONTINUA"
    elseif WEBSOCK_PROTO.TEXT == proto  then
        return "TEXT"
    elseif WEBSOCK_PROTO.BINARY == proto  then
        return "BINARY"
    elseif WEBSOCK_PROTO.CLOSE == proto  then
        return "CLOSE"
    elseif WEBSOCK_PROTO.PING == proto  then
        return "PING"
    elseif WEBSOCK_PROTO.PONG == proto  then
        return "PONG"
    end
    return "UNKNOWN"
end
--{proto, fin, data, size}
function wbsk.unpack(pack)
    return websock.unpack(pack)
end
--ws://host:port
function wbsk.connect(ws, sslname, appendev)
    local url = srey_url.parse(ws)
    if "ws" ~= url.scheme or not url.host then
        return INVALID_SOCK
    end
    local ip = url.host
    if "hostname"  == host_type(url.host)  then
        local ips = nslookup(url.host, false)
        if not ips or 0 == #ips then
            return INVALID_SOCK
        end
        ip = ips[1]
    end
    local port = url.port
    if not port then
        if SSL_NAME.NONE == sslname then
            port = 80
        else
            port = 443
        end
    end
    local fd, skid = srey.connect(PACK_TYPE.WEBSOCK, sslname, ip, port, appendev)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    local hspack, size = websock.handshake_pack(url.host)
    srey.sock_session(fd, skid, skid)
    srey.send(fd, skid, hspack, size, 0)
    if not srey.wait_handshaked(fd, skid) then
        return INVALID_SOCK
    end
    return fd, skid
end
function wbsk.ping(fd, skid, client)
    websock.ping(fd, skid, client)
end
function wbsk.pong(fd, skid, client)
    websock.pong(fd, skid, client)
end
function wbsk.close(fd, skid, client)
    websock.close(fd, skid,  client)
end
function wbsk.text_fin(fd, skid, client, fin, data, size)
    websock.text(fd, skid, client, fin, data, size)
end
function wbsk.text(fd, skid, client, data, size)
    wbsk.text_fin(fd, skid, client, 1, data, size)
end
function wbsk.binary_fin(fd, skid, client, fin, data, size)
    websock.binary(fd, skid, client, fin, data, size)
end
function wbsk.binary(fd, skid, client, data, size)
    wbsk.binary_fin(fd, skid, client, 1, data, size)
end
function wbsk.continua(fd, skid, client, fin, data, size)
    websock.continuation(fd, skid, client, fin, data, size)
end
local function _continua(fd, skid, proto, client, func, ...)
    local data, size = func(...)
    if not data then
        return
    end
    if WEBSOCK_PROTO.TEXT == proto then
        wbsk.text_fin(fd, skid, client, 0, data, size)
    elseif WEBSOCK_PROTO.BINARY == proto then
        wbsk.binary_fin(fd, skid, client, 0, data, size)
    end
    while true do
        data, size = func(...)
        if data then
            wbsk.continua(fd, skid, client, 0, data, size)
        else
            wbsk.continua(fd, skid, client, 1, nil, 0)
            break
        end
    end
end
function wbsk.text_continua(fd, skid, client, func, ...)
    _continua(fd, skid, WEBSOCK_PROTO.TEXT, client, func, ...)
end
function wbsk.binary_continua(fd, skid, client, func, ...)
    _continua(fd, skid, WEBSOCK_PROTO.BINARY, client, func, ...)
end

return wbsk