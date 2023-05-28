require("lib.funcs")
local core = require("lib.core")
local srey = require("srey.core")
local log = require("lib.log")
local json = require("cjson")
local msgpack = require("cmsgpack")
local coroutine = coroutine
local os = os
local table = table
local string = string
local strempty = strempty
local NETRPC_TIMEDIFF = NETRPC_TIMEDIFF
local NETRPC_SIGNKEY = NETRPC_SIGNKEY
local cur_coro = nil
local sess_coro = {}
local static_funcs = {}
local rpc_func = {}
local rpc_describe = {}
local normal_timeout = {}
local sleep_timeout = {}
local request_timeout = {}
local connect_timeout = {}
local coro_pool = setmetatable({}, { __mode = "kv" })
local encode = (RPC_USEJSON and json.encode or msgpack.pack)
local decode = (RPC_USEJSON and json.decode or msgpack.unpack)

--[[
描述:任务初始化完成后
参数：
    func function() :function
--]]
function core.started(func)
    assert("function" == type(func), "invalid param type.")
    static_funcs.STARTED = func
end
--[[
描述:程序退出时
参数：
    func function() :function
--]]
function core.closing(func)
    assert("function" == type(func), "invalid param type.")
    static_funcs.CLOSING = func
end
--[[
描述:socket accept
参数：
    func function(unptype :UNPACK_TYPE, fd :integer) :function
--]]
function core.accept(func)
    assert("function" == type(func), "invalid param type.")
    static_funcs.ACCEPT = func
end
--[[
描述:socket recv
参数：
    func function(unptype :UNPACK_TYPE, fd :integer, data :userdata, size :integer) :function
--]]
function core.recv(func)
    assert("function" == type(func), "invalid param type.")
    static_funcs.RECV = func
end
--[[
描述:udp recvfrom
参数：
    func function(unptype :UNPACK_TYPE, fd :integer, data :userdata, size:integer, ip :string, port :integer) :function
--]]
function core.recvfrom(func)
    assert("function" == type(func), "invalid param type.")
    static_funcs.RECVFROM = func
end
--[[
描述:socket sended
参数：
    func function(unptype :UNPACK_TYPE, fd :integer, size :integer) :function
--]]
function core.sended(func)
    assert("function" == type(func), "invalid param type.")
    static_funcs.SEND = func
end
--[[
描述:socket close
参数：
    func function(unptype :UNPACK_TYPE, fd :integer) :function
--]]
function core.closed(func)
    assert("function" == type(func), "invalid param type.")
    static_funcs.CLOSE = func
end
--[[
描述:定时
参数：
    ms 毫秒 :integer
    func function() :function
--]]
function core.timeout(ms, func)
    assert("function" == type(func), "invalid param type.")
    local sess = core.session()
    normal_timeout[sess] = func
    srey.timeout(core.self(), sess, ms)
end
--[[
描述:休眠
参数：
    ms 毫秒 :integer
--]]
function core.sleep(ms)
    local sess = core.session()
    sleep_timeout[sess] = cur_coro
    srey.timeout(core.self(), sess, ms)
    coroutine.yield()
end
--[[
描述:RPC函数注册
参数：
    name 名称 :string
    func function(...) :function
    describe 描述 :string
--]]
function core.regrpc(name, func, describe)
    assert("function" == type(func) and nil == rpc_func[name], "invalid param type or already register.")
    rpc_func[name] = func
    rpc_describe[name] = (nil == describe and "" or describe)
end
--[[
描述:RPC调用，不等待返回
参数：
    task :task_ctx
    name 名称 :string
    ... 调用参数
--]]
function core.call(task, name, ...)
    local info = {
        proto = TASKMSG_TYPE.REQUEST,
        func = name,
        param = {...}}
    srey.user(task, nil, core.session(), encode(info))
