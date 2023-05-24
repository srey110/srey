local core = require("lib.core")
local srey = require("srey.core")
local table = table
local coroutine = coroutine
local static_funcs = {}
local cur_coro = nil
local sess_func = {}
local sess_coro = {}
local coro_pool = setmetatable({}, { __mode = "kv" })

--[[
描述:任务初始化完成后
参数：
	func  函数
返回:
--]]
function core.started(func)
	static_funcs.STARTED = func
end
--[[
描述:程序退出时
参数：
	func  函数
返回:
--]]
function core.closing(func)
	static_funcs.CLOSING = func
end
--[[
描述:accept
参数：
    func 回调函数 func(fd)
返回:
--]]
function core.accept(func)
    static_funcs.ACCEPT = func
end
--[[
描述:recv
参数：
    func 回调函数 func(fd, data, size)
返回:
--]]
function core.recv(func)
    static_funcs.RECV = func
end
--[[
描述:recvfrom
参数：
    func 回调函数 func(fd, data, size, ip, port)
返回:
--]]
function core.recvfrom(func)
    static_funcs.RECVFROM = func
end
--[[
描述:sended
参数：
    func 回调函数 func(fd, size)
返回:
--]]
function core.sended(func)
    static_funcs.SEND = func
end
--[[
描述:socket close
参数：
    func 回调函数 func(fd)
返回:
--]]
function core.closed(func)
    static_funcs.CLOSE = func
end

--[[
描述:connect
参数：
    proto UNPACK_TYPE
	ip    ip
	port  端口
	ssl   evssl_ctx nil 不启用ssl
	sendev sended 消息
返回:
	fd  nil失败
--]]
function core.connect(proto, ip, port, ssl, sendev)
	local sess = core.session()
	sess_coro[sess] = cur_coro
	if srey.connect(core.self(), proto, sess, ssl, ip, port, nil == sendev and 0 or sendev) then
		local fd, err = coroutine.yield()
		if ERR_OK ~= err then
			return nil
		else
			return fd
		end
	else
		sess_coro[sess] = nil
		return nil
	end
end

--[[
描述:定时
参数：
	ms 毫秒
	func 回调函数 func()
返回:
--]]
function core.timeout(ms, func)
	local sess = core.session()
	sess_func[sess] = func
	srey.timeout(core.self(), sess, ms)
end
--[[
描述:休眠
参数：
    ms 毫秒
返回:
--]]
function core.sleep(ms)
    local sess = core.session()
    sess_coro[sess] = cur_coro
    srey.timeout(core.self(), sess, ms)
    coroutine.yield()
end

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
function _dispatch_message(msgtype, err, fd, src, data, size, sess, addr)
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
	elseif MSG_TYPE.TIMEOUT == msgtype then
		local co
        local func = sess_func[sess]
        if nil ~= func then
            sess_func[sess] = nil
            co = co_create(func)
        else
            co = sess_coro[sess]
            sess_coro[sess] = nil
        end
        cur_coro = co
        coroutine.resume(co)
	elseif MSG_TYPE.ACCEPT == msgtype then
		local func = static_funcs.ACCEPT
		if nil ~= func then
            local co = co_create(func)
            cur_coro = co
            coroutine.resume(co, fd)
        end
	elseif MSG_TYPE.CONNECT == msgtype then
		local co = sess_coro[sess]
		sess_coro[sess] = nil
        cur_coro = co
        coroutine.resume(co, fd, err)
    elseif MSG_TYPE.RECV == msgtype then
		local func = static_funcs.RECV
		if nil ~= func then
			local co = co_create(func)
            cur_coro = co
            coroutine.resume(co, fd, data, size)
		end
	elseif MSG_TYPE.SEND == msgtype then
		local func = static_funcs.SEND
		if nil ~= func then
			local co = co_create(func)
            cur_coro = co
            coroutine.resume(co, fd, size)
		end
	elseif MSG_TYPE.CLOSE == msgtype then
		local func = static_funcs.CLOSE
		if nil ~= func then
			local co = co_create(func)
            cur_coro = co
            coroutine.resume(co, fd)
		end
	elseif MSG_TYPE.RECVFROM == msgtype then
		local func = static_funcs.RECVFROM
		if nil ~= func then
			local co = co_create(func)
            cur_coro = co
			local ip,port = core.ipport(addr)
            coroutine.resume(co, fd, data, size, ip, port)
		end
	elseif MSG_TYPE.USER == msgtype then	
    end	
end

return core