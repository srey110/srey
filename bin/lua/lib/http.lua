local sutils = require("srey.utils")
local core = require("lib.core")
local syn = require("lib.synsl")
local hstatus = require("lib.http_status")
local json = require("cjson")
local table = table
local string = string
local http_ver = "1.1"
local http = {}

--[[
描述:chunk包状态
参数:
    pack :http_pack_ctx
返回:
    0非chunk包 1chunk包头 2chunk数据
--]]
function http.chunked(pack)
    return sutils.http_chunked(pack)
end
--[[
描述:http第一行
参数:
    pack :http_pack_ctx
返回:
    table
--]]
function http.status(pack)
    return sutils.http_status(pack)
end
--[[
描述:http头的值
参数:
    pack :http_pack_ctx
    key :string
返回:
    string
    nil
--]]
function http.head(pack, key)
    return sutils.http_head(pack, key)
end
--[[
描述:http头
参数:
    pack :http_pack_ctx
返回:
    table
    nil
--]]
function http.heads(pack)
    return sutils.http_heads(pack)
end
--[[
描述:http数据
参数:
    pack :http_pack_ctx
返回:
    data size
    nil
--]]
function http.data(pack)
    return sutils.http_data(pack)
end
--[[
描述:http数据
参数:
    pack :http_pack_ctx
返回:
    table {chunked, status, head, data, cksize, fin}
--]]
function http.unpack(pack)
    local tb = {}
    tb.chunked = http.chunked(pack)
    tb.status = http.status(pack)
    tb.head = http.heads(pack)
    local data, size = http.data(pack)
    if data then
        tb.data = core.utostr(data, size)
    end
    return tb
end
local function http_send(rsp, fd, skid, msg, ckfunc)
    local smsg = table.concat(msg)
    if rsp then
        core.send(fd, skid, smsg, #smsg, true)
        return
    end
    local sess = core.getid()
    local pack, _ = syn.send(fd, skid, sess, smsg, #smsg, true)
    if not pack then
        return
    end
    pack = http.unpack(pack)
    if 1 == pack.chunked then
        assert(ckfunc, "no have http chunked function.")
        pack.cksize = 0
        pack.fin = false
        local data, hdata, hsize, fin
        while not core.task_closing() do
            data, _, fin = syn.slice(fd, skid, sess)
            if not data then
                return
            end
            hdata, hsize = http.data(data)
            ckfunc(fd, skid, pack, hdata, hsize, fin)
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
local function http_msg(rsp, fd, skid, fline, headers, ckfunc, info, ...)
    local msg = {}
    table.insert(msg, fline)
    if nil ~= headers then
        for key, val in pairs(headers) do
            table.insert(msg, string.format("%s: %s\r\n", key, val))
        end
    end
    local msgtype = type(info)
    if "string" == msgtype then
        table.insert(msg, string.format("Content-Length: %d\r\n\r\n", #info))
        table.insert(msg, info)
        return http_send(rsp, fd, skid, msg, ckfunc)
    elseif "table" == msgtype then
        local jmsg = json.encode(info)
        table.insert(msg, string.format("Content-Type: application/json\r\nContent-Length: %d\r\n\r\n", #jmsg))
        table.insert(msg, jmsg)
        return http_send(rsp, fd, skid, msg, ckfunc)
    elseif "function" == msgtype then
        table.insert(msg, "Transfer-Encoding: chunked\r\n\r\n")
        local smsg, rtn, size
        while not core.task_closing() do
            rtn, size = info(...)
            if rtn then
                table.insert(msg, string.format("%x\r\n", size or #rtn))
                table.insert(msg, rtn)
                table.insert(msg, "\r\n")
                smsg = table.concat(msg)
                core.send(fd, skid, smsg, #smsg, true)
                msg = {}
                syn.sleep(10)--让其他能执行
            else
                table.insert(msg, "0\r\n\r\n")
                return http_send(rsp, fd, skid, msg, ckfunc)
            end
        end
    else
        table.insert(msg, "\r\n")
        return http_send(rsp, fd, skid, msg, ckfunc)
    end
end
--[[
描述:get
参数:
    fd :integer
    skid :integer
    url :string
    headers :table
    ckfunc :function(fd, skid, hinfo, data, size, fin)
返回:
    table {chunked, status, head, data, cksize}
    nil 失败 
--]]
function http.get(fd, skid, url, headers, ckfunc)
    local fline = string.format("GET %s HTTP/%s\r\n", core.urlencode(url or "/"), http_ver)
    return http_msg(false, fd, skid, fline, headers, ckfunc)
end
--[[
描述:post
参数:
    fd :integer
    skid :integer
    url :string
    headers :table
    ckfunc :function(fd, skid, hinfo, data, size, fin)
    info :string table function(返回 data size)
    ...  info为function时的参数
返回:
    table {chunked, status, head, data, cksize}
    nil 失败 
--]]
function http.post(fd, skid, url, headers, ckfunc, info, ...)
    local fline = string.format("POST %s HTTP/%s\r\n", core.urlencode(url or "/"), http_ver)
    return http_msg(false, fd, skid, fline, headers, ckfunc, info, ...)
end
--[[
描述:response
参数:
    fd :integer
    skid :integer
    code :integer
    headers :table
    info :string table function
    ...  info为function是的参数
--]]
function http.response(fd, skid, code, headers, info, ...)
    local fline = string.format("HTTP/%s %03d %s\r\n", http_ver, code, hstatus[code])
    http_msg(true, fd, skid, fline, headers, nil, info, ...)
end
--[[
描述: 检查是否升级为websocket，并返回签名字符
参数:
    hpack :http_pack_ctx
返回:
    签名字符 :string
    nil 非websocket握手
--]]
function http.upgrade_websock(hpack)
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
描述:向客户端返回握手成功,并切换socket对应的协议解析
参数:
    fd :integer
    skid :integer
    sign websock.upgrade 返回值 :string
--]]
function http.upgrade_websock_allowed(fd, skid, sign)
    core.ud_pktype(fd, skid, PACK_TYPE.WEBSOCK)
    core.ud_status(fd, skid, 1)
    local headers = {
        Upgrade = "websocket",
        Connection = "Upgrade"
    }
    headers["Sec-WebSocket-Accept"] = sign
    http.response(fd, skid, 101, headers)
end

return http
