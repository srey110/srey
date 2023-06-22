require("lib.define")
local sutils = require("srey.utils")
local score = require("srey.core")
local log = require("lib.log")
local curtask = _curtask
local core = {}

function core.self()
    return curtask
end
--[[
描述:保护模式调用函数
参数:
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
参数:
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
描述:返回socket远端 ip port 
参数:
    fd socket :integer
返回:
    ip :string, port :integer
    nil失败
--]]
function core.remoteaddr(fd)
    return sutils.remoteaddr(fd)
end
--[[
描述:userdata 转 lstring
参数:
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
    return sutils.utostr(data, size)
end
--[[
描述:md5
参数:
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
    return sutils.md5(data, size)
end
--[[
描述:sha1 然后 base64编码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function core.sha1_b64encode(data, size)
    if nil == data then
        log.WARN("invalid argument.")
        return nil
    end
    return sutils.sha1_b64encode(data, size)
end
--[[
描述:base64编码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function core.b64encode(data, size)
    if nil == data then
        log.WARN("invalid argument.")
        return nil
    end
    return sutils.b64encode(data, size)
end
--[[
描述:base64解码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function core.b64decode(data, size)
    if nil == data then
        log.WARN("invalid argument.")
        return nil
    end
    return sutils.b64decode(data, size)
end
--[[
描述:url编码
参数:
    data :string
返回:
    string
--]]
function core.urlencode(data)
    return sutils.urlencode(data)
end
--[[
描述:创建evssl_ctx
参数:
    name 名称 :string
    ca ca文件 :string
    cert cert文件 :string
    key key文件 :string
    ftype SSL_FILETYPE
    verify SSLVERIFY_TYPE
返回:
    evssl_ctx
--]]
function core.evssl_new(name, ca, cert, key, ftype, verify)
    return sutils.evssl_new(name, ca, cert, key,
                            ftype or SSLFILE_TYPE.PEM,
                            verify or SSLVERIFY_TYPE.NONE)
end
--[[
描述:创建evssl_ctx
参数:
    name 名称 :string
    p12 p12文件 :string
    pwd 密码 :string
    verify SSLVERIFY_TYPE
返回:
    evssl_ctx
--]]
function core.evssl_p12new(name, p12, pwd, verify)
    return sutils.evssl_p12new(name, p12, pwd, verify or SSLVERIFY_TYPE.NONE)
end
--[[
描述:evssl_ctx 查询
参数:
    name :string
返回:
    evssl_ctx
    nil无
--]]
function core.evssl_qury(name)
    return sutils.evssl_qury(name)
end
--[[
描述:任务注册
参数:
    file lua文件名 :string
    name :TASK_NAME
返回:
    task_ctx
    nil失败
--]]
function core.task_register(file, name)
    return score.task_register(file, name, 5)
end
--[[
描述:任务查询
参数:
    name :TASK_NAME
返回:
    task_ctx  
    nil无
--]]
function core.task_qury(name)
    return sutils.task_qury(name)
end
--[[
描述:获取任务名
参数:
    task nil为本任务 :task_ctx
返回:
    TASK_NAME
--]]
function core.task_name(task)
    return sutils.task_name(task or curtask)
end
--[[
描述:获取session
返回:
    integer
--]]
function core.task_session()
    return sutils.task_session(curtask)
end
--[[
描述:任务间通信,无返回
参数:
    dst :task_ctx
    data :string or userdata
    size :integer
--]]
function core.task_call(dst, data, size)
    sutils.task_call(dst, data, size)
end
--[[
描述:任务间通信,有返回
参数:
    dst :task_ctx
    data :string or userdata
    size :integer
返回:
    data size
    nil失败
--]]
function core.task_request(dst, data, size)
    return sutils.task_request(dst, curtask, data, size)
end
--[[
描述:任务间通信,返回数据
参数:
    dst :task_ctx
    sess :integer
    data :string or userdata
    size :integer
--]]
function core.task_response(dst, sess, data, size)
    sutils.task_response(dst, sess, data, size)
end
--[[
描述:设置socket的pktype status, nil保持不变
参数:
    fd :integer
    pktype :PACK_TYPE
    status :integer
--]]
function core.ud_typstat(fd, pktype, status)
    sutils.setud_typstat(fd, pktype, status)
end
--[[
描述:设置socket消息处理的任务
参数:
    fd :integer
    task :task_ctx
--]]
function core.ud_data(fd, task)
    sutils.setud_data(fd, task)
end
--[[
描述:休眠
参数:
    ms :integer
--]]
function core.sleep(ms)
    sutils.sleep(curtask, ms)
end
--[[
描述:链接
参数:
    ip ip :string
    port 端口 :integer
    pktype :PACK_TYPE
    ssl nil不启用ssl :evssl_ctx
    sendev 是否触发发送事件 :boolean
返回:
    socket :integer 
    INVALID_SOCK失败
--]]
function core.connect(ip, port, pktype, ssl, sendev)
    return sutils.connect(curtask, pktype, ssl, ip, port, sendev and 1 or 0)
end
--[[
描述:监听
参数:
    ip 监听ip :string
    port 端口 :integer
    pktype :PACK_TYPE
    ssl evssl_ctx  nil不启用ssl
    sendev 是否触发发送事件 :boolean
返回:
    bool
--]]
function core.listen(ip, port, pktype, ssl, sendev)
    return sutils.listen(curtask, pktype or PACK_TYPE.NONE, ssl, ip, port, sendev and 1 or 0)
end
--[[
描述:udp
参数:
    ip ip :string
    port 端口 :integer
返回:
    socket :integer
    INVALID_SOCK失败
--]]
function core.udp(ip, port)
    return sutils.udp(curtask, ip, port)
end
--[[
描述:tcp发送,等待数据返回
参数:
    fd socket :integer
    data lstring或userdata
    size data长度 :integer
    pktype :PACK_TYPE
返回:
    data size
    nil失败
--]]
function core.synsend(fd, data, size, pktype)
    if INVALID_SOCK == fd or nil == data then
        log.WARN("invalid argument.")
        return nil
    end
    return sutils.synsend(curtask, fd, data, size, pktype or PACK_TYPE.NONE)
end
--[[
描述:tcp发送
参数:
    fd socket :integer
    data lstring或userdata
    lens data长度 :integer
    pktype :PACK_TYPE
--]]
function core.send(fd, data, lens, pktype)
    if INVALID_SOCK == fd or nil == data then
        log.WARN("invalid argument.")
        return
    end
    sutils.send(fd, data, lens, pktype or PACK_TYPE.NONE)
end
--[[
描述:udp发送,等待数据返回
参数:
    fd socket :integer
    ip ip :string
    port 端口 :integer
    data lstring或userdata
    lens data长度 :integer
返回:
    data size
    nil失败
--]]
function core.synsendto(fd, ip, port, data, lens)
    if INVALID_SOCK == fd or nil == data then
        log.WARN("invalid argument.")
        return nil
    end
    return sutils.synsendto(curtask, fd, ip, port, data, lens)
end
--[[
描述:udp发送
参数:
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
    sutils.sendto(fd, ip, port, data, lens)
end
--[[
描述:关闭链接
参数:
    fd socket :integer
--]]
function core.close(fd)
    if INVALID_SOCK == fd then
        return
    end
    sutils.close(fd)
end

return core