end
--[[
描述:RPC调用，等待返回
参数：
    task :task_ctx
    name 名称 :string
    ... 调用参数
返回:
    bool, 被调函数返回值 
--]]
function core.request(task, name, ...)
    local info = {
        proto = TASKMSG_TYPE.REQUEST,
        func = name,
        param = {...}}
    local sess = core.session()
    sess_coro[sess] = cur_coro
    request_timeout[sess] = {
        co = cur_coro,
        info = info}
    srey.timeout(core.self(), sess, REQUEST_TIMEOUT)
    srey.user(task, core.self(), sess, encode(info))
    return coroutine.yield()
end
--验签
local function netrpc_paramstr(info)
    if nil == info.param then
        return ""
    end
    return json.encode(info.param)
end
local function netreq_signstr(info)
    return string.format("%d%d%s%d%s%s",
                         info.proto, info.dst, info.func,
                         info.timestamp, netrpc_paramstr(info), NETRPC_SIGNKEY)
end
local function netresp_signstr(info)
    return string.format("%d%d%s%d%s%s",
                          info.proto, info.sess,
                          tostring(info.ok), info.timestamp, netrpc_paramstr(info), NETRPC_SIGNKEY)
end
local function netrpc_sign(info)
    if strempty(NETRPC_SIGNKEY) then
        return
    end
    info.timestamp = os.time()
    local signstr
    if TASKMSG_TYPE.NETREQ == info.proto then
        signstr = netreq_signstr(info)
    else
        signstr = netresp_signstr(info)
    end
    info.sign = srey.md5(signstr)
end
local function netrpc_signcheck(info)
    if strempty(NETRPC_SIGNKEY) then
        return true
    end
    local signstr
    if TASKMSG_TYPE.NETREQ == info.proto then
        signstr = netreq_signstr(info)
    else
        signstr = netresp_signstr(info)
    end
    if string.lower(info.sign) ~= srey.md5(signstr) then
        return false
    end
    if NETRPC_TIMEDIFF and NETRPC_TIMEDIFF > 0 then
        if math.abs(os.time() - info.timestamp) > NETRPC_TIMEDIFF then
            log.WARN("timestamp check failed.")
            return false
        end
    end
    return true
end
--[[
描述:网络RPC调用，不等待返回
参数：
    fd socket :integer
    task :TASK_NAME
    name 名称 :string
    ... 调用参数
--]]
function core.netcall(fd, task, name, ...)
    if INVALID_SOCK == fd then
        log.WARN("invalid socket.")
        return
    end
    local info = {
        proto = TASKMSG_TYPE.NETREQ,
        dst = task,
        func = name,
        param = {...}}
    netrpc_sign(info)
    core.send(fd, encode(info), nil, PACK_TYPE.RPC)
end
--[[
描述:网络RPC调用，等待返回
参数：
    fd socket :integer
    task :TASK_NAME
    name 名称 :string
    ... 调用参数
返回:
    bool, 被调函数返回值 
--]]
function core.netreq(fd, task, name, ...)
    if INVALID_SOCK == fd then
        log.WARN("invalid socket.")
        return
    end
    local sess = core.session()
    local info = {
        proto = TASKMSG_TYPE.NETREQ,
        sess = sess,
        dst = task,
        src = core.name(),
        func = name,
        param = {...}}
    netrpc_sign(info)
    sess_coro[sess] = cur_coro
    request_timeout[sess] = {
        co = cur_coro,
        info = info}
    srey.timeout(core.self(), sess, NETREQ_TIMEOUT)
    core.send(fd, encode(info), nil, PACK_TYPE.RPC)
    return coroutine.yield()
end

local function rpc_des()
    return json.encode(rpc_describe)
end
core.regrpc("rpc_describe", rpc_des, "show rpc function describe.rpc_des()")
--[[
描述:RPC描述
参数：
    task :task_ctx
返回:
    RPC函数描述 :string
--]]
function core.describe(task)
    local rtn, des = core.request(task, "rpc_describe")
    return rtn and des or ""
