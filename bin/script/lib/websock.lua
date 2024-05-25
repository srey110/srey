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
function wbsk.ping(client)
    return websock.ping(client)
end
function wbsk.pong(client)
    return websock.pong(client)
end
function wbsk.close(client)
    return websock.close(client)
end
function wbsk.text_fin(client, fin, data, size)
    return websock.text(client, fin, data, size)
end
function wbsk.text(client, data, size)
    return wbsk.text_fin(client, 1, data, size)
end
function wbsk.binary_fin(client, fin, data, size)
    return websock.binary(client, fin, data, size)
end
function wbsk.binary(client, data, size)
    return wbsk.binary_fin(client, 1, data, size)
end
function wbsk.continua(client, fin, data, size)
    return websock.continuation(client, fin, data, size)
end
local function _continua(fd, skid, proto, client, func, ...)
    local data, size = func(...)
    if not data then
        return
    end
    if WEBSOCK_PROTO.TEXT == proto then
        data, size = wbsk.text_fin(client, 0, data, size)
        srey.send(fd, skid, data, size, 0)
    elseif WEBSOCK_PROTO.BINARY == proto then
        data, size = wbsk.binary_fin(client, 0, data, size)
        srey.send(fd, skid, data, size, 0)
    end
    while true do
        data, size = func(...)
        if data then
            data, size = wbsk.continua(client, 0, data, size)
            srey.send(fd, skid, data, size, 0)
        else
            data, size = wbsk.continua(client, 1, nil, 0)
            srey.send(fd, skid, data, size, 0)
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
