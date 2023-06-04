local srey = require("lib.srey")
local hstatus = require("lib.http_status")
local json = require("cjson")
local table = table
local string = string
local http = {}

local function http_send(rsp, fd, msg, ckfunc)
    if rsp then
        srey.send(fd, table.concat(msg), nil, PACK_TYPE.HTTP)
    else
        local ok, data, _ = srey.synsend(fd, table.concat(msg), nil, PACK_TYPE.HTTP, ckfunc)
        return ok and data or nil
    end
end
local function http_msg(rsp, fd, fline, headers, ckfunc, info, ...)
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
        return http_send(rsp, fd, msg, ckfunc)
    elseif "table" == msgtype then
        local jmsg = json.encode(info)
        table.insert(msg, string.format("Content-Type: application/json\r\nContent-Length: %d\r\n\r\n", #jmsg))
        table.insert(msg, jmsg)
        return http_send(rsp, fd, msg, ckfunc)
    elseif "function" == msgtype then
        table.insert(msg, "Transfer-Encoding: chunked\r\n\r\n")
        srey.send(fd, table.concat(msg), nil, PACK_TYPE.HTTP)
        local rtn
        while true do
            rtn = info(...)
            msg = {}
            if rtn then
                table.insert(msg, string.format("%d\r\n", #rtn))
                table.insert(msg, rtn)
                table.insert(msg, "\r\n")
                srey.send(fd, table.concat(msg))
                srey.sleep(10)--让其他能执行
            else
                table.insert(msg, "0\r\n\r\n")
                return http_send(rsp, fd, msg, ckfunc)
            end
        end
    else
        table.insert(msg, "\r\n")
        return http_send(rsp, fd, msg, ckfunc)
    end
end
--[[
描述:get
参数：
    fd :integer
    url :string
    headers :table
    ckfunc :function
返回:
    table
    nil 失败 
--]]
function http.get(fd, url, headers, ckfunc)
    local fline = string.format("GET %s HTTP/1.1\r\n", url)
    return http_msg(false, fd, fline, headers, ckfunc)
end
--[[
描述:post
参数：
    fd :integer
    url :string
    headers :table
    ckfunc :function
    info :string table function
    ...  info为function时的参数
返回:
    table
    nil 失败 
--]]
function http.post(fd, url, headers, ckfunc, info, ...)
    local fline = string.format("POST %s HTTP/1.1\r\n", url)
    return http_msg(false, fd, fline, headers, ckfunc, info, ...)
end
--[[
描述:response
参数：
    fd :integer
    code :integer
    headers :table
    info :string table function
    ...  info为function时的参数
--]]
function http.response(fd, code, headers, info, ...)
    local status = hstatus[code]
    local fline = string.format("HTTP/1.1 %03d %s\r\n", code, status or "")
    if nil == info then
        info = status or ""
    end
    http_msg(true, fd, fline, headers, nil, info, ...)
end
--[[
描述:http 升级websocket成功 返回
参数：
    fd :integer
    sign :string
--]]
function http.rsp_websock_allowed(fd, sign)
    local fline = "HTTP/1.1 101 Switching Protocols\r\n"
    local headers = {
        Upgrade = "websocket",
        Connection = "Upgrade"
    }
    headers["Sec-WebSocket-Accept"] = sign
    http_msg(true, fd, fline, headers)
end

return http
