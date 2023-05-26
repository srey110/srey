local core = require("lib.core")
local srey = require("srey.core")
local log = require("lib.log")
local json = require("cjson")
local msgpack = require("cmsgpack")
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

--[[
描述:任务初始化完成后
参数：
    func function() :function
--]]
function core.started(func)
    assert("function" == type(func), "param type error.")
    static_funcs.STARTED = func
end
--[[
描述:程序退出时
参数：
    func function() :function
--]]
function core.closing(func)
    assert("function" == type(func), "param type error.")
    static_funcs.CLOSING = func
end
--[[
描述:socket accept
参数：
    func function(unptype :UNPACK_TYPE, fd :integer) :function
--]]
function core.accept(func)
    assert("function" == type(func), "param type error.")
    static_funcs.ACCEPT = func
end
--[[
描述:socket recv
参数：
    func function(unptype :UNPACK_TYPE, fd :integer, data :userdata, size :integer) :function
--]]
function core.recv(func)
    assert("function" == type(func), "param type error.")
    static_funcs.RECV = func
end
--[[
描述:udp recvfrom
参数：
    func function(unptype :UNPACK_TYPE, fd :integer, data :userdata, size:integer, ip :string, port :integer) :function
--]]
function core.recvfrom(func)
    assert("function" == type(func), "param type error.")
    static_funcs.RECVFROM = func
end
--[[
描述:socket sended
参数：
    func function(unptype :UNPACK_TYPE, fd :integer, size :integer) :function
--]]
function core.sended(func)
    assert("function" == type(func), "param type error.")
    static_funcs.SEND = func
end
--[[
描述:socket close
参数：
    func function(unptype :UNPACK_TYPE, fd :integer) :function
--]]
function core.closed(func)
    assert("function" == type(func), "param type error.")
    static_funcs.CLOSE = func
end
--[[
描述:定时
参数：
    ms 毫秒 :integer
    func function() :function
--]]
function core.timeout(ms, func)
    assert("function" == type(func), "param type error.")
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
    assert("function" == type(func) and nil == rpc_func[name], "param type error or already register.")
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
    local info = {proto = USERMSG_TYPE.RPC_REQUEST, func = name, param = {...}}
    local sess = core.session()
    srey.user(task, nil, sess, msgpack.pack(info))
end
--[[
描述:RPC调用，等待返回
参数：
    task :task_ctx
    name 名称 :string
    ... 调用参数
返回:
    被调函数返回值 
    nil失败
--]]
function core.request(task, name, ...)
    local info = {proto = USERMSG_TYPE.RPC_REQUEST, func = name, param = {...}}
    local sess = core.session()
    sess_coro[sess] = cur_coro
    if MONITOR_TIMEOUT then
        request_timeout[sess] = {co = cur_coro, info = info}
        srey.timeout(core.self(), sess, REQUEST_TIMEOUT)
    end
    srey.user(task, core.self(), sess, msgpack.pack(info))
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
    return core.request(task, "rpc_describe")
end
--[[
描述:connect
参数：
    ip ip :string
    port 端口 :integer
    ssl nil不启用ssl :evssl_ctx
    sendev 0不触发 1触发 :integer
    unptype :UNPACK_TYPE
返回:
    socket :integer 
    INVALID_SOCK失败
--]]
function core.connect(ip, port, ssl, sendev, unptype)
    local sess = core.session()
    local sock = srey.connect(core.self(), nil == unptype and UNPACK_TYPE.NONE or unptype,
                              sess, ssl, ip, port, nil == sendev and 0 or sendev)
    if INVALID_SOCK == sock then
        return INVALID_SOCK
    end
    sess_coro[sess] = cur_coro
    connect_timeout[sess] = {co = cur_coro, sock = sock, ip = ip, port = port}
    srey.timeout(core.self(), sess, CONNECT_TIMEOUT)
    local _, fd, err = coroutine.yield()
    if ERR_OK ~= err then
        return INVALID_SOCK
    else
        return fd
    end
end

local function poolsize(tb)
    local cnt = 0
    for _, _ in pairs(tb) do
        cnt = cnt + 1
    end
    return cnt
