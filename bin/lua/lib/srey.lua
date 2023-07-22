local sutils = require("srey.utils")
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
local cosess_set = syn.cosess_set
local cosess_get = syn.cosess_get
local cosess_del = syn.cosess_del
local msg_clean = core.msg_clean
local task_grab = core.task_grab
local task_incref = sutils.task_incref
local task_ungrab = sutils.task_ungrab
local _xpcall = core.xpcall
local task_response = core.task_response
local coro_pool = setmetatable({}, { __mode = "kv" })

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
    task_incref(core.self())
    _xpcall(func, ...)
    task_ungrab(core.self())
    if MSG_TYPE.CLOSING == msg.mtype then
        task_ungrab(core.self())
    end
end
local function _dispatch_timeout(msg)
    local cosess = cosess_get(msg.sess)
    if not cosess then
        return
    end
    if TIMEOUT_TYPE.NORMAL == cosess.ttype then
        coro_run(_coro_cb, cosess.func, msg, tunpack(cosess.args))
    elseif TIMEOUT_TYPE.WAIT == cosess.ttype then
        if 0 ~= cosess.assoc then
            cosess_del(cosess.assoc)
        end
        coro_resume(cosess.co, msg)
    end
end
local function _dispatch_connect(msg)
    if 0 ~= msg.sess then
        local cosess = cosess_get(msg.sess)
        if cosess then
            coro_resume(cosess.co, msg)
        end
    else
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    end
end
local function _dispatch_handshaked(msg)
    if 0 ~= msg.sess then
        local cosess = cosess_get(msg.sess)
        if cosess then
            coro_resume(cosess.co, msg)
        end
    else
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    end
end
local function _dispatch_netrd(msg)
    if 0 ~= msg.sess then
        local cosess = cosess_get(msg.sess)
        if cosess then
            if SLICE_TYPE.START == msg.slice then
                cosess_set(TIMEOUT_TYPE.NONE, msg.sess, 0, cosess.co)
            elseif SLICE_TYPE.END == msg.slice then
                cosess_del(msg.sess)
            end
            coro_resume(cosess.co, msg)
        end
    else
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    end
end
local function _dispatch_close(msg)
    if 0 ~= msg.sess then
        local cosess = cosess_get(msg.sess)
        if cosess then
            if TIMEOUT_TYPE.NONE == cosess.ttype then
                cosess_del(msg.sess)
            end
            coro_resume(cosess.co, msg)
        end
    end
    local func = cb_func(msg.mtype)
    if func then
        coro_run(_coro_cb, func, msg, msg)
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
local function _dispatch_response(msg)
    local cosess = cosess_get(msg.sess)
    if cosess then
        coro_resume(cosess.co, msg)
    else
        log.ERROR("not find session %s.", tostring(msg.sess))
    end
end
function dispatch_message(msg)
    setmetatable(msg, { __gc = function (tmsg) msg_clean(tmsg) end })
    if MSG_TYPE.STARTUP == msg.mtype then
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg)
        end
    elseif MSG_TYPE.CLOSING == msg.mtype then
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg)
        else
            task_ungrab(core.self())
        end
    elseif MSG_TYPE.TIMEOUT == msg.mtype then
        _dispatch_timeout(msg)
    elseif MSG_TYPE.ACCEPT == msg.mtype then
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    elseif MSG_TYPE.CONNECT == msg.mtype then
        _dispatch_connect(msg)
    elseif MSG_TYPE.HANDSHAKED == msg.mtype then
        _dispatch_handshaked(msg)
    elseif MSG_TYPE.RECV == msg.mtype then
        _dispatch_netrd(msg)
    elseif MSG_TYPE.SEND == msg.mtype then
        local func = cb_func(msg.mtype)
        if func then
            coro_run(_coro_cb, func, msg, msg)
        end
    elseif MSG_TYPE.CLOSE == msg.mtype then
        _dispatch_close(msg)
    elseif MSG_TYPE.RECVFROM == msg.mtype then
        _dispatch_netrd(msg)
    elseif MSG_TYPE.REQUEST == msg.mtype then
        _dispatch_request(msg)
    elseif MSG_TYPE.RESPONSE == msg.mtype then
        _dispatch_response(msg)
    end
end

return core
