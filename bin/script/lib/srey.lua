require("lib.funcs")
local sutils = require("srey.utils")
local core = require("lib.core")
local json = require("cjson")
local msgpack = require("cmsgpack")
local simple = require("lib.simple")
local http = require("lib.http")
local table = table
local rpc_func = {}--rpc 函数
local static_funcs = {}
local timeout = {}
local encode = (RPC_USEJSON and json.encode or msgpack.pack)
local decode = (RPC_USEJSON and json.decode or msgpack.unpack)

--[[
描normal_timeout:任务初始化完成后
参数:
    func function() :function
--]]
function core.started(func)
    static_funcs.STARTED = func
end
--[[
描述:程序退出时
参数:
    func function() :function
--]]
function core.closing(func)
    static_funcs.CLOSING = func
end
--[[
描述:socket accept
参数:
    func function(pktype, fd, skid) :function
--]]
function core.accepted(func)
    static_funcs.ACCEPT = func
end
--[[
描述:socket connect
参数:
    func function(pktype, fd, skid, erro) :function
--]]
function core.connected(func)
    static_funcs.CONNECT = func
end
--[[
描述:socket handshaked
参数:
    func function(pktype, fd, skid) :function
--]]
function core.handshaked(func)
    static_funcs.HANDSHAKED = func
end
--[[
描述:socket recv
参数:
    func function(pktype, fd, skid, data, size) :function
--]]
function core.recved(func)
    static_funcs.RECV = func
end
--[[
描述:udp recvfrom
参数:
    func function(fd, skid, data, size, ip, port) :function
--]]
function core.recvfromed(func)
    static_funcs.RECVFROM = func
end
--[[
描述:socket sended
参数:
    func function(pktype, fd, skid, size) :function
--]]
function core.sended(func)
    static_funcs.SEND = func
end
--[[
描述:socket close
参数:
    func function(pktype, fd, skid) :function
--]]
function core.closed(func)
    static_funcs.CLOSE = func
end
--[[
描述:定时
参数:
    ms 毫秒 :integer
    func function(...) :function
--]]
function core.timeout(ms, func, ...)
    local sess = core.getid()
    timeout[sess] = {
        func = func,
        arg = {...}
    }
    sutils.timeout(core.self(), sess, ms)
end
--[[
描述:RPC函数注册
参数:
    name 名称 :string
    func function(...) :function
--]]
function core.regrpc(name, func)
    if rpc_func[name] then
        return
    end
    rpc_func[name] = func
end
--[[
描述:RPC调用，不等待返回
参数:
    task :task_ctx
    name 名称 :string
    ... 调用参数
--]]
function core.call(task, name, ...)
    local info = {
        func = name,
        arg = {...}
    }
    sutils.task_call(task, encode(info))
end
--[[
描述:RPC调用，等待返回
参数:
    task :task_ctx
    name 名称 :string
    ... 调用参数
返回:
    bool, 被调函数返回值 
--]]
function core.request(task, name, ...)
    local info = {
        func = name,
        arg = {...}
    }
    local data, size = sutils.task_request(task, core.self(), encode(info))
    if not data then
        return false
    end
    info = decode(data, size)
    if info.ok then
        return true, table.unpack(info.arg)
    end
    return false
end
--[[
描述:远程RPC调用，不等待返回
参数:
    fd :integer
    skid :integer
    task :TASK_NAME
    name 名称 :string
    ... 调用参数
--]]
function core.netcall(fd, skid, task, name, ...)
    local info = {
        dst = task,
        func = name,
        arg = {...}
    }
    core.send(fd, skid, PACK_TYPE.RPC, encode(info))
end
--[[
描述:远程RPC调用，等待返回
参数:
    fd :integer
    skid :integer
    task :TASK_NAME
    name 名称 :string
    ... 调用参数
返回:
    bool, 被调函数返回值 
--]]
function core.netreq(fd, skid, task, name, ...)
    if INVALID_SOCK == fd then
        return false
    end
    local info = {
        dst = task,
        src = core.task_name(core.self()),
        func = name,
        arg = {...}
    }
    local resp, _, _ = core.synsend(fd, skid, PACK_TYPE.RPC, encode(info))
    if nil == resp then
        return false
    end
    info = decode(simple.unpack(resp))
    if info.ok then
        return true, table.unpack(info.arg)
    end
    return false
