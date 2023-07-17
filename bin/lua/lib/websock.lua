local sutils = require("srey.utils")
local core = require("lib.core")
local syn = require("lib.synsl")
local dns = require("lib.dns")
local log = require("lib.log")
local WEBSOCK_PROTO = WEBSOCK_PROTO
local wbsk = {}

--[[
描述: 解包
参数:
    pack :websock_pack_ctx
返回:
    table {proto, fin, data, size}
--]]
function wbsk.unpack(pack)
    return sutils.websock_unpack(pack)
end
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
     mask :bool
 --]]
function wbsk.ping(fd, skid, mask)
    sutils.websock_ping(fd, skid, mask and 1 or 0)
end
 --[[
 描述: pong
 参数:
     fd :integer
     skid :integer
     mask :bool
 --]]
function wbsk.pong(fd, skid, mask)
    sutils.websock_pong(fd, skid, mask and 1 or 0)
end
 --[[
 描述: close
 参数:
     fd :integer
     skid :integer
     mask :bool
 --]]
function wbsk.close(fd, skid, mask)
    sutils.websock_close(fd, skid, mask and 1 or 0)
end
local function continua(fd, skid, proto, func, mask, ...)
    local data = func(...)
    if not data then
        log.WARN("continua, but get nil data at first pack.")
        return
    end
    if WEBSOCK_PROTO.TEXT == proto then
        sutils.websock_text(fd, skid, mask, 0, data, #data)
    elseif WEBSOCK_PROTO.BINARY == proto then
        sutils.websock_binary(fd, skid, mask, 0, data, #data)
    end
    while not core.task_closing()  do
        data = func(...)
        if data then
            sutils.websock_continuation(fd, skid, mask, 0, data, #data)
            syn.sleep(10)
        else
            sutils.websock_continuation(fd, skid, mask, 1, nil, 0)
            break
        end
    end
end
 --[[
 描述: text
 参数:
     fd :integer
     skid :integer
     info :string or userdata or func(返回 data)
     lens :integer
     mask :bool
 --]]
function wbsk.text(fd, skid, info, lens, mask, ...)
    if "function" == type(info) then
        continua(fd, skid, WEBSOCK_PROTO.TEXT, info, mask and 1 or 0, ...)
    else
        sutils.websock_text(fd, skid, mask and 1 or 0, 1, info, lens)
    end
end
 --[[
 描述: binary
 参数:
     fd :integer
     skid :integer
     info :string or userdata or func(返回 data)
     lens :integer
     mask :bool
 --]]
function wbsk.binary(fd, skid, info, lens, mask, ...)
    if "function" == type(info) then
        continua(fd, skid, WEBSOCK_PROTO.BINARY, info, mask and 1 or 0, ...)
    else
        sutils.websock_binary(fd, skid, mask and 1 or 0, 1, info, lens)
    end
end

return wbsk
