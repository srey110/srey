local sutils = require("srey.utils")
local core = require("lib.core")
local http = require("lib.http")
local string = string
local websock = {}
local status_start = 1
WEBSOCK_PROTO = {
    CONTINUE = 0x00,
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0A
}
--[[
描述: 解包
参数:
    pack :websock_pack_ctx
返回:
    proto, fin, data, size
--]]
function websock.unpack(pack)
    return sutils.websock_pack(pack)
end
--[[
描述: 服务端检查websocket参数，并返回签名字符
参数:
    hpack :http_pack_ctx
返回:
    签名字符 :string
    nil 非websocket握手
--]]
function websock.upgrade(hpack)
    local val = http.head(hpack, "connection")
    if not val or not string.find(string.lower(val), "upgrade") then
        return nil
    end
    val = http.head(hpack, "upgrade")
    if not val or "websocket" ~= string.lower(val) then
        return nil
    end
    val = http.head(hpack, "sec-websocket-version")
    if not val or "13" ~= val then
        return nil
    end
    val = http.head(hpack, "sec-websocket-key")
    if not val then
        return nil
    end
    return core.sha1_b64encode(string.format("%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", val))
end
--[[
描述: 服务端向客户端返回握手成功,并切换socket对应的协议解析
参数:
    fd :integer
    skid :integer
    sign websock.upgrade 返回值 :string
--]]
function websock.allowed(fd, skid, sign)
    core.ud_pktype(fd, skid, PACK_TYPE.WEBSOCK)
    core.ud_status(fd, skid, status_start)
    http.rsp_websock_allowed(fd, skid, sign)
end
local function websock_checkrsp(hpack)
    if not hpack.head or not hpack.status or
       "101" ~= hpack.status[2] or "switching protocols" ~= string.lower(hpack.status[3]) then
        return nil
    end
    local cnt = 0
    local signkey
    for key, value in pairs(hpack.head) do
        key = string.lower(key)
        if "connection" == key then
            if not string.find(string.lower(value), "upgrade") then
                return nil
            end
            cnt = cnt + 1
        elseif "upgrade" == key then
            if "websocket" ~= string.lower(value) then
                return nil
            end
            cnt = cnt + 1
        elseif "sec-websocket-accept" == key then
            signkey = value
            cnt = cnt + 1
        end
        if 3 == cnt then
            break
        end
    end
    if 3 ~= cnt then
        return nil
    end
    return signkey
end
--[[
描述: wesocket握手
参数:
    ssl :evssl_ctx
    skid :integer
    url :url
    headers :table
返回:
    boolean    
--]]
function websock.handshake(fd, skid, url, headers)
    if not headers then
        headers = {}
    end
    local key = core.b64encode(randstr(8))
    headers["Connection"] = "Upgrade,Keep-Alive"
    headers["Upgrade"] = "websocket"
    headers["Sec-WebSocket-Version"] = "13"
    headers["Sec-WebSocket-Key"] = key
    local rtn = http.get(fd, skid, url, headers)
    if not rtn then
        return false
    end
    local sign = websock_checkrsp(rtn)
    if not sign then
        return false
    end
    if sign ~= core.sha1_b64encode(string.format("%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key)) then
        return false
    end
    core.ud_pktype(fd, skid, PACK_TYPE.WEBSOCK)
    core.ud_status(fd, skid, status_start)
    return true
end
--[[
描述: wesocket链接,并握手
参数:
    ip :string
    port :integer
    ssl :evssl_ctx
返回:
    fd :integer skid :integer
    INVALID_SOCK 失败 
--]]
function websock.connect(ip, port, ssl)
    return sutils.websock_connect(core.self(), ip, port, ssl)
end
 --[[
 描述: ping
 参数:
     fd :integer
     skid :integer
 --]]
 function websock.ping(fd, skid)
    sutils.websock_ping(fd, skid)
 end
 --[[
 描述: pong
 参数:
     fd :integer
     skid :integer
 --]]
 function websock.pong(fd, skid)
    sutils.websock_pong(fd, skid)
 end
 --[[
 描述: close
 参数:
     fd :integer
     skid :integer
 --]]
 function websock.close(fd, skid)
    sutils.websock_close(fd, skid)
 end
 --起始帧:FIN为0,opcode非0 中间帧:FIN为0,opcode为0 结束帧:FIN为1,opcode为0
 local function continuation(fd, skid, key, func, ...)
    local fin
    local rtn
    while true do
        fin, rtn = func(...)
        if fin then
           sutils.websock_continuation(fd, skid, 1, rtn, nil, key)
           break
        else
           sutils.websock_continuation(fd, skid, 0, rtn, nil, key)
           core.sleep(10)--让其他能执行
        end
    end
end
 --[[
 描述: text
 参数:
     fd :integer
     skid :integer
     data :string or userdata
     dlen :integer
     key 客服端需要 :string
     func nil 不分片: function  返回 boolean(true 结束)， data
 --]]
 function websock.text(fd, skid, data, dlen, key, func, ...)
    if key and 4 > #key then
        return
    end
    if not func then
        sutils.websock_text(fd, skid, 1, data, dlen, key)
    else
        sutils.websock_text(fd, skid, 0, data, dlen, key)
        continuation(fd, skid, key, func, ...)
    end
 end
 --[[
 描述: binary
 参数:
     fd :integer
     skid :integer
     data :string or userdata
     dlen :integer
     key 客服端需要 :string
     func : function  返回 boolean(true 结束)， data
 --]]
 function websock.binary(fd, skid, data, dlen, key, func, ...)
    if key and 4 > #key then
        return
    end
    if not func then
        sutils.websock_binary(fd, skid, 1, data, dlen, key)
    else
        sutils.websock_binary(fd, skid, 0, data, dlen, key)
        continuation(fd, skid, key, func, ...)
    end
 end

return websock
