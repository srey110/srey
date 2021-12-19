local core = require("srey.core")
local utile = require("lib.utile")
local json = require("cjson")
local assert = assert
local type = type
local table = table
local debug = debug
local _task = _task

local MSGTYPE = {
    START = 3,
    STOP = 4,
    TIMEOUT = 5, 
    ACCEPT = 6, 
    CONNECT = 7, 
    RECV = 8, 
    RECVFROM = 9, 
    SEND = 10,
    CLOSE = 11, 
    REQUEST = 12, 
    RESPONSE = 13
}
local LOGLV = {
    FATAL = 0, 
    ERROR = 1, 
    WARN = 2, 
    INFO = 3, 
    DEBUG = 4
}
local coro_pool = setmetatable({}, { __mode = "kv" })
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

local function lualog(loglv, msg)
    local info = debug.getinfo(3)
    if nil == info then
        return
    end
    core.log(loglv, info.source, info.currentline, utile.dump(msg))
end
local srey = {}
--日志
function srey.FATAL(msg)
    lualog(LOGLV.FATAL, msg)
end
function srey.ERROR(msg)
    lualog(LOGLV.ERROR, msg)
end
function srey.WARN(msg)
    lualog(LOGLV.WARN, msg)
end
function srey.INFO(msg)
    lualog(LOGLV.INFO, msg)
end
function srey.DEBUG(msg)
    lualog(LOGLV.DEBUG, msg)
end
--本任务
function srey.self()
    return _task
end
--任务注册 nil/task
function srey.newtask(file, name, maxcnt)
    return core.newtask(file, name, maxcnt)
end
--任务获取 nil/task
function srey.grab(idname)
    return core.grab(idname)
end
--任务释放 newtask/grab
function srey.release(task)
    core.release(nil == task and _task or task)
end
function srey.taskid(task)
    return core.taskid(nil == task and _task or task)
end
--socket
function srey.addsock(fd, socktype, family)
    return core.addsock(fd, socktype, family)
end
function srey.sockclose(sock)
    core.sockclose(sock)
end
function srey.sockid(sock)
    return core.sockid(sock)
end
function srey.socktype(sock)
    return core.socktype(sock)
end
function srey.socksend(sock, msg, ip, port)
    if nil == ip then
        return core.socksend(sock, msg)
    end
    return core.socksend(sock, msg, ip, port)
end
function srey.bufsize(sock)
    return core.bufsize(sock)
end
function srey.bufcopy(sock, lens)
    return core.bufcopy(sock, lens)
end
function srey.bufdrain(sock, lens)
    return core.bufdrain(sock, lens)
end 
function srey.bufremove(sock, lens)
    return core.bufremove(sock, lens)
end
function srey.bufsearch(sock, start, what)
    return core.bufsearch(sock, start, what)
end

local _taskid = nil
if nil ~= _task then
    _taskid = core.taskid(_task)
end
local _stop = nil
local _start = nil
local _cur_coro = nil
local rpc = {}
local sess_func = {}
local sess_coro = {}
local function session()
    while true
    do
        local sess = core.newsession(_task)
        if nil == sess_func[sess] 
            and nil == sess_coro[sess] then
            return sess
        end
    end
end
function srey.start(func)
    _start = func
end
function srey.stop(func)
    _stop = func
end
--超时func()
function srey.timeout(ms, func)
    local sess = session()
    sess_func[sess] = func
    core.timeout(_task, sess, ms)
end
function srey.sleep(ms)
    local sess = session()
    sess_coro[sess] = _cur_coro
    core.timeout(_task, sess, ms)
    coroutine.yield()
end
--网络func(sock)  nil/listener
function srey.listen(ip, port, func)
    local sess = session()
    sess_func[sess] = func
    local lsn = core.listener(_task, sess, ip, port)
    if nil == lsn then
        sess_func[sess] = nil
    end
    return lsn
end
function srey.freelsn(lsn)
    local sess = core.listenersess(lsn)
    sess_func[sess] = nil
    core.freelsn(lsn)
end
function srey.connect(ip, port, ms)
    local sess = session()
    sess_coro[sess] = _cur_coro
    local sock = core.connecter(_task, sess, ms, ip, port)
    if nil == sock then
        sess_coro[sess] = nil
        return nil
    end
    local rtn = coroutine.yield()
    if 0 ~= rtn then
        return nil
    end
    return sock
