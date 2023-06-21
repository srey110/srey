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
    data :userdata
    toud 数据是否以userdata方式传递 0 否 1 是
返回:
    table
--]]
function websock.unpack(data, toud)
    return sutils.websock_pack(data, toud)
end
--[[
描述: 服务端检查websocket参数，并返回签名字符
参数:
    head srey.http_head 返回值 :table
返回:
    签名字符 :string
    nil 非websocket握手
--]]
function websock.upgrade(head)
    if not head or not head.head then
        return nil
    end
    local cnt = 0
    local signkey
    for key, value in pairs(head.head) do
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
        elseif "sec-websocket-version" == key then
            if "13" ~= value then
                return nil
            end
            cnt = cnt + 1
        elseif "sec-websocket-key" == key then
            signkey = value
            cnt = cnt + 1
        end
        if 4 == cnt then
            break
        end
    end
    if 4 ~= cnt then
        return nil
    end
    return core.sha1_b64encode(string.format("%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", signkey))
end
--[[
描述: 服务端向客户端返回握手成功,并切换socket对应的协议解析
参数:
    fd :integer
    sign websock.upgrade 返回值 :string
--]]
function websock.allowed(fd, sign)
    core.ud_typstat(fd, PACK_TYPE.WEBSOCK, status_start)
    http.rsp_websock_allowed(fd, sign)
end
local function websock_checkrsp(head)
    if not head or not head.head then
        return nil
    end
    if not head.status or "101" ~= head.status[2]
       or "switching protocols" ~= string.lower(head.status[3]) then
        return nil
    end
    local cnt = 0
    local signkey
    for key, value in pairs(head.head) do
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
描述: wesocket链接,并握手
参数:
    ip :string
    port :integer
    ssl :evssl_ctx
返回:
    fd :integer
    INVALID_SOCK 失败 
--]]
function websock.connect(ip, port, ssl)
    return sutils.websock_connect(core.self(), ip, port, ssl)
end
--[[
描述: wesocket握手
参数:

    ssl :evssl_ctx
    url :url
    headers :table
返回:
    boolean    
--]]
function websock.handshake(fd, url, headers)
    if not headers then
        headers = {}
    end
    local key = core.b64encode(randstr(8))
    headers["Connection"] = "Upgrade,Keep-Alive"
    headers["Upgrade"] = "websocket"
    headers["Sec-WebSocket-Version"] = "13"
    headers["Sec-WebSocket-Key"] = key
    local rtn = http.get(fd, url, headers)
    core.ud_typstat(fd, PACK_TYPE.WEBSOCK, status_start)
    local sign = websock_checkrsp(rtn)
    if not sign then
        return false
    end
    if sign ~= core.sha1_b64encode(string.format("%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key)) then
        return false
    end
    return true
end
 --[[
 描述: ping
 参数:
     fd :integer
 --]]
 function websock.ping(fd)
    sutils.websock_ping(fd)
 end
 --[[
 描述: pong
 参数:
     fd :integer
 --]]
 function websock.pong(fd)
    sutils.websock_pong(fd)
 end
 --[[
 描述: close
 参数:
     fd :integer
 --]]
 function websock.close(fd)
    sutils.websock_close(fd)
 end
 --[[
 描述: text
 参数:
     fd :integer
     data :string or userdata
     dlen :integer
     key 客服端需要 :string
 --]]
 function websock.text(fd, data, dlen, key)
     if key and 4 > #key then
         return
     end
     sutils.websock_text(fd, data, dlen, key)
 end
 --[[
 描述: binary
 参数:
     fd :integer
     data :string or userdata
     dlen :integer
     key 客服端需要 :string
 --]]
 function websock.binary(fd, data, dlen, key)
     if key and 4 > #key then
         return
     end
     sutils.websock_binary(fd, data, dlen, key)
 end
 --[[
 描述: continuation
 参数:
     fd :integer
     key 客服端需要 :string
     func : function  返回 boolean(true 结束)， data
 --]]
 function websock.continuation(fd, key, func, ...)
     if key and 4 > #key then
         return
     end
     local fin
     local rtn
     while true do
         fin, rtn = func(...)
         if fin then
            sutils.websock_continuation(fd, 1, rtn, nil, key)
            break
         else
            sutils.websock_continuation(fd, 0, rtn, nil, key)
            core.sleep(10)--让其他能执行
         end
     end
 end

return websock
