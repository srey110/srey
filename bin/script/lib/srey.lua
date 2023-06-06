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
local cur_coro = nil
local static_funcs = {}--回调
local rpc_func = {}--rpc 函数
local rpc_describe = {}--rpc描述
local sess_coro = {} --core.sleep core.request core.netreq core.connect  sess_coro[sess] = cur_coro 
local synsock_coro = {} --core.synsend synsock_coro[fd] = {co = cur_coro, sess = sess}
local synssock_timeout = {} --core.synsend synssock_timeout[sess] = fd
local normal_timeout = {}--core.timeout normal_timeout[sess] = func
local request_timeout = {}--core.request core.netreq request_timeout[sess] = info
local request_sock = {}--core.netreq {fd = {sess...}}
local connect_timeout = {}--core.connect connect_timeout[sess] = {sock = sock, ip = ip, port = port}
local coro_pool = setmetatable({}, { __mode = "kv" })
local encode = (RPC_USEJSON and json.encode or msgpack.pack)
local decode = (RPC_USEJSON and json.decode or msgpack.unpack)
local monitor_tb = {sess_coro = sess_coro, synsock_coro = synsock_coro, synssock_timeout = synssock_timeout,
                    normal_timeout = normal_timeout, request_timeout = request_timeout,
                    request_sock = request_sock, connect_timeout = connect_timeout,
                    coro_pool = coro_pool}
local TASKMSG_TYPE = {
    REQUEST = 0x01,
    RESPONSE = 0x02,
    NETREQ = 0x03,
    NETRESP = 0x04,
}

--[[
描述:任务初始化完成后
参数：
    func function() :function
--]]
function core.started(func)
    static_funcs.STARTED = func
end
--[[
描述:程序退出时
参数：
    func function() :function
--]]
function core.closing(func)
    static_funcs.CLOSING = func
end
--[[
描述:socket accept
参数：
    func function(pktype :PACK_TYPE, fd :integer) :function
--]]
function core.accepted(func)
    static_funcs.ACCEPT = func
end
--[[
描述:socket recv
参数：
    func function(pktype :PACK_TYPE, fd :integer, data) :function
--]]
function core.recved(func)
    static_funcs.RECV = func
end
--[[
描述:udp recvfrom
参数：
    func function(pktype :PACK_TYPE, fd :integer, data, ip :string, port :integer) :function
--]]
function core.recvfromed(func)
    static_funcs.RECVFROM = func
end
--[[
描述:socket sended
参数：
    func function(pktype :PACK_TYPE, fd :integer, size :integer) :function
--]]
function core.sended(func)
    static_funcs.SEND = func
end
--[[
描述:socket close
参数：
    func function(pktype :PACK_TYPE, fd :integer) :function
--]]
function core.closed(func)
    static_funcs.CLOSE = func
end
--[[
描述:socket 应答模式 发送后等待数据返回
参数：
    fd socket :integer
    data lstring或userdata
    lens data长度 :integer
    ptype :PACK_TYPE
    ckfunc func(data) :function
返回:
    bool; data 
--]]
function core.synsend(fd, data, lens, ptype, func)
    if INVALID_SOCK == fd or nil == data then
        log.WARN("invalid argument.")
        return false
    end
    if nil ~= synsock_coro[fd] then
        log.WARN("sock %d waiting response already.", fd)
        return false
    end
    local sess = core.session()
    synsock_coro[fd] = {
        co = cur_coro,
        sess = sess,
        func = func
    }
    synssock_timeout[sess] = fd
    srey.timeout(core.self(), sess, NETRD_TIMEOUT)
    core.send(fd, data, lens, ptype)
    return coroutine.yield()
end
--[[
描述:定时
参数：
    ms 毫秒 :integer
    func function() :function
--]]
function core.timeout(ms, func)
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
    sess_coro[sess] = cur_coro
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
    if rpc_func[name] then
        log.WARN ("%s already register.", name)
        return
    end
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
        param = {...}
    }
    srey.user(task, -1, core.session(), encode(info))
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
        param = {...}
    }
    local sess = core.session()
    sess_coro[sess] = cur_coro
    request_timeout[sess] = info
    srey.timeout(core.self(), sess, RPCREQ_TIMEOUT)
    srey.user(task, core.name(core.self()), sess, encode(info))
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
    return string.format("%d%d%d%d%s%s%s",
                         info.proto, info.sock, info.dst, info.timestamp, info.func,
                         netrpc_paramstr(info), NETRPC_SIGNKEY)
