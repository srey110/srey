local sutils = require("srey.utils")
local core = require("lib.core")
local syn = require("lib.synsl")
local http = require("lib.http")
local json = require("cjson")
local crypto = require("lib.crypto")
local key = sutils.sign_key()
local rpc = {}

--[[
描述:RPC，无返回
参数:
    task :task_grab返回值
    method : string
--]]
function rpc.call(task, method, ...)
    local req = {
        method = method,
        args = {...}
    }
    local jreq = json.encode(req)
    core.task_call(task, REQUEST_TYPE.RPC, jreq, #jreq, true)
end
--[[
描述:RPC，等待返回
参数:
    task :task_grab返回值
    method : string
返回:
    boolean ...
--]]
function rpc.request(task, method, ...)
    local req = {
        method = method,
        args = {...}
    }
    local jreq = json.encode(req)
    local ok, data, size = syn.task_request(task, REQUEST_TYPE.RPC, jreq, #jreq, true)
    if not ok then
        return false
    end
    return ok, table.unpack(json.decode(data, size))
end
local function _net_rpc_sign(head, url, jreq)
    if not key or 0 == #key  then
        return
    end
    local tms = tostring(os.time())
    local sign = string.format("%s%s%s%s", url, jreq, tms, key)

    crypto.sha256_update(sign)
    local en = crypto.sha256_final()

    crypto.md5_update(en)
    en = crypto.md5_final()
    head["X-Timestamp"] = tms
    head["Authorization"] = core.tohex(en)
end
--[[
描述:远程RPC，无返回
参数:
    dst :TASK_NAME
    fd : integer
    skid :integer
    method : string
--]]
function rpc.net_call(dst, fd, skid, method, ...)
    local req = {
        method = method,
        args = {...}
    }
    local head = {
        Server = "Srey",
        Connection = "Keep-Alive"
    }
    local jreq = json.encode(req)
    local url = string.format("/rpc_call?dst=%d", dst)
    _net_rpc_sign(head, url, jreq)
    head["Content-Type"] = "application/json"
    local fline = string.format("POST %s HTTP/1.1\r\n", crypto.url_encode(url))
    http.http_msg(true, fd, skid, fline, head, nil, jreq)
end
--[[
描述:远程RPC，等待返回
参数:
    dst :TASK_NAME
    fd : integer
    skid :integer
    method : string
返回:
    boolean ...
--]]
function rpc.net_request(dst, fd, skid, method, ...)
    local req = {
        method = method,
        args = {...}
    }
    local head = {
        Server = "Srey",
        Connection = "Keep-Alive"
    }
    local jreq = json.encode(req)
    local url = string.format("/rpc_request?dst=%d", dst)
    _net_rpc_sign(head, url, jreq)
    head["Content-Type"] = "application/json"
    local resp = http.post(fd, skid, url , head, nil, jreq)
    if not resp then
        return false
    end
    if 0 ~= resp.chunked then
        return false
    end
    if "200" ~= resp.status[2] then
        return false
    end
    return true, table.unpack(json.decode(resp.data))
end

return rpc
