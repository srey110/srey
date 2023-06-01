require("lib.define")
local srey = require("srey.core")
local log = require("lib.log")
local curtask = _curtask
local propath = _propath
local pathsep = _pathsep
local core = {}

--[[
描述:保护模式调用函数
参数：
    func function(...) :function
返回:
    bool, 被调函数返回值
--]]
function core.xpcall(func, ...)
    local function error(err)
        log.WARN("%s.\n%s", err, debug.traceback())
    end
    return xpcall(func, error, ...)
end
--[[
描述:执行字符串
参数：
    msg :string
返回:
    bool, 被调函数返回值
--]]
function core.dostring(msg)
    local func, err = load(msg)
    if nil == func then
        log.WARN("%s.\n%s", err, debug.traceback())
        return false
    end
    return core.xpcall(func)
end
--[[
描述:程序所在路径
返回:
    路径 :string
--]]
function core.path()
    return propath
end
--[[
描述:路径分隔符
返回:
    分隔符 :string
--]]
function core.pathsep()
    return pathsep
end
--[[
描述:任务注册
参数：
    file lua文件名 :string
    name :TASK_NAME
返回:
    task_ctx
--]]
function core.register(file, name)
    return srey.register(file, name, 5)
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
    return srey.name(task or curtask)
end
--[[
描述:获取session
参数：
    task nil为本任务 :task_ctx
返回:
    integer
--]]
function core.session(task)
    return srey.session(task or curtask)
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
    verify SSLVERIFY_TYPE
返回:
    evssl_ctx
--]]
function core.sslevnew(name, ca, cert, key, ftype, verify)
    return srey.sslevnew(name, ca, cert, key,
                         ftype or SSLFILE_TYPE.PEM,
                         verify or SSLVERIFY_TYPE.NONE)
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
function core.sslevp12new(name, p12, pwd, verify)
    return srey.sslevp12new(name, p12, pwd, verify or SSLVERIFY_TYPE.NONE)
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
    unptype :UNPACK_TYPE
    ssl evssl_ctx  nil不启用ssl
    sendev 是否触发发送事件 :boolean
返回:
    bool
--]]
function core.listen(ip, port, unptype, ssl, sendev)
    return srey.listen(curtask, unptype or UNPACK_TYPE.NONE, ssl, ip, port, sendev and 1 or 0)
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
    return srey.udp(curtask, unptype or UNPACK_TYPE.NONE, ip, port)
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
    if INVALID_SOCK == fd or nil == data then
        log.WARN("invalid argument.")
        return
    end
    srey.send(curtask, fd, data, lens, ptype or PACK_TYPE.NONE)
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
    if INVALID_SOCK == fd or nil == data then
        log.WARN("invalid argument.")
        return
    end
    srey.sendto(curtask, fd, ip, port, data, lens)
end
--[[
描述:关闭链接
参数：
    fd socket :integer
--]]
function core.close(fd)
    if INVALID_SOCK == fd then
        return
    end
    srey.close(curtask, fd)
end
--[[
描述:md5
参数：
    data string or userdata
    size :integer
返回:
    string
--]]
function core.md5(data, size)
    if nil == data then
        log.WARN("invalid argument.")
        return nil
    end
    return srey.md5(data, size)
end
--[[
描述:userdata 转 lstring
参数：
    data :userdata
    size :integer
返回:
    string
    nil失败
--]]
function core.utostr(data, size)
    if nil == data then
        log.WARN("invalid argument.")
        return nil
    end
    if 0 == size then
        return ""
    end
    return srey.utostr(data, size)
end

return core
