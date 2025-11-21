require("lib.dns")
local srey = require("lib.srey")
local websock = require("srey.websock")
local srey_url = require("srey.url")
local PACK_TYPE = PACK_TYPE
local wbsk = {}
--websock prot
WEBSOCK_PROT = {
    CONTINUA = 0x00,
    TEXT =     0x01,
    BINARY =   0x02,
    CLOSE =    0x08,
    PING =     0x09,
    PONG =     0x0A
}

function wbsk.prottostr(prot)
    if WEBSOCK_PROT.CONTINUA == prot then
        return "CONTINUA"
    elseif WEBSOCK_PROT.TEXT == prot  then
        return "TEXT"
    elseif WEBSOCK_PROT.BINARY == prot  then
        return "BINARY"
    elseif WEBSOCK_PROT.CLOSE == prot  then
        return "CLOSE"
    elseif WEBSOCK_PROT.PING == prot  then
        return "PING"
    elseif WEBSOCK_PROT.PONG == prot  then
        return "PONG"
    end
    return "UNKNOWN"
end
--{fin, prot, secprot,secpack, data, size}
function wbsk.unpack(pack)
    return websock.unpack(pack)
end
--ws://host:port
function wbsk.connect(ws, sslname, secprot, netev)
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
    local fd, skid = srey.connect(PACK_TYPE.WEBSOCK, sslname, ip, port, netev)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    local hspack, size = websock.pack_handshake(url.host, secprot)
    srey.send(fd, skid, hspack, size, 0)
    local ok, data, dlens = srey.wait_handshaked(fd, skid)
    if not ok then
        return INVALID_SOCK
    end
    if secprot and #secprot > 0 then
        if srey.ud_str(data, dlens) ~= secprot then
            srey.close(fd, skid)
            return INVALID_SOCK
        end
    end
    return fd, skid
end
function wbsk.ping(client)
    return websock.pack_ping(client)
end
function wbsk.pong(client)
    return websock.pack_pong(client)
end
function wbsk.close(client)
    return websock.pack_close(client)
end
function wbsk.text_fin(client, fin, data, size)
    return websock.pack_text(client, fin, data, size)
end
function wbsk.text(client, data, size)
    return wbsk.text_fin(client, 1, data, size)
end
function wbsk.binary_fin(client, fin, data, size)
    return websock.pack_binary(client, fin, data, size)
end
function wbsk.binary(client, data, size)
    return wbsk.binary_fin(client, 1, data, size)
end
function wbsk.continua(client, fin, data, size)
    return websock.pack_continua(client, fin, data, size)
end
local function _continua(fd, skid, prot, client, func, ...)
    local data, size = func(...)
    if not data then
        return
    end
    if WEBSOCK_PROT.TEXT == prot then
        data, size = wbsk.text_fin(client, 0, data, size)
        srey.send(fd, skid, data, size, 0)
    elseif WEBSOCK_PROT.BINARY == prot then
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
    _continua(fd, skid, WEBSOCK_PROT.TEXT, client, func, ...)
end
function wbsk.binary_continua(fd, skid, client, func, ...)
    _continua(fd, skid, WEBSOCK_PROT.BINARY, client, func, ...)
end

return wbsk
