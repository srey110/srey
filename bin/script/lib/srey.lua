require("lib.funcs")
local sutils = require("srey.utils")
local core = require("lib.core")
local json = require("cjson")
local msgpack = require("cmsgpack")
local table = table
local rpc_func = {}--rpc 函数
local static_funcs = {}
local timeout = {}
local chunked = {}
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
    func function(pktype :PACK_TYPE, fd :integer) :function
--]]
function core.accepted(func)
    static_funcs.ACCEPT = func
end
--[[
描述:socket connect
参数:
    func function(pktype :PACK_TYPE, fd :integer, erro :integer) :function
--]]
function core.connected(func)
    static_funcs.CONNECT = func
end
--[[
描述:socket recv
参数:
    func function(pktype :PACK_TYPE, fd :integer, data :userdata, size :integer) :function
--]]
function core.recved(func)
    static_funcs.RECV = func
end
--[[
描述:udp recvfrom
参数:
    func function(fd :integer, data :userdata, size :integer, 
                  ip :string, port :integer) :function
--]]
function core.recvfromed(func)
    static_funcs.RECVFROM = func
end
--[[
描述:socket sended
参数:
    func function(pktype :PACK_TYPE, fd :integer, size :integer) :function
--]]
function core.sended(func)
    static_funcs.SEND = func
end
--[[
描述:socket close
参数:
    func function(pktype :PACK_TYPE, fd :integer) :function
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
    local sess = core.task_session()
    timeout[sess] = {
        func = func,
        arg = {...}
    }
    sutils.timeout(core.self(), sess, ms)
end
--[[
描述:设置处理http chunked包函数
参数:
    fd :integer
    ckfunc function(fd :integer, data :userdata, size :integer) :function
--]]
function core.http_chunked(fd, ckfunc)
    chunked[fd] = ckfunc
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
    task :TASK_NAME
    name 名称 :string
    ... 调用参数
--]]
function core.netcall(fd, task, name, ...)
    local info = {
        dst = task,
        func = name,
        arg = {...}
    }
    core.send(fd, encode(info), nil, PACK_TYPE.RPC)
end
--[[
描述:远程RPC调用，等待返回
参数:
    fd socket :integer
    task :TASK_NAME
    name 名称 :string
    ... 调用参数
返回:
    bool, 被调函数返回值 
--]]
function core.netreq(fd, task, name, ...)
    if INVALID_SOCK == fd then
        return false
    end
    local info = {
        dst = task,
        src = core.task_name(core.self()),
        func = name,
        arg = {...}
    }
    local resp, _ = core.synsend(fd, encode(info), nil, PACK_TYPE.RPC)
    if nil == resp then
        return false
    end
    local data = sutils.simple_pack(resp, 1)
    info = decode(data.data, data.size)
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
    RECV = 6,
    SEND = 7,
    CLOSE = 8,
    RECVFROM = 9,
    REQUEST = 10
}
local function call_static_funcs(func, ...)
    if not func then
        return
    end
    func(...)
end
local function rpc_netreq(msg)
    local data = sutils.simple_pack(msg.data, 1)
    local info = decode(data.data, data.size)
    local dst = core.task_qury(info.dst)
    if nil == dst then
        if nil ~= info.src then
            local resp = {}
            resp.ok = false
            core.send(msg.fd, encode(resp), nil, PACK_TYPE.RPC)
        end
        return
    end
    if nil == info.src then
        core.call(dst, info.func, table.unpack(info.arg))
    else
        local resp = {}
        resp.arg = {core.request(dst, info.func, table.unpack(info.arg))}
        resp.ok = table.remove(resp.arg, 1)
        core.send(msg.fd, encode(resp), nil, PACK_TYPE.RPC)
    end
end
local function dispatch_recv(msg)
    if PACK_TYPE.HTTP == msg.pktype then
        local func = chunked[msg.fd]
        if func then
            local hpack = sutils.http_pack(msg.data, 1)
            if not hpack.data or 0 == hpack.size then
                chunked[msg.fd] = nil
            end
            func(msg.fd, hpack.data, hpack.size)
            return
        end
    elseif PACK_TYPE.RPC == msg.pktype then
        rpc_netreq(msg)
        return
    end
    call_static_funcs(static_funcs.RECV, msg.pktype, msg.fd, msg.data, msg.size)
end
local function dispatch_request(msg)
    local info = decode(msg.data, msg.size)
    local func = rpc_func[info.func]
    if nil == func then
        if -1 ~=  msg.src then
            local resp = {}
            resp.ok = false
            sutils.task_response(core.task_qury(msg.src), msg.sess, encode(resp))
        end
        return
    end
    if -1 ~= msg.src then
        local resp = {}
        resp.arg = {core.xpcall(func, table.unpack(info.arg))}
        resp.ok = table.remove(resp.arg, 1)
        sutils.task_response(core.task_qury(msg.src), msg.sess, encode(resp))
    else
        core.xpcall(func, table.unpack(info.arg))
    end
end
function dispatch_message(msg)
    if MSG_TYPE.STARTED == msg.msgtype then
        collectgarbage("generational")
        math.randomseed(os.time())
        call_static_funcs(static_funcs.STARTED)
    elseif MSG_TYPE.CLOSING == msg.msgtype then
        call_static_funcs(static_funcs.CLOSING)
    elseif MSG_TYPE.TIMEOUT == msg.msgtype then
        local info = timeout[msg.sess]
        timeout[msg.sess] = nil
        info.func(table.unpack(info.arg))
    elseif MSG_TYPE.CONNECT == msg.msgtype then--pktype fd err
        call_static_funcs(static_funcs.CONNECT, msg.pktype, msg.fd, msg.err)
    elseif MSG_TYPE.ACCEPT == msg.msgtype then--pktype fd
        call_static_funcs(static_funcs.ACCEPT, msg.pktype, msg.fd)
    elseif MSG_TYPE.SEND == msg.msgtype then--pktype fd  size
        call_static_funcs(static_funcs.SEND, msg.pktype, msg.fd, msg.size)
    elseif MSG_TYPE.CLOSE == msg.msgtype then--pktype fd
        call_static_funcs(static_funcs.CLOSE, msg.pktype, msg.fd)
    elseif MSG_TYPE.RECV == msg.msgtype then--pktype fd data size
        dispatch_recv(msg)
    elseif MSG_TYPE.RECVFROM == msg.msgtype then--pktype fd data size ip port 
        call_static_funcs(static_funcs.RECVFROM, msg.fd,
                          msg.data, msg.size, msg.ip, msg.port)
    elseif MSG_TYPE.REQUEST == msg.msgtype then--sess src data size
        dispatch_request(msg)
    end
end

return core
