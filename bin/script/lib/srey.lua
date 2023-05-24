local core = require("lib.core")

--[[
描述:程序退出时
参数：
	func  函数
返回:
--]]
local _onclosing = nil
function core.closing(func)
	_onclosing = func
end

local MSGTYPE = {
	CLOSING = 1,
	TIMEOUT = 2,
	ACCEPT = 3,
	CONNECT = 4,
	RECV = 5,
	SEND = 6,
	CLOSE = 7,
	RECVFROM = 8,
	USER = 9
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
function _dispatch_message(msgtype, err, fd, src, data, size, session, addr)
	if MSGTYPE.CLOSING == msgtype then
        if nil ~= _onclosing then
            _onclosing()
        end
    elseif MSGTYPE.RECV == msgtype then
		core.send(fd, data, size, 1)
	elseif MSGTYPE.RECVFROM == msgtype then
		local ip,port = core.ipport(addr)
		core.sendto(fd, ip, port, data, size)
    end
end

return core