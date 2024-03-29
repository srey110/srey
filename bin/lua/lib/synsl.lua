local sutils = require("srey.utils")
local core = require("lib.core")
local log = require("lib.log")
local yield = coroutine.yield
local getid = core.getid
local ud_sess = core.ud_sess
local timeout = sutils.timeout
local cur_task = core.self()
local MSG_TYPE = MSG_TYPE
local SLICE_TYPE = SLICE_TYPE
local TIMEOUT_TYPE = TIMEOUT_TYPE
local co_sess = {}
local synsl = {}
local cur_coro = nil

function synsl.cur_coro(coro)
    cur_coro = coro
end
function synsl.cosess_set(ttype, sess, assoc, co, func, ...)
    if TIMEOUT_TYPE.NORMAL == ttype then
        assert(func, "invalid parameter.")
        co_sess[sess] = {
            ttype = ttype,
            co = co or cur_coro,
            func = func,
            args = {...}
        }
    else
        co_sess[sess] = {
            ttype = ttype,
            co = co or cur_coro,
            assoc = assoc or 0
        }
    end
end
function synsl.cosess_get(sess)
    local cosess = co_sess[sess]
    if not cosess then
        return
    end
    if TIMEOUT_TYPE.NONE ~= cosess.ttype then
        co_sess[sess] = nil
    end
    return cosess
end
function synsl.cosess_del(sess)
    co_sess[sess] = nil
end
local function wait_until(ms, sess, assoc)
    synsl.cosess_set(TIMEOUT_TYPE.WAIT, sess, assoc)
    timeout(cur_task, sess, ms)
    local msg = yield()
    assert(0 ~= msg.sess and (sess == msg.sess or assoc == msg.sess), "different session.")
    return msg
end
--[[
描述:休眠
参数:
    ms :integer
--]]
function synsl.sleep(ms)
    wait_until(ms, getid())
end
--[[
描述:超时
参数:
    ms :integer
    func :function
--]]
function synsl.timeout(ms, func, ...)
    local sess = getid()
    synsl.cosess_set(TIMEOUT_TYPE.NORMAL, sess, 0, cur_coro, func, ...)
    timeout(cur_task, sess, ms)
end
--[[
描述:任务通信，等待返回
参数:
    task :task_grab返回值
    rtype :REQUEST_TYPE
    data : string or uerdata
    size : integer
    copy : bool
返回:
    boolean data size
--]]
function synsl.task_request(task, rtype, data, size, copy)
    if not task or not data then
        return false
    end
    local sess = getid()
    core.task_request(task, rtype, sess, data, size, copy)
    local msg = wait_until(REQUEST_TIMEOUT, sess)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        log.WARN("task %d, request timeout.", core.task_name())
        return false
    end
    return ERR_OK == msg.erro, msg.data, msg.size
end
--[[
描述:链接 同步模式
参数:
    ip ip :string
    port 端口 :integer
    pktype :PACK_TYPE
    ssl nil不启用ssl :evssl_ctx
    sendev 是否触发发送事件 :boolean
返回:
    socket :integer skid :integer
    INVALID_SOCK失败
--]]
function synsl.connect(ip, port, pktype, ssl, sendev)
    local sess = getid()
    local fd, skid = core.connect(sess, ip, port, pktype, ssl, sendev)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    local msg = wait_until(CONNECT_TIMEOUT, sess)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        ud_sess(fd, skid, 0)
        core.close(fd, skid)
        log.WARN("task %d, connect host %s:%d timeout.", core.task_name(), ip, port)
        return INVALID_SOCK
    end
    if ERR_OK ~= msg.erro then
        log.WARN("task %d, connect host %s:%d error.", core.task_name(), ip, port);
        return INVALID_SOCK
    end
    return fd, skid
end
--[[
描述:链接，并发送握手包,等待握手完成 同步模式
参数:
    ip ip :string
    port 端口 :integer
    pktype :PACK_TYPE
    ssl nil不启用ssl :evssl_ctx
    hspack 握手数据包 : string or userdata
    size hspack长度 : integer
    sendev 是否触发发送事件 :boolean
返回:
    socket :integer skid :integer
    INVALID_SOCK失败
--]]
function synsl.conn_handshake(ip, port, pktype, ssl, hspack, size, sendev)
    local fd, skid = synsl.connect(ip, port, pktype, ssl, sendev)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    local sess = getid()
    ud_sess(fd, skid, sess)
    core.send(fd, skid, hspack, size, true)
    local msg = wait_until(NETRD_TIMEOUT, sess)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        ud_sess(fd, skid, 0)
        core.close(fd, skid)
        log.WARN("task %d, handshake timeout.", core.task_name())
        return INVALID_SOCK
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        log.WARN("task %d, connection timeout.", core.task_name())
        return INVALID_SOCK
    end
    if ERR_OK ~= msg.erro then
        return INVALID_SOCK
    end
    return fd, skid
end
--[[
描述:tcp发送 等待返回
参数:
    fd socket :integer
    skid :integer
    sess :integer
    data lstring或userdata
    lens data长度 :integer
    copy 是否拷贝 :boolean
返回:
    data, size
    nil失败
--]]
function synsl.send(fd, skid, sess, data, lens, copy)
    if INVALID_SOCK == fd or nil == data then
        log.WARN("invalid argument.")
        return
    end
    ud_sess(fd, skid, sess)
    core.send(fd, skid, data, lens, copy)
    local msg = wait_until(NETRD_TIMEOUT, sess)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        ud_sess(fd, skid, 0)
        core.close(fd, skid)
        log.WARN("task %d, send timeout.", core.task_name())
        return
    end
    if MSG_TYPE.CLOSE == msg.type then
        log.WARN("task %d, connction closed.", core.task_name())
        return
    end
    return msg.data, msg.size
end
--[[
描述:tcp分片消息接收
参数:
    fd socket :integer
    skid :integer
    sess :integer
返回:
    data, size，fin(boolean)
    nil失败
--]]
function synsl.slice(fd, skid, sess)
    local slice_sess = getid()
    local msg = wait_until(NETRD_TIMEOUT, slice_sess, sess)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        ud_sess(fd, skid, 0)
        core.close(fd, skid)
        log.WARN("task: %d, slice timeout.", core.task_name())
        return
    end
    synsl.cosess_del(slice_sess)
    if MSG_TYPE.CLOSE == msg.mtype then
        log.WARN("task %d, connction closed.", core.task_name())
        return
    end
    return msg.data, msg.size, SLICE_TYPE.END == msg.slice
end
--[[
描述:udp发送 等待返回
参数:
    fd socket :integer
    skid :integer
    ip ip :string
    port 端口 :integer
    data lstring或userdata
    lens data长度 :integer
返回:
    data, size
    nil失败
--]]
function synsl.sendto(fd, skid, ip, port, data, lens)
    if INVALID_SOCK == fd or nil == data then
        log.WARN("invalid argument.")
        return
    end
    local sess = getid()
    ud_sess(fd, skid, sess)
    if not core.sendto(fd, skid, ip, port, data, lens) then
        ud_sess(fd, skid, 0)
        return
    end
    local msg = wait_until(NETRD_TIMEOUT, sess)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        ud_sess(fd, skid, 0)
        log.WARN("task %d, sendto timeout.", core.task_name())
        return
    end
    return msg.udata, msg.size
end

return synsl
