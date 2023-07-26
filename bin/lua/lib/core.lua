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
        log.ERROR("%s.\n%s", err, debug.traceback())
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
    if not func then
        log.ERROR("%s.\n%s", err, debug.traceback())
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
    if not data then
        log.WARN("invalid argument.")
        return nil
    end
    if 0 == size then
        return ""
    end
    return sutils.utostr(data, size)
end
function core.tohex(data, size)
    if not data then
        return nil
    end
    if 0 == size then
        return ""
    end
    return sutils.tohex(data, size)
end
--[[
描述:获取id
返回:id
    integer
--]]
function core.getid()
    return sutils.getid()
end
--[[
描述:url解析
参数:
    data :string
返回:
    table
--]]
function core.url_parse(data, size)
    if not data then
        log.WARN("invalid argument.")
        return nil
    end
    return sutils.url_parse(data, size)
end
--[[
描述:创建evssl_ctx
参数:
    name 名称 :SSL_NAME
    ca ca文件 :string
    cert cert文件 :string
    key key文件 :string
    ftype SSL_FILETYPE
    verify SSLVERIFY_TYPE
返回:
    evssl_ctx
    nil失败
--]]
function core.evssl_new(name, ca, cert, key, ftype, verify)
    return sutils.evssl_new(name, ca, cert, key,
                            ftype or SSLFILE_TYPE.PEM,
                            verify or SSLVERIFY_TYPE.NONE)
end
--[[
描述:创建evssl_ctx
参数:
    name 名称 :SSL_NAME
    p12 p12文件 :string
    pwd 密码 :string
    verify SSLVERIFY_TYPE
返回:
    evssl_ctx
    nil失败
--]]
function core.evssl_p12new(name, p12, pwd, verify)
    return sutils.evssl_p12new(name, p12, pwd, verify or SSLVERIFY_TYPE.NONE)
end
--[[
描述:evssl_ctx 查询
参数:
    name :SSL_NAME
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
    maxcnt 每次最多执行多少条消息: integer
    maxqulens 消息队列最大长度: integer
返回:
    boolean
--]]
function core.task_register(file, name, maxcnt, maxqulens)
    return score.task_register(file, name, maxcnt or EVERY_EXLENS, maxqulens or MAX_QULENS)
end
--[[
描述:任务获取
参数:
    name :TASK_NAME
返回:
    table<close> table[1]为task_ctx
    nil失败
--]]
function core.task_grab(name)
    if INVALID_TNAME == name then
        return nil
    end
    local task = sutils.task_grab(name)
    if not task then
        return nil
    end
    local rtn = setmetatable({}, {__close = function() sutils.task_ungrab(task) end})
    table.insert(rtn, task)
    return rtn
end
--[[
描述:关闭任务
参数:
    task :task_ctx
--]]
function core.task_close(task)
    sutils.task_close(task or curtask)
end
--[[
描述:获取任务名
返回:
    TASK_NAME
--]]
function core.task_name(task)
    return sutils.task_name(task or curtask)
end
--[[
描述:任务通信，无返回
参数:
    task :task_grab返回值
    rtype :REQUEST_TYPE
    data : string or uerdata
    lens : integer
    copy : bool
--]]
function core.task_call(task, rtype, data, lens, copy)
    if not task or not data then
        return
    end
    sutils.task_call(task[1], rtype, data, lens, copy and 1 or 0)
end
--[[
描述:任务通信
参数:
    task : task_grab返回值
    rtype :REQUEST_TYPE
    sess : integer
    data : string or uerdata
    size : integer
    copy : bool
--]]
function core.task_request(task, rtype, sess, data, size, copy)
    assert(task, "invalid parameter.")
    sutils.task_request(task[1], curtask, rtype, sess, data, size, copy and 1 or 0)
end
--[[
描述:返回任务通信
参数:
    task :task_grab返回值
    sess :integer
    erro : ERR_OK
    data : string or uerdata
    lens : integer
    copy : bool
--]]
function core.task_response(task, sess, erro, data, lens, copy)
    if not task then
        return
    end
    sutils.task_response(task[1], sess, erro, data, lens, copy and 1 or 0)
end
--[[
描述:设置socket的pktype
参数:
    fd :integer
    skid :integer
    pktype :PACK_TYPE
--]]
function core.ud_pktype(fd, skid, pktype)
    sutils.setud_pktype(fd, skid, pktype)
end
--[[
描述:设置socket的status
参数:
    fd :integer
    skid :integer
    status :integer
--]]
function core.ud_status(fd, skid, status)
    sutils.setud_status(fd, skid, status)
end
--[[
描述:设置socket消息处理的任务名
参数:
    fd :integer
    skid :integer
    name :TASK_NAME
--]]
function core.ud_name(fd, skid, name)
    sutils.setud_name(fd, skid, name)
end
--[[
描述:设置socket 的session
参数:
    fd :integer
    skid :integer
    sess :integer
--]]
function core.ud_sess(fd, skid, sess)
    sutils.setud_sess(fd, skid, sess)
end
--[[
描述:链接
参数:
    sess :integer
    ip ip :string
    port 端口 :integer
    pktype :PACK_TYPE
    ssl nil不启用ssl :evssl_ctx
    sendev 是否触发发送事件 :boolean
返回:
    socket :integer skid :integer
    INVALID_SOCK失败
--]]
function core.connect(sess, ip, port, pktype, ssl, sendev)
    return sutils.connect(curtask, sess, pktype, ssl, ip, port, sendev and 1 or 0)
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
    id :integer
    nil失败
--]]
function core.listen(ip, port, pktype, ssl, sendev)
    return sutils.listen(curtask, pktype or PACK_TYPE.NONE, ssl, ip, port, sendev and 1 or 0)
end
--[[
描述:取消监听
参数:
    id listen 返回的id :integer
--]]
function core.unlisten(id)
    sutils.unlisten(id)
end
--[[
描述:udp
参数:
    ip ip :string
    port 端口 :integer
返回:
    socket :integer skid :integer
    INVALID_SOCK失败
--]]
function core.udp(ip, port)
    return sutils.udp(curtask, ip, port)
end
--[[
描述:tcp发送
参数:
    fd socket :integer
    skid :integer
    data lstring或userdata
    lens data长度 :integer
    copy 是否拷贝 :boolean
--]]
function core.send(fd, skid, data, lens, copy)
    if INVALID_SOCK == fd or not data then
        return
    end
    sutils.send(curtask, fd, skid, data, lens, copy and 1 or 0)
end
--[[
描述:udp发送
参数:
    fd socket :integer
    skid :integer
    ip ip :string
    port 端口 :integer
    data lstring或userdata
    lens data长度 :integer
返回:
    bool
--]]
function core.sendto(fd, skid, ip, port, data, lens)
    if INVALID_SOCK == fd or not data then
        return
    end
    return sutils.sendto(fd, skid, ip, port, data, lens)
end
--[[
描述:关闭链接
参数:
    fd socket :integer
    skid :integer
--]]
function core.close(fd, skid)
    if INVALID_SOCK == fd then
        return
    end
    sutils.close(fd, skid)
end
--[[
描述:消息释放
参数:
    msg :table
--]]
function core.msg_clean(msg)
    if not msg.data then
        return
    end
    sutils.msg_clean(curtask, msg.mtype, msg.pktype, msg.data)
end

return core
