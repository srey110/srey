local core = require("srey.core")
local utile = require("lib.utile")
local json = require("cjson")
local msgpack = require("cmsgpack")
local assert = assert
local type = type
local table = table
local debug = debug
local _task = _task
local usejson = true
local encode = usejson and json.encode or msgpack.pack
local decode = usejson and json.decode or msgpack.unpack

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
--[[
描述:任务注册
参数：
    file lua文件名
	name 任务名
	maxcnt 每次最多执行任务数
返回:
    nil 失败
	任务对象
--]]
function srey.newtask(file, name, maxcnt)
    return core.newtask(file, name, maxcnt)
end
--[[
描述:任务获取
参数：
    idname 任务id或任务名
返回:
    nil 失败
	任务对象
--]]
function srey.grab(idname)
    return core.grab(idname)
end
--[[
描述:释放newtask grab 返回的任务对象
参数：
    task 任务对象
返回:
--]]
function srey.release(task)
    core.release(nil == task and _task or task)
end
--[[
描述:任务id
参数：
    task 任务对象
返回:
    任务id
--]]
function srey.taskid(task)
    return core.taskid(nil == task and _task or task)
end
--[[
描述:获取当前时间戳  毫秒
参数：
返回:
    时间戳
--]]
function srey.ms()
    return core.millisecond()
end
--[[
描述:将已有socket句柄加到事件循环
参数：
    fd socket句柄
	socktype 字符串tcp udp
	family 字符串ipv4 ipv6
返回:
    nil 失败
	sock 对象
--]]
function srey.addsock(fd, socktype, family)
    return core.addsock(fd, socktype, family)
end
--[[
描述:关闭sock对象
参数：
    sock sock对象
返回:
--]]
function srey.sockclose(sock)
    core.sockclose(sock)
end
--[[
描述:sock id
参数：
    sock sock对象
返回:
    sock id
--]]
function srey.sockid(sock)
    return core.sockid(sock)
end
--[[
描述:sock 类型
参数：
    sock sock对象
返回:
    tcp  udp
--]]
function srey.socktype(sock)
    return core.socktype(sock)
end
--[[
描述:发送消息
参数：
    sock sock对象
	msg 消息
	ip ip (udp)
	port 端口 (udp) 
返回:
    bool
--]]
function srey.socksend(sock, msg, ip, port)
    if nil == ip then
        return core.socksend(sock, msg)
    end
    return core.socksend(sock, msg, ip, port)
end
--[[
描述:缓存中数据总长度
参数：
    sock sock对象
返回:
    长度
--]]
function srey.bufsize(sock)
    return core.bufsize(sock)
end
--[[
描述:拷贝数据
参数：
    sock sock对象
	lens 需要拷贝的长度
返回:
    nil/lstring
--]]
function srey.bufcopy(sock, lens)
    return core.bufcopy(sock, lens)
end
--[[
描述:删除数据
参数：
    sock sock对象
	lens 需要删除的长度
返回:
    实际删除的长度
--]]
function srey.bufdrain(sock, lens)
    return core.bufdrain(sock, lens)
end 
--[[
描述:删除并拷贝数据
参数：
    sock sock对象
	lens 长度
返回:
    nil/lstring
--]]
function srey.bufremove(sock, lens)
    return core.bufremove(sock, lens)
end
--[[
描述:在缓存中查找
参数：
    sock sock对象
	start 开始位置
	what 查找的对象
返回:
    -1/pos
--]]
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
local _sess = 0
local rpc = {}
local sess_func = {}
local sess_coro = {}
local function session()
    _sess = _sess + 1
    return _sess
end
--[[
描述:注册任务初始化函数
参数：
    func 初始化函数(func())
返回:
--]]
function srey.start(func)
    _start = func
end
--[[
描述:注册任务退出函数
参数：
    func 退出函数(func())
返回:
--]]
function srey.stop(func)
    _stop = func
end
--[[
描述:注册定时任务
参数：
    func 回调函数(func())
返回:
--]]
function srey.timeout(ms, func)
    assert(nil ~= func)
    local sess = session()
    sess_func[sess] = func
    core.timeout(_task, sess, ms)
end
--[[
描述:休眠
参数：
    ms 毫秒
返回:
--]]
function srey.sleep(ms)
    local sess = session()
    sess_coro[sess] = _cur_coro
    core.timeout(_task, sess, ms)
    coroutine.yield()
end
--[[
描述:监听
参数：
    ip ip
	port 端口
	func accept回调函数(func(sock))
返回:
    nil/listener
--]]
function srey.listen(ip, port, func)
    assert(nil ~= func)
    local sess = session()
    sess_func[sess] = func
    local lsn = core.listener(_task, sess, ip, port)
    if nil == lsn then
        sess_func[sess] = nil
    end
    return lsn
end
--[[
描述:释放监听
参数：
    lsn listener
返回:
--]]
function srey.freelsn(lsn)
    local sess = core.listenersess(lsn)
    sess_func[sess] = nil
    core.freelsn(lsn)
end
--[[
描述:链接
参数：
    ip ip
	port 端口
	ms 超时时间 毫秒
返回:
	nil
	sock对象
--]]
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
--[[
描述:开始读写
参数：
    sock sock对象
	func_recv 收到消息回调(func(sock, size))
	func_send 发送消息回调(func(sock, size))
	func_close 链接关闭回调(func(sock))
返回:
	bool
--]]
function srey.enablerw(sock, func_recv, func_send, func_close)
    assert(nil ~= func_recv)
    local sess = session()
    sess_func[sess] = {r = func_recv, c = func_close, s = func_send}
    local bok = core.enablerw(_task, sock, sess, nil == func_send and 0 or 1)
    if not bok then
        sess_func[sess] = nil
    end
    return bok
end
--[[
描述:rpc注册
参数：
    name 名称
	func 函数
返回:
--]]
function srey.regrpc(name, func)
    assert(nil ~= func)
    rpc[name] = func
end
--[[
描述:rpc调用，不等待返回
参数：
    task 任务对象
	name 名称
	... 调用参数
返回:
--]]
function srey.call(task, name, ...)
    local info = {f = name, p = {...}}
    core.call(task, encode(info))
end
--[[
描述:rpc调用，等待有返回
参数：
    task 任务对象
	name 名称
	... 调用参数
返回:
    bool   被调函数返回值
--]]
function srey.request(task, name, ...)
    local sess = session()
    sess_coro[sess] = _cur_coro
    local info = {f = name, p = {...}}
    core.request(task, _taskid, sess, encode(info))
    return coroutine.yield()
end
local function _call_rpc(srcid, sess, msg, size)
    local info = decode(msg, size)
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
            core.response(task, sess, encode(resp))
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
        local info = decode(msg, size)
        local co = sess_coro[sess]
        sess_coro[sess] = nil
        _cur_coro = co
        coroutine.resume(co, info.ok, table.unpack(info.p))
    end
end

return srey