end
--func_recv(sock, size, [ip, port]) func_send(sock, size) func_close(sock)
function srey.enablerw(sock, func_recv, func_send, func_close)
    local sess = session()
    sess_func[sess] = {r = func_recv, c = func_close, s = func_send}
    local bok = core.enablerw(_task, sock, sess, nil == func_send and 0 or 1)
    if not bok then
        sess_func[sess] = nil
    end
    return bok
end
--rpc
function srey.regrpc(name, func)
    rpc[name] = func
end
function srey.call(task, name, ...)
    local info = {f = name, p = {...}}
    core.call(task, json.encode(info))
end
function srey.request(task, name, ...)
    local sess = session()
    sess_coro[sess] = _cur_coro
    local info = {f = name, p = {...}}
    core.request(task, _taskid, sess, json.encode(info))
    return coroutine.yield()
end
local function _call_rpc(srcid, sess, msg, size)
    local info = json.decode(msg, size)
    local func = rpc[info.f]
    local resp = {}
    resp.ok = false
    if nil == func then
        resp.p = {}
    else
        resp.ok = true
        resp.p = {func(table.unpack(info.p))}
    end
    if 0 ~= srcid and 0 ~= sess then
        local task = srey.grab(srcid)
        if nil ~= task then
            core.response(task, sess, json.encode(resp))
            srey.release(task)
        end
    end
end
function _dispatch_message(msgtype, srcid, sess, msg, size)
    if MSGTYPE.START == msgtype then
        if nil ~= _start then
            local co = co_create(_start)
            _cur_coro = co
            coroutine.resume(co)
        end
    elseif MSGTYPE.STOP == msgtype then
        if nil ~= _stop then
            local co = co_create(_stop)
            _cur_coro = co
            coroutine.resume(co)
        end
    elseif MSGTYPE.TIMEOUT == msgtype then
        local co
        local func = sess_func[sess]
        if nil ~= func then
            sess_func[sess] = nil
            co = co_create(func)
        else
            co = sess_coro[sess]
            sess_coro[sess] = nil
        end
        _cur_coro = co
        coroutine.resume(co)
    elseif MSGTYPE.ACCEPT == msgtype then
        local func = sess_func[sess]
        if nil ~= func then
            local co = co_create(func)
            _cur_coro = co
            coroutine.resume(co, msg)
        end
    elseif MSGTYPE.CONNECT == msgtype then
        local co = sess_coro[sess]
        sess_coro[sess] = nil
        _cur_coro = co
        coroutine.resume(co, size)
    elseif MSGTYPE.RECV == msgtype then
        local fs = sess_func[sess]
        local func = fs.r
        if nil ~= func then
            local co = co_create(func)
            _cur_coro = co
            coroutine.resume(co, msg, size)
        end
    elseif MSGTYPE.RECVFROM == msgtype then
        local fs = sess_func[sess]
        local func = fs.r
        if nil ~= func then
            local co = co_create(func)
            local ip, port, sock = core.udpmsg(msg)
            _cur_coro = co
            coroutine.resume(co, sock, size, ip, port)
        end
    elseif MSGTYPE.SEND == msgtype then
        local fs = sess_func[sess]
        local func = fs.s
        if nil ~= func then
            local co = co_create(func)
            _cur_coro = co
            coroutine.resume(co, msg, size)
        end
    elseif MSGTYPE.CLOSE == msgtype then
        local fs = sess_func[sess]
        sess_func[sess] = nil
        local func = fs.c
        if nil ~= func then
            local co = co_create(func)
            _cur_coro = co
            coroutine.resume(co, msg)
        end
    elseif MSGTYPE.REQUEST == msgtype then
        local co = co_create(_call_rpc)
        _cur_coro = co
        coroutine.resume(co, srcid, sess, msg, size)
    elseif MSGTYPE.RESPONSE == msgtype then
        local info = json.decode(msg, size)
        local co = sess_coro[sess]
        sess_coro[sess] = nil
        _cur_coro = co
        coroutine.resume(co, info.ok, table.unpack(info.p))
    end
end

return srey