end
local function netresp_signstr(info)
    return string.format("%d%d%d%d%s%s%s",
                          info.proto, info.sock, info.sess, info.timestamp,
                          tostring(info.ok), netrpc_paramstr(info), NETRPC_SIGNKEY)
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
    info.sign = core.md5(signstr)
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
    if string.lower(info.sign) ~= core.md5(signstr) then
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
        log.WARN("invalid argument.")
        return
    end
    local info = {
        proto = TASKMSG_TYPE.NETREQ,
        sock = fd,
        dst = task,
        func = name,
        param = {...}
    }
    netrpc_sign(info)
    core.send(fd, encode(info), nil, PACK_TYPE.RPC)
end
local function request_sock_add(fd, sess)
    local info = request_sock[fd]
    if nil == info then
        info = {}
        request_sock[fd] = info
    end
    table.insert(info, sess)
end
local function request_sock_remove(fd, sess)
    local info = request_sock[fd]
    if nil == info then
        return
    end
    for index, value in ipairs(info) do
        if sess == value then
            table.remove(info, index)
            break
        end
    end
end
local function request_sock_closed(fd)
    local info = request_sock[fd]
    if nil == info then
        return
    end
    for _, value in ipairs(info) do
        local co = sess_coro[value]
        sess_coro[value] = nil
        request_timeout[value] = nil
        if nil ~= co then
            cur_coro = co
            coroutine.resume(cur_coro, false)
        end
    end
    request_sock[fd] = nil
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
        log.WARN("invalid argument.")
        return false
    end
    local sess = core.session()
    local info = {
        proto = TASKMSG_TYPE.NETREQ,
        sock = fd,
        sess = sess,
        dst = task,
        src = core.name(),
        func = name,
        param = {...}
    }
    netrpc_sign(info)
    sess_coro[sess] = cur_coro
    request_timeout[sess] = info
    request_sock_add(fd, sess)
    srey.timeout(core.self(), sess, NETRD_TIMEOUT)
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
    if nil == task then
        return rpc_des()
    end
    local rtn, des = core.request(task, "rpc_describe")
    return rtn and des or ""
end
--[[
描述:connect
参数：
    ip ip :string
    port 端口 :integer
    pktype :PACK_TYPE
    ssl nil不启用ssl :evssl_ctx
    sendev 是否触发发送事件 :boolean
返回:
    socket :integer 
    INVALID_SOCK失败
--]]
function core.connect(ip, port, pktype, ssl, sendev)
    local send = 0
    if nil ~= sendev and sendev then
        send = 1
    end
    local sess = core.session()
    local sock = srey.connect(core.self(), pktype or PACK_TYPE.NONE, sess, ssl, ip, port, send)
    if INVALID_SOCK == sock then
        return INVALID_SOCK
    end
    sess_coro[sess] = cur_coro
    connect_timeout[sess] = {
        sock = sock,
        ip = ip,
        port = port
    }
    srey.timeout(core.self(), sess, CONNECT_TIMEOUT)
    local err = coroutine.yield()
    if ERR_OK ~= err then
        return INVALID_SOCK
    else
        return sock
    end
