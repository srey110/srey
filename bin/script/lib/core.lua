require("lib.define");
local srey = require("srey.core")
local _curtask = _curtask
local _propath = _propath

local core = {}
--程序所在路径
function core.path()
    return _propath
end
--[[
描述:任务注册
参数：
    file lua文件名
	name 任务名 数字
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
	name 任务名 数字
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
	name 任务名 数字
--]]
function core.name(task)
	return srey.name(nil == task and _curtask or task)
end
--[[
描述:获取session
参数：
	task 任务对象, nil为本任务
返回:
	session
--]]
function core.session(task)
	return srey.session(nil == task and _curtask or task)
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
    return _curtask
end
--[[
描述:任务间通信
参数：
	totask 目标任务
	sess session
	data 数据
返回:
--]]
function core.user(totask, sess, data)
	srey.user(totask, _curtask, sess, data)
end
--[[
描述:定时
参数：
	sess session
	ms 毫秒
返回:
--]]
function core.timeout(sess, ms)
	srey.timeout(_curtask, sess, ms)
end
--[[
描述:监听
参数：
	proto 协议包解析名 数字
	ip    监听ip
	port  端口
	ssl   evssl_ctx nil 不启用ssl
返回:
	bool
--]]
function core.listen(proto, ip, port, ssl)
	return srey.listen(_curtask, proto, ssl, ip, port, 0)
end
--[[
描述:链接
参数：
	proto 协议包解析名 数字
	sess  session
	ip    ip
	port  端口
	ssl   evssl_ctx nil 不启用ssl
返回:
	bool
--]]
function core.connect(proto, sess, ip, port, ssl)
	return srey.connect(_curtask, proto, sess, ssl, ip, port, 0)
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
	return srey.udp(_curtask, ip, port)
end
--[[
描述:tcp发送
参数：
	fd    sock
	data  数据
	lens  长度
	ptype 打包名称 数字
返回:
--]]
function core.send(fd, data, lens, ptype)
	srey.send(_curtask, fd, data, lens, ptype)
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
	srey.sendto(_curtask, fd, ip, port, data, lens)
end
--[[
描述:关闭链接
参数：
	fd    sock
返回:
--]]
function core.close(fd)
	srey.close(_curtask, fd)
end

return core
