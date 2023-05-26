require("lib.define")
local srey = require("srey.core")
local curtask = _curtask
local core = {}

--[[
描述:程序所在路径
返回:
    路径 :string
--]]
function core.path()
    return _propath
end
--[[
描述:路径分隔符
返回:
    分隔符 :string
--]]
function core.pathsep()
    return _pathsep
end
--[[
描述:任务注册
参数：
    file lua文件名 :string
    name :TASK_NAME
    maxcnt 每次最多执行任务数 nil 3 :integer
返回:
    task_ctx
--]]
function core.register(file, name, maxcnt)
    return srey.register(file, name, nil == maxcnt and 3 or maxcnt)
end
--[[
描述:任务查询
参数：
    name :TASK_NAME
返回:
    task_ctx  
    nil无
--]]
function core.qury(name)
    return srey.qury(name)
end
--[[
描述:获取任务名
参数：
    task nil为本任务 :task_ctx
返回:
    TASK_NAME
--]]
function core.name(task)
    return srey.name(nil == task and curtask or task)
end
--[[
描述:获取session
参数：
    task nil为本任务 :task_ctx
返回:
    integer
--]]
function core.session(task)
    return srey.session(nil == task and curtask or task)
end
--[[
描述:返回netaddr_ctx保存的ip port 
参数：
    addr :netaddr_ctx
返回:
    ip :string, port :integer
    nil失败
--]]
function core.ipport(addr)
    return srey.ipport(addr)
end
--[[
描述:返回socket远端 ip port 
参数：
    fd socket :integer
返回:
    ip :string, port :integer
    nil失败
--]]
function core.remoteaddr(fd)
    return srey.remoteaddr(fd)
end
--[[
描述:创建evssl_ctx
参数：
    name 名称 :string
    ca ca文件 :string
    cert cert文件 :string
    key key文件 :string
    ftype SSL_FILETYPE
返回:
    evssl_ctx
--]]
function core.sslevnew(name, ca, cert, key, ftype)
    return srey.sslevnew(name, ca, cert, key, nil == ftype and SSLFILE_TYPE.PEM or ftype)
end
--[[
描述:创建evssl_ctx
参数：
    name 名称 :string
    p12 p12文件 :string
    pwd 密码 :string
返回:
    evssl_ctx
--]]
function core.sslevp12new(name, p12, pwd)
    return srey.sslevp12new(name, p12, pwd)
end
--[[
描述:evssl_ctx 查询
参数：
    name :string
返回:
    evssl_ctx
    nil无
--]]
function core.sslevqury(name)
    return srey.sslevqury(name)
end
--[[
描述:本任务 task_ctx
返回:
    task_ctx
--]]
function core.self()
    return curtask
end
--[[
描述:listen
参数：
    ip 监听ip :string
    port 端口 :integer
    ssl evssl_ctx  nil不启用ssl
    sendev 0不触发 1触发 :integer
    unptype :UNPACK_TYPE
返回:
    bool
--]]
function core.listen(ip, port, ssl, sendev, unptype)
    return srey.listen(curtask, nil == unptype and UNPACK_TYPE.NONE or unptype,
                       ssl, ip, port, nil == sendev and 0 or sendev)
end
--[[
描述:udp
参数：
    ip ip :string
    port 端口 :integer
    unptype :UNPACK_TYPE
返回:
    socket :integer
    INVALID_SOCK失败
--]]
function core.udp(ip, port, unptype)
    return srey.udp(curtask, nil == unptype and UNPACK_TYPE.NONE or unptype, ip, port)
end
--[[
描述:tcp发送
参数：
    fd socket :integer
    data lstring或userdata
    lens data长度 :integer
    ptype :PACK_TYPE
--]]
function core.send(fd, data, lens, ptype)
    srey.send(curtask, fd, data, lens, nil == ptype and PACK_TYPE.NONE or ptype)
end
--[[
描述:udp发送
参数：
    fd socket :integer
    ip ip :string
    port 端口 :integer
    data lstring或userdata
    lens data长度 :integer
--]]
function core.sendto(fd, ip, port, data, lens)
    srey.sendto(curtask, fd, ip, port, data, lens)
end
--[[
描述:关闭链接
参数：
    fd socket :integer
--]]
function core.close(fd)
    srey.close(curtask, fd)
end

return core
