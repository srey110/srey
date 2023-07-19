local core = require("lib.core")
local syn = require("lib.synsl")
local cbs = require("lib.cbs")
local log = require("lib.log")
local MSG_TYPE = MSG_TYPE
local SLICE_TYPE = SLICE_TYPE
local TIMEOUT_TYPE = TIMEOUT_TYPE
local tunpack = table.unpack
local tremove = table.remove
local cocreate = coroutine.create
local yield = coroutine.yield
local resume = coroutine.resume
local cb_func = cbs.cb
local cur_coro = syn.cur_coro
local sess_get = syn.sess_get
local timeout_get =syn.timeout_get
local timeout_del = syn.timeout_del
local msg_clean = core.msg_clean
local task_refadd = core.task_refadd
local task_release = core.task_release
local task_grab = core.task_grab
local _xpcall = core.xpcall
local task_response = core.task_response
local coro_pool = setmetatable({}, { __mode = "kv" })

function core.coro_pool_size()
    return #coro_pool
end
local function _coro_create(func)
    local co = tremove(coro_pool)
    if not co then
        co = cocreate(
            function(...)
                func(...)
                while true do
                    func = nil
                    coro_pool[#coro_pool + 1] = co
                    func = yield()
                    func(yield())
                end
            end)
    else
        resume(co, func)
    end
    return co
end
local function coro_resume(coro, ...)
    cur_coro(coro)
    resume(coro, ...)
end
local function coro_run(func, ...)
    coro_resume(_coro_create(func), ...)
end
local function _coro_cb(func, msg, ...)
    task_refadd()
    _xpcall(func, ...)
    task_release()
    if MSG_TYPE.CLOSING == msg.mtype then
        task_release()
    end
end
local function resume_sess(msg, delsess, deltmo)
    local coro = sess_get(msg.sess, delsess)
    if not coro then
        return false
    end
    if deltmo then
        timeout_del(msg.sess)
    end
    coro_resume(coro, msg)
    return true
end
local function _dispatch_timeout(msg)
    local cotmo = timeout_get(msg.sess)
    if not cotmo then
        return
    end
    if TIMEOUT_TYPE.SLEEP == cotmo.ttype then
        coro_resume(cotmo.co, msg)
    elseif TIMEOUT_TYPE.NORMAL == cotmo.ttype then
        coro_run(_coro_cb, cotmo.func, msg, tunpack(cotmo.args))
    elseif TIMEOUT_TYPE.SESSION == cotmo.ttype then
        resume_sess(msg, true, false)
    end
end
local function _dispatch_netrd(msg)
    if 0 ~= msg.sess then
        local delsess
        if SLICE_TYPE.SLICE == msg.slice or
           SLICE_TYPE.START == msg.slice then
            delsess = false
        else
            delsess = true
        end
        local deltmo
        if SLICE_TYPE.NONE == msg.slice or
           SLICE_TYPE.START == msg.slice then
            deltmo = true
        else
            deltmo = false
        end
        resume_sess(msg, delsess, deltmo)
    else
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    end
end
local function _dispatch_request(msg)
    local func = cb_func(msg.mtype)
    if not func then
        if INVALID_TNAME ~= msg.src and 0 ~= msg.sess then
            local src<close> = task_grab(msg.src)
            task_response(src, msg.sess, ERR_FAILED)
        end
        return
    end
    coro_run(_coro_cb, func, msg, msg)
end
function dispatch_message(msg)
    setmetatable(msg, { __gc = function (tmsg) msg_clean(tmsg) end })
    if MSG_TYPE.STARTUP == msg.mtype then
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg)
        end
    elseif MSG_TYPE.CLOSING == msg.mtype then
        coro_run(_coro_cb, cb_func(msg.mtype), msg)
    elseif MSG_TYPE.TIMEOUT == msg.mtype then
        _dispatch_timeout(msg)
    elseif MSG_TYPE.ACCEPT == msg.mtype then
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    elseif MSG_TYPE.CONNECT == msg.mtype then
        if 0 ~= msg.sess then
            resume_sess(msg, true, true)
        else
            local func = cb_func(msg.mtype)
            if func then
                coro_run(_coro_cb, func, msg, msg)
            end
        end
    elseif MSG_TYPE.HANDSHAKED == msg.mtype then
        if 0 ~= msg.sess then
            resume_sess(msg, true, true)
        else
            local func = cb_func(msg.mtype)
            if func then
                coro_run(_coro_cb, func, msg, msg)
            end
        end
    elseif MSG_TYPE.RECV == msg.mtype then
        _dispatch_netrd(msg)
    elseif MSG_TYPE.SEND == msg.mtype then
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    elseif MSG_TYPE.CLOSE == msg.mtype then
        if 0 ~= msg.sess then
            resume_sess(msg, true, true)
        end
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    elseif MSG_TYPE.RECVFROM == msg.mtype then
        _dispatch_netrd(msg)
    elseif MSG_TYPE.REQUEST == msg.mtype then
        _dispatch_request(msg)
    elseif MSG_TYPE.RESPONSE == msg.mtype then
        if not resume_sess(msg, true, true) then
            log.ERROR("not find session %s.", tostring(msg.sess))
        end
    end
end

return core