end
local function log_poolsize()
    local msg = string.format("task:%d, sess_coro:%d, normal_timeout:%d, sleep_timeout:%d, request_timeout:%d, connect_timeout:%d, coro_pool:%d",
                              core.name(), poolsize(sess_coro), poolsize(normal_timeout), poolsize(sleep_timeout),
                              poolsize(request_timeout), poolsize(connect_timeout),poolsize(coro_pool))
    log.INFO(msg)
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
--超时消息处理
local function dispatch_timeout(sess)
    local co
    local func = normal_timeout[sess]
    if nil ~= func then
        normal_timeout[sess] = nil
        co = co_create(func)
        cur_coro = co
        coroutine.resume(co)
        return
    end

    if MONITOR_TIMEOUT then
        co = request_timeout[sess]
        if nil ~= co then
            request_timeout[sess] = nil
            if nil ~= sess_coro[sess] then
                sess_coro[sess] = nil
                cur_coro = co.co
                log.WARN(string.format("request timeout. session: %s param: %s",
                                        tostring(sess), json.encode(co.info)))
                coroutine.resume(co.co, nil)
            end
            return
        end
    end

    co = connect_timeout[sess]
    if nil ~= co then
        connect_timeout[sess] = nil
        if nil ~= sess_coro[sess] then
            sess_coro[sess] = nil
            cur_coro = co.co
            core.close(co.sock)
            log.WARN(string.format("connect timeout. session: %s ip: %s port: %s",
                                    tostring(sess), co.ip, tostring(co.port)))
            coroutine.resume(co.co, nil, co.sock, ERR_FAILED)
        end
        return
    end

    co = sleep_timeout[sess]
    if nil ~= co then
        sleep_timeout[sess] = nil
        cur_coro = co
        coroutine.resume(co)
        return
    end
end
--RPC_REQUEST处理
local function rpc_request(src, sess, info)
    local func = rpc_func[info.func]
    local resp = {proto = USERMSG_TYPE.RPC_RESPONSE}
    if nil == func then
        resp.ok = false
    else
        resp.ok = true
        resp.param = {func(table.unpack(info.param))}
    end
    if nil ~= src then
        srey.user(src, nil, sess, msgpack.pack(resp))
    end
end
--自定义消息处理
local function dispatch_user(src, data, size, sess)
    local info = msgpack.unpack(data, size)
    if USERMSG_TYPE.RPC_REQUEST == info.proto then
        local co = co_create(rpc_request)
        cur_coro = co
        coroutine.resume(co, src, sess, info)
    elseif USERMSG_TYPE.RPC_RESPONSE == info.proto then
        local co = sess_coro[sess]
        if nil ~= co then
            sess_coro[sess] = nil
            if MONITOR_TIMEOUT then
                request_timeout[sess] = nil
            end
            cur_coro = co
            if info.ok then
                coroutine.resume(co, table.unpack(info.param))
            else
                coroutine.resume(co, nil)
            end
        else
            log.WARN(string.format("not find response session, maybe timeout.session: %s response: %s",
                                   tostring(sess), json.encode(info)))
        end
    end
end
--消息处理
function dispatch_message(msgtype, unptype, err, fd, src, data, size, sess, addr)
    if MSG_TYPE.STARTED == msgtype then
        local func = static_funcs.STARTED
        if nil ~= func then
            local co = co_create(func)
            cur_coro = co
            coroutine.resume(co)
        end
    elseif MSG_TYPE.CLOSING == msgtype then
        local func = static_funcs.CLOSING
        if nil ~= func then
            local co = co_create(func)
            cur_coro = co
            coroutine.resume(co)
        end
        log_poolsize()
    elseif MSG_TYPE.TIMEOUT == msgtype then
        dispatch_timeout(sess)
    elseif MSG_TYPE.ACCEPT == msgtype then
        local func = static_funcs.ACCEPT
        if nil ~= func then
            local co = co_create(func)
            cur_coro = co
            coroutine.resume(co, unptype, fd)
        end
    elseif MSG_TYPE.CONNECT == msgtype then
        local co = sess_coro[sess]
        if nil ~= co then
            sess_coro[sess] = nil
            cur_coro = co
            coroutine.resume(co, unptype, fd, err)
        else
            log.WARN(string.format("not find connect session, maybe timeout.session: %s", tostring(sess)))
        end
    elseif MSG_TYPE.RECV == msgtype then
        local func = static_funcs.RECV
        if nil ~= func then
            local co = co_create(func)
            cur_coro = co
            coroutine.resume(co, unptype, fd, data, size)
        end
    elseif MSG_TYPE.SEND == msgtype then
        local func = static_funcs.SEND
        if nil ~= func then
            local co = co_create(func)
            cur_coro = co
            coroutine.resume(co, unptype, fd, size)
        end
    elseif MSG_TYPE.CLOSE == msgtype then
        local func = static_funcs.CLOSE
        if nil ~= func then
            local co = co_create(func)
            cur_coro = co
            coroutine.resume(co, unptype, fd)
        end
    elseif MSG_TYPE.RECVFROM == msgtype then
        local func = static_funcs.RECVFROM
        if nil ~= func then
            local co = co_create(func)
            cur_coro = co
            local ip, port = core.ipport(addr)
            coroutine.resume(co, unptype, fd, data, size, ip, port)
        end
    elseif MSG_TYPE.USER == msgtype then
        dispatch_user(src, data, size, sess)
    end
end

return core
