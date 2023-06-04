local srey = require("lib.srey")
local http = require("lib.http")
local core = require("srey.core")
local log = require("lib.log")
local string = string
local websock = {}
WEBSOCK_PROTO = {
    CONTINUE = 0x00,
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0A
}

--[[
描述: 检查websocket参数，并返回签名字符
参数：
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
    return srey.sha1_b64encode(string.format("%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", signkey))
end
--[[
描述: 向客户端返回握手成功,并切换socket对应的协议解析
参数：
    fd :integer
    sign websock.upgrade 返回值 :string
--]]
function websock.allowed(fd, sign)
    srey.setpktype(fd, PACK_TYPE.WEBSOCK)
    http.rsp_websock_allowed(fd, sign)
end
--[[
描述: 解包收到的数据
参数：
    pack :websock_pack_ctx
返回:
    tabel {fin=1, proto=1, data=, size=}
--]]
function websock.frame(pack)
   return core.websock_pack(pack)
end
--[[
描述: ping
参数：
    fd :integer
--]]
function websock.ping(fd)
    core.websock_ping(fd)
end
--[[
描述: pong
参数：
    fd :integer
--]]
function websock.pong(fd)
    core.websock_pong(fd)
end
--[[
描述: close
参数：
    fd :integer
--]]
function websock.close(fd)
    core.websock_close(fd)
end
--[[
描述: text
参数：
    fd :integer
    data :string or userdata
    dlen :integer
    key :string
--]]
function websock.text(fd, data, dlen, key)
    if key and 4 > #key then
        log.WARN("key lens error. %d", #key)
        return
    end
    core.websock_text(fd, data, dlen, key)
end
--[[
描述: binary
参数：
    fd :integer
    data :string or userdata
    dlen :integer
    key :string
--]]
function websock.binary(fd, data, dlen, key)
    if key and 4 > #key then
        log.WARN("key lens error. %d", #key)
        return
    end
    core.websock_binary(fd, data, dlen, key)
end
--[[
描述: continuation
参数：
    fd :integer
    key :string
    func : function  返回 boolean(true 结束)， data
--]]
function websock.continuation(fd, key, func, ...)
    if key and 4 > #key then
        log.WARN("key lens error. %d", #key)
        return
    end
    local fin
    local rtn
    while true do
        fin, rtn = func(...)
        if fin then
            core.websock_continuation(fd, 1, rtn, nil, key)
            break
        else
            core.websock_continuation(fd, 0, rtn, nil, key)
            srey.sleep(10)--让其他能执行
        end
    end
end
local function websock_checkrsp(head)
    if not head or not head.head then
        return nil
    end
    if not head.status or "101" ~= head.status.code then
        return nil
    end
    if not head.status.reason or "switching protocols" ~= string.lower(head.status.reason) then
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
描述: 发起wesocket握手请求
参数：
    fd :integer
返回:

--]]
function websock.handshake(fd, url, headers)
    if not headers then
        headers = {}
    end
    local key = srey.b64encode(randstr(8))
    headers["Connection"] = "Upgrade"
    headers["Upgrade"] = "websocket"
    headers["Sec-WebSocket-Version"] = "13"
    headers["Sec-WebSocket-Key"] = key
    local rtn = http.get(fd, url, headers)
    srey.setpktype(fd, PACK_TYPE.WEBSOCK)
    local sign = websock_checkrsp(rtn)
    if not sign then
        srey.close(fd)
        log.WARN("check param error. %s", dump(rtn))
        return false
    end
    if sign ~= srey.sha1_b64encode(string.format("%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key)) then
        srey.close(fd)
        log.WARN("check sign error. key: %s \n %s", key, dump(rtn))
        return false
    end
    return true;
end

return websock