end
function core.sysinfo()
    local tm = string.format("task:%d, memory:%.2f(kb) ", core.name(), collectgarbage("count"))
    local sizes = {}
    for key, value in pairs(monitor_tb) do
        table.insert(sizes, string.format("%s: %d, ", key, tbsize(value)))
    end
    log.INFO("%s %s", tm, table.concat(sizes))
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
                core.xpcall(func, ...)
                while true do
                    func = nil
                    coro_pool[#coro_pool + 1] = co
                    func = coroutine.yield()
                    core.xpcall(func, coroutine.yield())
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
    --core.timeout(ms, func)
    local param = normal_timeout[sess] --func
    if nil ~= param then
        normal_timeout[sess] = nil
        resume_normal(param)
        return
    end
    --core.request(task, name, ...) core.netreq(fd, task, name, ...)
    param = request_timeout[sess] --info
    if nil ~= param then
        request_timeout[sess] = nil
        local co = sess_coro[sess]
        if nil ~= co then
            local sock = param.sock
            if nil ~= sock then
                request_sock_remove(sock, sess)
            end
            sess_coro[sess] = nil
            cur_coro = co
            log.WARN("request timeout. param: %s.", json.encode(param))
            coroutine.resume(cur_coro, false)
        end
        return
    end
    --core.connect(ip, port, pktype, ssl, sendev)
    param = connect_timeout[sess]--{sock = sock, ip = ip, port = port}
    if nil ~= param then
        connect_timeout[sess] = nil
        local co = sess_coro[sess]
        if nil ~= co then
            sess_coro[sess] = nil
            cur_coro = co
            core.close(param.sock)
            log.WARN("connect timeout. session: %d ip: %s port: %d.", sess, param.ip, param.port)
            coroutine.resume(cur_coro, nil, param.sock, ERR_FAILED)
        end
        return
    end
    --core.synsend(fd, data, lens, ptype)
    param = synssock_timeout[sess] -- fd
    if nil ~= param then
        synssock_timeout[sess] = nil
        local info = synsock_coro[param] --{co = cur_coro, sess = sess, func = func}
        if nil ~= info then
            synsock_coro[param] = nil
            cur_coro = info.co
            log.WARN("synsend timeout. socket: %d.", param)
            coroutine.resume(cur_coro, false)
        end
        return
    end
    --core.sleep(ms) --co
    param = sess_coro[sess]
    if nil ~= param then
        sess_coro[sess] = nil
        cur_coro = param
        coroutine.resume(cur_coro)
        return
    end
end
--RPC_REQUEST
local function rpc_request(sess, src, info)
    local func = rpc_func[info.func]
    local resp = {proto = TASKMSG_TYPE.RESPONSE}
    if nil == func then
        resp.ok = false
        log.WARN("not find method: %s.", info.func)
    else
        resp.param = {core.xpcall(func, table.unpack(info.param))}
        resp.ok = table.remove(resp.param, 1)
    end
    if -1 ~= src then
        srey.user(core.qury(src), -1, sess, encode(resp))
    end
end
--自定义消息处理 msgtype sess src data size
local function dispatch_user(msg)
    local pack = decode(msg.data, msg.size)
    if TASKMSG_TYPE.REQUEST == pack.proto then
        resume_normal(rpc_request, msg.sess, msg.src, pack)
    elseif TASKMSG_TYPE.RESPONSE == pack.proto then
        local co = sess_coro[msg.sess]
        if nil ~= co then
            sess_coro[msg.sess] = nil
            request_timeout[msg.sess] = nil
            cur_coro = co
            if pack.ok then
                coroutine.resume(cur_coro, pack.ok, table.unpack(pack.param))
            else
                coroutine.resume(cur_coro, pack.ok)
            end
        end
    end
end
--远程RPC
local function rpc_netreq(fd, pack)
    if not netrpc_signcheck(pack) then
        core.close(fd)
        log.WARN("netrpc request sign check failed.")
        return
    end
    local task = core.qury(pack.dst)
    if nil == task then
        log.WARN("netrpc request not find task: %d.", pack.dst)
        if nil ~= pack.src then
            local resp = {
                proto = TASKMSG_TYPE.NETRESP,
                sock = pack.sock,
                sess = pack.sess,
                ok = false
            }
            netrpc_sign(resp)
            core.send(fd, encode(resp), nil, PACK_TYPE.RPC)
        end
        return
    end
    if nil == pack.src then
        core.call(task, pack.func, table.unpack(pack.param))
    else
        local resp = {
            proto = TASKMSG_TYPE.NETRESP,
            sock = pack.sock,
            sess = pack.sess
        }
        resp.param = {core.request(task, pack.func, table.unpack(pack.param))}
        resp.ok = table.remove(resp.param, 1)
        netrpc_sign(resp)
        core.send(fd, encode(resp), nil, PACK_TYPE.RPC)
    end
end
local function dispatch_netrpc(fd, pack)
    if TASKMSG_TYPE.NETREQ == pack.proto then
        resume_normal(rpc_netreq, fd, pack)
    else
        if not netrpc_signcheck(pack) then
            core.close(fd)
            log.WARN("netrpc response sign check failed.")
            return
        end
        local co = sess_coro[pack.sess]
        if nil ~= co then
            sess_coro[pack.sess] = nil
            request_timeout[pack.sess] = nil
            request_sock_remove(pack.sock, pack.sess)
            cur_coro = co
            if pack.ok then
                coroutine.resume(cur_coro, pack.ok, table.unpack(pack.param))
            else
                coroutine.resume(cur_coro, pack.ok)
            end
        end
    end
end
local function synsock_resume(fd, coinfo, result, ...)
    synsock_coro[fd] = nil
    synssock_timeout[coinfo.sess] = nil
    cur_coro = coinfo.co
    coroutine.resume(cur_coro, result, ...)
end
--http 
--pack: {chunked, data, status{}, head{}} 
--coinfo: {co, sess, func(data)}
local function dispatch_revc_http(fd, coinfo, pack)
    local chunked = pack.chunked
    if 1 == chunked then
        synssock_timeout[coinfo.sess] = nil
        coinfo.chunked = pack
        if nil == coinfo.func then
            coinfo.chunked.data = {}
        end
    elseif 2 == chunked then
        local over = false
        if nil ~= coinfo.func then
            if 0 == #pack.data then
                over = true
            end
            core.xpcall(coinfo.func, pack.data)
        else
            if 0 == #pack.data then
                over = true
            end
            table.insert(coinfo.chunked.data, pack.data)
        end
        if over then
            synsock_resume(fd, coinfo, true, coinfo.chunked)
        end
    else
        synsock_resume(fd, coinfo, true, pack)
    end
end
--tcp消息 pktype fd data size
local function dispatch_revc(msg)
    local pack
    if PACK_TYPE.RPC == msg.pktype then
        pack = decode(msg.data, msg.size)
    else
        pack = msg.data
    end
    --socket 应答模式(core.synsend) coinfo{co, sess, func(data)}
    local coinfo = synsock_coro[msg.fd]
    if nil ~= coinfo then
        if PACK_TYPE.HTTP == msg.pktype then
            dispatch_revc_http(msg.fd, coinfo, pack)
        else
            synsock_resume(msg.fd, coinfo, true, pack)
        end
    else
        if PACK_TYPE.RPC == msg.pktype then
            dispatch_netrpc(msg.fd, pack)
        else
            resume_normal(static_funcs.RECV, msg.pktype, msg.fd, pack)
        end
    end
end
--udp消息 msgtype pktype fd ip port data size
local function dispatch_revcfrom(msg)
    local pack
    if PACK_TYPE.RPC == msg.pktype then
        pack = decode(msg.data, msg.size)
    else
        pack = msg.data
    end
    resume_normal(static_funcs.RECVFROM, msg.pktype, msg.fd, pack, msg.ip, msg.port)
end
--消息处理 所有都有 msgtype
function dispatch_message(msg)
    if MSG_TYPE.STARTED == msg.msgtype then
        collectgarbage("generational")
        math.randomseed(os.time())
        resume_normal(static_funcs.STARTED)
    elseif MSG_TYPE.CLOSING == msg.msgtype then
        resume_normal(static_funcs.CLOSING)
        core.sysinfo()
    elseif MSG_TYPE.TIMEOUT == msg.msgtype then
        dispatch_timeout(msg.sess)
    elseif MSG_TYPE.CONNECT == msg.msgtype then--sess error
        local co = sess_coro[msg.sess]
        if nil ~= co then
            sess_coro[msg.sess] = nil
            connect_timeout[msg.sess] = nil
            cur_coro = co
            coroutine.resume(cur_coro, msg.err)
        else
            log.WARN("not find connect session, maybe timeout.session: %d.", msg.sess)
        end
    elseif MSG_TYPE.ACCEPT == msg.msgtype then--pktype fd
        resume_normal(static_funcs.ACCEPT, msg.pktype, msg.fd)
    elseif MSG_TYPE.SEND == msg.msgtype then--pktype fd  size
        resume_normal(static_funcs.SEND, msg.pktype, msg.fd, msg.size)
    elseif MSG_TYPE.CLOSE == msg.msgtype then--pktype fd
        local coinfo = synsock_coro[msg.fd]
        if nil ~= coinfo then
            synsock_resume(msg.fd, coinfo, false)
        end
        if PACK_TYPE.RPC == msg.pktype then
            request_sock_closed(msg.fd)
        end
        resume_normal(static_funcs.CLOSE, msg.pktype, msg.fd)
    elseif MSG_TYPE.RECV == msg.msgtype then
        dispatch_revc(msg)
    elseif MSG_TYPE.RECVFROM == msg.msgtype then
        dispatch_revcfrom(msg)
    elseif MSG_TYPE.USER == msg.msgtype then
        dispatch_user(msg)
    end
end

return core
