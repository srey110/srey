require("lib.define");
local srey = require("srey.core")
local curtask = _curtask
local propath = _propath
local core = {}

--[[
描述:程序所在路径
参数：
返回:
    路径
--]]
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
    return srey.register(file, name, nil == maxcnt and 3 or maxcnt)
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
--[[
描述:返回netaddr_ctx保存的ip port 
参数：
    addr netaddr_ctx对象
返回:
    ip port
--]]
function core.ipport(addr)
    return srey.ipport(addr)
end
--[[
描述:创建evssl_ctx
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
描述:创建evssl_ctx
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
--[[
描述:本任务 task_ctx
参数：
返回:
    task_ctx
--]]
function core.self()
    return curtask
end
--[[
描述:listen
参数：
    ip    监听ip
    port  端口
    ssl   evssl_ctx nil 不启用ssl
    sendev sended 0 不触发 1 触发
    unptype UNPACK_TYPE
返回:
    bool
--]]
function core.listen(ip, port, ssl, sendev, unptype)
    return srey.listen(curtask, checkunptype(unptype), ssl, ip, port, nil == sendev and 0 or sendev)
end
--[[
描述:udp
参数：
    ip    ip
    port  端口
    unptype UNPACK_TYPE
返回:
    fd  INVALID_SOCK 失败
--]]
function core.udp(ip, port, unptype)
    return srey.udp(curtask, checkunptype(unptype), ip, port)
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
    srey.send(curtask, fd, data, lens, nil == ptype and PACK_TYPE.NONE or ptype)
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
