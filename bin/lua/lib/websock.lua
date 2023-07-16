local sutils = require("srey.utils")
local syn = require("lib.synsl")
local dns = require("lib.dns")
local wbsk = {}

WEBSOCK_PROTO = {
    CONTINUE = 0x00,
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0A
}
--[[
描述: wesocket链接,并握手
参数:
    host :string
    port :integer
    ssl :evssl_ctx
返回:
    fd :integer skid :integer
    INVALID_SOCK 失败 
--]]
function wbsk.connect(host, port, ssl)
    local wkip = dns.qury(host)
    if not wkip then
        return INVALID_SOCK
    end
    local hpack = sutils.websock_hspack(host)
    local fd, skid = syn.conn_handshake(wkip, port, PACK_TYPE.WEBSOCK, ssl, hpack, #hpack, false)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    return fd, skid
end
 --[[
 描述: ping
 参数:
     fd :integer
     skid :integer
     client :bool
 --]]
function wbsk.ping(fd, skid, client)
    sutils.websock_ping(fd, skid, client and 1 or 0)
end
 --[[
 描述: pong
 参数:
     fd :integer
     skid :integer
     client :bool
 --]]
function wbsk.pong(fd, skid, client)
    sutils.websock_pong(fd, skid, client and 1 or 0)
end
 --[[
 描述: close
 参数:
     fd :integer
     skid :integer
     client :bool
 --]]
function wbsk.close(fd, skid, client)
    sutils.websock_close(fd, skid, client and 1 or 0)
end
 --[[
 描述: text
 参数:
     fd :integer
     skid :integer
     client :bool
     fin :bool
     data :string or userdata
     lens :integer
 --]]
function wbsk.text(fd, skid, client, fin, data, lens)
    sutils.websock_text(fd, skid, client and 1 or 0, fin and 1 or 0, data, lens)
end
 --[[
 描述: binary
 参数:
     fd :integer
     skid :integer
     client :bool
     fin :bool
     data :string or userdata
     lens :integer
 --]]
function wbsk.binary(fd, skid, client, fin, data, lens)
    sutils.websock_binary(fd, skid, client and 1 or 0, fin and 1 or 0, data, lens)
end
 --[[
 描述: continuation
 参数:
     fd :integer
     skid :integer
     client :bool
     fin :bool
     data :string or userdata
     lens :integer
 --]]
function wbsk.continuation(fd, skid, client, fin, data, lens)
    sutils.websock_continuation(fd, skid, client and 1 or 0, fin and 1 or 0, data, lens)
end

return wbsk
