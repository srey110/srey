local srey = require("lib.srey")
local srey_http = require("srey.http")
local digest = require("srey.digest")
local base64 = require("srey.base64")
local json = require("cjson")
local table = table
local string = string
local HTTP_VERSION = "1.1"
local http = {}

function http.status(pack)
    return srey_http.status(pack)
end
function http.chunked(pack)
    return srey_http.chunked(pack)
end
function http.head(pack, key)
    return srey_http.head(pack, key)
end
function http.heads(pack)
    return srey_http.heads(pack)
end
function http.data(pack)
    return srey_http.data(pack)
end
function http.datastr(pack)
    return srey_http.datastr(pack)
end
function http.unpack(pack)
    local tb = {}
    tb.status = http.status(pack)
    tb.chunked = http.chunked(pack)
    tb.heads = http.heads(pack)
    tb.data = http.datastr(pack)
    return tb
end
function http.code_status(code)
    return srey_http.code_status(code)
end
local function _http_send(rsp, fd, skid, msg, ckfunc)
    local smsg = table.concat(msg)
    if rsp then
        srey.send(fd, skid, smsg, #smsg, 1)
        return
    end
    local pack, _ = srey.syn_send(fd, skid, smsg, #smsg, 1)
    if not pack then
        return
    end
    pack = http.unpack(pack)
    if 1 == pack.chunked then
        pack.cksize = 0
        pack.fin = false
        local ok, data, hdata, hsize, fin
        while true do
            ok, fin, data, _ = srey.syn_slice(fd, skid)
            if not ok then
                return
            end
            hdata, hsize = http.data(data)
            ckfunc(fin, hdata, hsize)
            if hsize then
                pack.cksize = pack.cksize + hsize
            end
            if fin then
                pack.fin = true
                break
            end
        end
    end
    return pack
end
local function _http_msg(rsp, fd, skid, status, headers, ckfunc, info, ...)
    local msg = {}
    table.insert(msg, status)
    if nil ~= headers then
        for key, val in pairs(headers) do
            table.insert(msg, string.format("%s: %s\r\n", key, val))
        end
    end
    local msgtype = type(info)
    if "string" == msgtype then
        table.insert(msg, string.format("Content-Length: %d\r\n\r\n", #info))
        table.insert(msg, info)
        return _http_send(rsp, fd, skid, msg, ckfunc)
    elseif "table" == msgtype then
        local jmsg = json.encode(info)
        table.insert(msg, string.format("Content-Type: application/json\r\nContent-Length: %d\r\n\r\n", #jmsg))
        table.insert(msg, jmsg)
        return _http_send(rsp, fd, skid, msg, ckfunc)
    elseif "function" == msgtype then
        table.insert(msg, "Transfer-Encoding: chunked\r\n\r\n")
        local smsg, rtn, size
        while true do
            rtn, size = info(...)
            if rtn then
                table.insert(msg, string.format("%x\r\n", size or #rtn))
                table.insert(msg, rtn)
                table.insert(msg, "\r\n")
                smsg = table.concat(msg)
                srey.send(fd, skid, smsg, #smsg, 1)
                msg = {}
            else
                table.insert(msg, "0\r\n\r\n")
                return _http_send(rsp, fd, skid, msg, ckfunc)
            end
        end
    else
        table.insert(msg, "\r\n")
        return _http_send(rsp, fd, skid, msg, ckfunc)
    end
end
--ckfunc(fin, data, size)
function http.get(fd, skid, url, headers, ckfunc)
    local status = string.format("GET %s HTTP/%s\r\n", url or "/", HTTP_VERSION)
    return _http_msg(false, fd, skid, status, headers, ckfunc)
end
function http.post(fd, skid, url, headers, ckfunc, info, ...)
    local status = string.format("POST %s HTTP/%s\r\n", url or "/", HTTP_VERSION)
    return _http_msg(false, fd, skid, status, headers, ckfunc, info, ...)
end
function http.response(fd, skid, code, headers, info, ...)
    local status = string.format("HTTP/%s %03d %s\r\n", HTTP_VERSION, code, http.code_status(code))
    _http_msg(true, fd, skid, status, headers, nil, info, ...)
end
function http.websock_upgrade(hpack)
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
    local signstr = string.format("%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", val)
    local sha1 = digest.new(DIGEST_TYPE.SHA1)
    sha1:update(signstr)
    local sha1str = sha1:final()
    return base64.encode(sha1str)
end
function http.websock_allowed(fd, skid, sign, tname)
    srey.sock_pack_type(fd, skid, PACK_TYPE.WEBSOCK)
    srey.sock_status(fd, skid, 1)
    if tname then
        srey.sock_bind_task(fd, skid, tname)
    end
    local headers = {
        Upgrade = "websocket",
        Connection = "Upgrade"
    }
    headers["Sec-WebSocket-Accept"] = sign
    http.response(fd, skid, 101, headers)
end

return http
