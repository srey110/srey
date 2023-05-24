require("lib.define");
local srey = require("srey.core")
local curtask = _curtask
local propath = _propath

local core = {}
--程序所在路径
function core.path()
    return propath
end
--[[
描述:任务注册
参数：
    file lua文件名
	name 任务名 TASK_NAME
	maxcnt 每次最多执行任务数 nil 3
返回:
	任务对象
--]]
function core.register(file, name, maxcnt)
    return srey.register(file, name, maxcnt)
end
--[[
描述:任务查询
参数：
	name 任务名 TASK_NAME
返回:
	nil 无
	任务对象
--]]
function core.qury(name)
    return srey.qury(name)
end
--[[
描述:获取任务名
参数：
	task 任务对象, nil为本任务
返回:
	name 任务名 TASK_NAME
--]]
function core.name(task)
	return srey.name(nil == task and curtask or task)
end
--[[
描述:获取session
参数：
	task 任务对象, nil为本任务
返回:
	session
--]]
function core.session(task)
	return srey.session(nil == task and curtask or task)
end
--ip port
function core.ipport(addr)
	return srey.ipport(addr)
end
--[[
描述:evssl_ctx
参数：
	name 名称
	ca ca文件
	cert cert文件
	key key文件
	ftype  SSL_FILETYPE
返回:
	evssl_ctx
--]]
function core.sslevnew(name, ca, cert, key, ftype)
	return srey.sslevnew(name, ca, cert, key, nil == ftype and SSL_FILETYPE.PEM or ftype)
end
--[[
描述:evssl_ctx
参数：
	name 名称
	p12 p12文件
	pwd 密码
返回:
	evssl_ctx
--]]
function core.sslevp12new(name, p12, pwd)
	return srey.sslevp12new(name, p12, pwd)
end
--[[
描述:evssl_ctx 查询
参数：
	name 名称
返回:
	evssl_ctx
--]]
function core.sslevqury(name)
	return srey.sslevqury(name)
end

--以下在任务中使用
--本任务
function core.self()
    return curtask
end
--[[
描述:监听
参数：
	proto UNPACK_TYPE
	ip    监听ip
	port  端口
	ssl   evssl_ctx nil 不启用ssl
	sendev sended 消息
返回:
	bool
--]]
function core.listen(proto, ip, port, ssl, sendev)
	return srey.listen(curtask, proto, ssl, ip, port, nil == sendev and 0 or sendev)
end
--[[
描述:udp
参数：
	ip    ip
	port  端口
返回:
	bool
--]]
function core.udp(ip, port)
	return srey.udp(curtask, ip, port)
end
--[[
描述:tcp发送
参数：
	fd    sock
	data  数据
	lens  长度
	ptype PACK_TYPE
返回:
--]]
function core.send(fd, data, lens, ptype)
	srey.send(curtask, fd, data, lens, ptype)
end
--[[
描述:udp发送
参数：
	fd    sock
	ip    ip
	port  端口
	data  数据
	lens  长度
返回:
--]]
function core.sendto(fd, ip, port, data, lens)
	srey.sendto(curtask, fd, ip, port, data, lens)
end
--[[
描述:关闭链接
参数：
	fd    sock
返回:
--]]
function core.close(fd)
	srey.close(curtask, fd)
end

return core