end
--消息类型
local MSG_TYPE = {
    STARTED = 1,
    CLOSING = 2,
    TIMEOUT = 3,
    ACCEPT = 4,
    CONNECT = 5,
    HANDSHAKED = 6,
    RECV = 7,
    SEND = 8,
    CLOSE = 9,
    RECVFROM = 10,
    REQUEST = 11
}
local function call_static_funcs(func, ...)
    if not func then
        return
    end
    func(...)
end
local function rpc_netreq(fd, skid, data)
    local info = decode(simple.unpack(data))
    local dst = core.task_qury(info.dst)
    if nil == dst then
        if nil ~= info.src then
            local resp = {}
            resp.ok = false
            core.send(fd, skid, PACK_TYPE.RPC, encode(resp))
        end
        return
    end
    if nil == info.src then
        core.call(dst, info.func, table.unpack(info.arg))
    else
        local resp = {}
        resp.arg = {core.request(dst, info.func, table.unpack(info.arg))}
        resp.ok = table.remove(resp.arg, 1)
        core.send(fd, skid, PACK_TYPE.RPC, encode(resp))
    end
end
local function dispatch_recv(pktype, fd, skid, data, size)
    if PACK_TYPE.RPC == pktype then
        rpc_netreq(fd, skid, data)
        return
    end
    --function(pktype, fd, skid, data, size)
    call_static_funcs(static_funcs.RECV, pktype, fd, skid, data, size)
end
local function dispatch_request(sess, src, data, size)
    local info = decode(data, size)
    local func = rpc_func[info.func]
    if nil == func then
        if -1 ~=  src then
            local resp = {}
            resp.ok = false
            sutils.task_response(core.task_qury(src), sess, encode(resp))
        end
        return
    end
    if -1 ~= src then
        local resp = {}
        resp.arg = {core.xpcall(func, table.unpack(info.arg))}
        resp.ok = table.remove(resp.arg, 1)
        sutils.task_response(core.task_qury(src), sess, encode(resp))
    else
        core.xpcall(func, table.unpack(info.arg))
    end
end
function dispatch_message(msgtype, msg)
    if MSG_TYPE.STARTED == msgtype then
        math.randomseed(os.time())
        call_static_funcs(static_funcs.STARTED)
    elseif MSG_TYPE.CLOSING == msgtype then
        call_static_funcs(static_funcs.CLOSING)
    elseif MSG_TYPE.TIMEOUT == msgtype then--sess
        local sess = sutils.msg_info(msg)
        local info = timeout[sess]
        timeout[sess] = nil
        info.func(table.unpack(info.arg))
    elseif MSG_TYPE.CONNECT == msgtype then--function(pktype, fd, skid, erro)
        call_static_funcs(static_funcs.CONNECT, sutils.msg_info(msg))
    elseif MSG_TYPE.ACCEPT == msgtype then--function(pktype, fd, skid)
        call_static_funcs(static_funcs.ACCEPT, sutils.msg_info(msg))
    elseif MSG_TYPE.HANDSHAKED == msgtype then--function(pktype, fd, skid)
        call_static_funcs(static_funcs.HANDSHAKED, sutils.msg_info(msg))
    elseif MSG_TYPE.RECV == msgtype then--pktype fd skid data size
        dispatch_recv(sutils.msg_info(msg))
    elseif MSG_TYPE.SEND == msgtype then--function(pktype, fd, skid, size)
        call_static_funcs(static_funcs.SEND, sutils.msg_info(msg))
    elseif MSG_TYPE.CLOSE == msgtype then--function(pktype, fd, skid)
        call_static_funcs(static_funcs.CLOSE, sutils.msg_info(msg))
    elseif MSG_TYPE.RECVFROM == msgtype then--function(fd, skid, data, size, ip, port)
        call_static_funcs(static_funcs.RECVFROM, sutils.msg_info(msg))
    elseif MSG_TYPE.REQUEST == msgtype then--sess src data size
        dispatch_request(sutils.msg_info(msg))
    end
end

return core