end
--[[
描述:connect
参数：
    ip ip :string
    port 端口 :integer
    ssl nil不启用ssl :evssl_ctx
    sendev 是否触发发送事件 :boolean
    unptype :UNPACK_TYPE
返回:
    socket :integer 
    INVALID_SOCK失败
--]]
function core.connect(ip, port, ssl, sendev, unptype)
    local send = 0
    if nil ~= sendev and sendev then
        send = 1
    end
    local sess = core.session()
    local sock = srey.connect(core.self(), nil == unptype and UNPACK_TYPE.NONE or unptype,
                              sess, ssl, ip, port, send)
    if INVALID_SOCK == sock then
        return INVALID_SOCK
    end
    sess_coro[sess] = cur_coro
    connect_timeout[sess] = {
        co = cur_coro,
        sock = sock,
        ip = ip,
        port = port}
    srey.timeout(core.self(), sess, CONNECT_TIMEOUT)
    local _, fd, err = coroutine.yield()
    if ERR_OK ~= err then
        return INVALID_SOCK
    else
        return fd
    end
end
function core.sysinfo()
    local tm = string.format("task:%d, memory:%.2f(kb)", core.name(), collectgarbage("count"))
    local poolsize = string.format("table size: sess_coro:%d, normal_timeout:%d, sleep_timeout:%d, request_timeout:%d, connect_timeout:%d, coro_pool:%d",
                                    tbsize(sess_coro), tbsize(normal_timeout), tbsize(sleep_timeout),
                                    tbsize(request_timeout), tbsize(connect_timeout),tbsize(coro_pool))
    log.INFO("%s %s", tm, poolsize)
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
    USER = 10
}
--协程池
local function co_create(func)
    local co = table.remove(coro_pool)
    if nil == co then
        co = coroutine.create(
            function(...)
                func(...)
                while true do
                    func = nil
                    coro_pool[#coro_pool + 1] = co
                    func = coroutine.yield()
                    func(coroutine.yield())
                end
            end)
    else
        coroutine.resume(co, func)
    end
    return co
end
local function resume_normal(func, ...)
    if nil == func then
        return
    end
    cur_coro = co_create(func)
    coroutine.resume(cur_coro, ...)
end
--超时消息处理
local function dispatch_timeout(sess)
    local param = normal_timeout[sess]
    if nil ~= param then
        normal_timeout[sess] = nil
        resume_normal(param)
        return
    end

    param = request_timeout[sess] --{co = cur_coro, info = info}
    if nil ~= param then
        request_timeout[sess] = nil
        if nil ~= sess_coro[sess] then
            sess_coro[sess] = nil
            cur_coro = param.co
            log.WARN("request timeout. session: %d param: %s.", sess, json.encode(param.info))
            coroutine.resume(cur_coro, false)
        end
        return
    end

    param = connect_timeout[sess]--{co = cur_coro, sock = sock, ip = ip, port = port}
    if nil ~= param then
        connect_timeout[sess] = nil
        if nil ~= sess_coro[sess] then
            sess_coro[sess] = nil
            cur_coro = param.co
            core.close(param.sock)
            log.WARN("connect timeout. session: %d ip: %s port: %d.", sess, param.ip, param.port)
            coroutine.resume(cur_coro, nil, param.sock, ERR_FAILED)
        end
        return
    end

    param = sleep_timeout[sess]
    if nil ~= param then
        sleep_timeout[sess] = nil
        cur_coro = param
        coroutine.resume(cur_coro)
        return
    end
end
--RPC_REQUEST
local function rpc_request(src, sess, info)
    local func = rpc_func[info.func]
    local resp = {proto = TASKMSG_TYPE.RESPONSE}
    if nil == func then
        resp.ok = false
        log.WARN("not find rpc method. request: %s.", json.encode(info))
    else
        resp.ok = true
        resp.param = {func(table.unpack(info.param))}
    end
    if nil ~= src then
        srey.user(src, nil, sess, encode(resp))
    end
end
--自定义消息处理
local function dispatch_user(src, data, size, sess)
    local info = decode(data, size)
    if TASKMSG_TYPE.REQUEST == info.proto then
        resume_normal(rpc_request, src, sess, info)
    elseif TASKMSG_TYPE.RESPONSE == info.proto then
        local co = sess_coro[sess]
        if nil ~= co then
            sess_coro[sess] = nil
            request_timeout[sess] = nil
            cur_coro = co
            if info.ok then
                coroutine.resume(cur_coro, info.ok, table.unpack(info.param))
            else
                coroutine.resume(cur_coro, info.ok)
            end
        else
            log.WARN("not find response session, maybe timeout. response: %s.", json.encode(info))
        end
    end
end
--远程RPC
local function rpc_netreq(fd, info)
    if not netrpc_signcheck(info) then
        core.close(fd)
        log.WARN("netrpc request check failed. request: %s.", json.encode(info))
        return
    end
    local task = core.qury(info.dst)
    if nil == task then
        log.WARN("netrpc request not find task. request: %s.", json.encode(info))
        if nil ~= info.src then
            local resp = {
                proto = TASKMSG_TYPE.NETRESP,
                sess = info.sess,
                ok = false
            }
            netrpc_sign(resp)
            core.send(fd, encode(resp), nil, PACK_TYPE.RPC)
        end
        return
    end
    if nil == info.src then
        core.call(task, info.func, table.unpack(info.param))
    else
        local resp = {
            proto = TASKMSG_TYPE.NETRESP,
            sess = info.sess
        }
        local rtn = {core.request(task, info.func, table.unpack(info.param))}
        resp.ok = table.remove(rtn, 1)
        if resp.ok then
            resp.param = rtn
        end
        netrpc_sign(resp)
        core.send(fd, encode(resp), nil, PACK_TYPE.RPC)
    end
end
local function dispatch_netrpc(fd, data, size)
    local info = decode(data, size)
    if TASKMSG_TYPE.NETREQ == info.proto then
        resume_normal(rpc_netreq, fd, info)
    else
        if not netrpc_signcheck(info) then
            core.close(fd)
            log.WARN("netrpc check response failed. response: %s.", json.encode(info))
            return
        end
        local co = sess_coro[info.sess]
        if nil ~= co then
            sess_coro[info.sess] = nil
            request_timeout[info.sess] = nil
            cur_coro = co
            if info.ok then
                coroutine.resume(cur_coro, info.ok, table.unpack(info.param))
            else
                coroutine.resume(cur_coro, info.ok)
            end
        else
            log.WARN("netrpc not find response session, maybe timeout. response: %s.", json.encode(info))
        end
    end
end
--消息处理
function dispatch_message(msgtype, unptype, err, fd, src, data, size, sess, addr)
    if MSG_TYPE.STARTED == msgtype then
        resume_normal(static_funcs.STARTED)
    elseif MSG_TYPE.CLOSING == msgtype then
        resume_normal(static_funcs.CLOSING)
        core.sysinfo()
    elseif MSG_TYPE.TIMEOUT == msgtype then
        dispatch_timeout(sess)
    elseif MSG_TYPE.ACCEPT == msgtype then
        resume_normal(static_funcs.ACCEPT, unptype, fd)
    elseif MSG_TYPE.CONNECT == msgtype then
        local co = sess_coro[sess]
        if nil ~= co then
            sess_coro[sess] = nil
            cur_coro = co
            coroutine.resume(cur_coro, unptype, fd, err)
        else
            log.WARN("not find connect session, maybe timeout.session: %d.", sess)
        end
    elseif MSG_TYPE.RECV == msgtype then
        if UNPACK_TYPE.RPC == unptype then
            dispatch_netrpc(fd, data, size)
        else
            resume_normal(static_funcs.RECV, unptype, fd, data, size)
        end
    elseif MSG_TYPE.SEND == msgtype then
        resume_normal(static_funcs.SEND, unptype, fd, size)
    elseif MSG_TYPE.CLOSE == msgtype then
        resume_normal(static_funcs.CLOSE, unptype, fd)
    elseif MSG_TYPE.RECVFROM == msgtype then
        local ip, port = core.ipport(addr)
        resume_normal(static_funcs.RECVFROM, unptype, fd, data, size, ip, port)
    elseif MSG_TYPE.USER == msgtype then
        dispatch_user(src, data, size, sess)
    end
end

return core
