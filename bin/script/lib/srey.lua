require("lib.define")
require("lib.utils")
require("lib.log")
local task = require("srey.task")
local utils = require("srey.utils")
local core = require("srey.core")
local http = require("srey.http")
local harbor = require("srey.harbor")
local xpcall = xpcall
local tunpack = table.unpack
local tremove = table.remove
local coroutine_create = coroutine.create
local coroutine_yield = coroutine.yield
local coroutine_resume = coroutine.resume
local cur_task = _curtask
local coro_running = nil
local coro_sess = {}
local slice_sess = {}
local func_cbs = {}
local srey = {}
local coro_pool = setmetatable({}, { __mode = "kv" })
local MSG_TYPE = {
    STARTUP =    0x01,
    CLOSING =    0x02,
    TIMEOUT =    0x03,
    ACCEPT =     0x04,
    CONNECT =    0x05,
    HANDSHAKED = 0x06,
    RECV =       0x07,
    SEND =       0x08,
    CLOSE =      0x09,
    RECVFROM =   0x0a,
    REQUEST =    0x0b,
    RESPONSE =   0x0c
}

function srey.xpcall(func, ...)
    local function _error(err)
        ERROR("%s\n%s", err, debug.traceback())
    end
    return xpcall(func, _error, ...)
end
function srey.dostring(str)
    local func, err = load(str)
    if not func then
        ERROR("%s.\n%s", err, debug.traceback())
        return false
    end
    return srey.xpcall(func)
end

local function _coro_create(func)
    local coro = tremove(coro_pool)
    if not coro then
        coro = coroutine_create(
            function(...)
                func(...)
                while true do
                    func = nil
                    coro_pool[#coro_pool + 1] = coro
                    func = coroutine_yield()
                    func(coroutine_yield())
                end
            end)
    else
        coroutine_resume(coro, func)
    end
    return coro
end
local function _coro_resume(coro, ...)
    coro_running = coro
    coroutine_resume(coro_running, ...)
end
local function _coro_run(func, ...)
    _coro_resume(_coro_create(func), ...)
end
local function _coro_cb(func, ...)
    srey.task_incref(cur_task)
    srey.xpcall(func, ...)
    srey.task_ungrab(cur_task)
end

function srey.id()
    return utils.id()
end
function srey.ud_str(data, size)
    return utils.ud_str(data, size)
end
function srey.hex(data, size)
    return utils.hex(data, size)
end
function srey.remote_addr(fd)
    return utils.remote_addr(fd)
end

--task_ctx
function srey.task_register(file, name)
    return task.register(file, name)
end
function srey.task_close(taskctx)
    task.close(taskctx)
end
--task_ctx
function srey.task_grab(name)
    return task.grab(name)
end
function srey.task_incref(taskctx)
    task.incref(taskctx)
end
function srey.task_ungrab(taskctx)
    task.ungrab(taskctx)
end
function srey.task_name(taskctx)
    return task.name(taskctx)
end

--func()
function srey.startup(func)
    func_cbs[MSG_TYPE.STARTUP] = func
end
local function _startup_dispatch()
    _coro_run(_coro_cb, func_cbs[MSG_TYPE.STARTUP])
end

--func()
function srey.closing(func)
    func_cbs[MSG_TYPE.CLOSING] = func
end
local function _closing()
    local func = func_cbs[MSG_TYPE.CLOSING]
    if func then
        srey.xpcall(func)
    end
    srey.task_ungrab(cur_task)
end
local function _closing_dispatch()
    _coro_run(_coro_cb, _closing)
end

local function _set_coro_sess(sess, assoc, coro, keep, func, ...)
    coro_sess[sess] = {
        assoc = assoc,
        coro = coro,
        keep = keep,
        func = func,
        args = {...}
    }
end
local function _get_coro_sess(sess)
    local cosess = coro_sess[sess]
    if not cosess then
        return nil
    end
    if not cosess.keep then
        coro_sess[sess] = nil
    end
    return cosess
end
local function _coro_wait(sess, assoc, ms)
    _set_coro_sess(sess, assoc, coro_running)
    if ms and ms >= 0 then
        core.timeout(sess, ms)
    end
    local msg = coroutine_yield()
    assert((0 ~= sess) and (sess == msg.sess or assoc == msg.sess), "different session.")
    return msg
end

function srey.sleep(ms)
    _coro_wait(srey.id(), 0, ms)
end
--func(...)
function srey.timeout(ms, func, ...)
    local sess = srey.id()
    _set_coro_sess(sess, 0, nil, false, func, ...)
    core.timeout(sess, ms)
end
local function _timeout_dispatch(msg)
    local cosess = _get_coro_sess(msg.sess)
    if not cosess then
        return
    end
    if cosess.func then
        _coro_run(_coro_cb, cosess.func, tunpack(cosess.args))
    else
        if 0 ~= cosess.assoc then
            coro_sess[cosess.assoc] = nil
        end
        _coro_resume(cosess.coro, msg)
    end
end

--func(reqtype, sess, src, data, size)
function srey.on_requested(func)
    func_cbs[MSG_TYPE.REQUEST] = func
end
function srey.call(dst, reqtype, data, size, copy)
    if INVALID_TNAME == dst then
        return
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        return
    end
    core.call(dtask, reqtype, data, size, copy)
    srey.task_ungrab(dtask)
end
--data size
function srey.request(dst, ms, reqtype, data, size, copy)
    if INVALID_TNAME == dst then
        return nil
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        return nil
    end
    local sess = srey.id()
    core.request(dtask, reqtype, sess, data, size, copy)
    srey.task_ungrab(dtask)
    local msg = _coro_wait(sess, 0, ms)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        WARN("task %d session %s request timeout.", srey.task_name(), tostring(sess))
        return nil
    end
    if ERR_OK ~= msg.erro then
        return nil
    end
    return msg.data, msg.size
end
local function _request_dispatch(msg)
    local func = func_cbs[MSG_TYPE.REQUEST]
    if not func then
        srey.response(msg.src, msg.sess, ERR_FAILED, "not register request function.")
        return
    end
    _coro_run(_coro_cb, func, msg.pktype, msg.sess, msg.src, msg.data, msg.size)
end

function srey.response(dst, sess, erro, data, size, copy)
    if INVALID_TNAME == dst or 0 == sess then
        return
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        return
    end
    core.response(dtask, sess, erro, data, size, copy)
    srey.task_ungrab(dtask)
end
local function _response_dispatch(msg)
    local cosess = _get_coro_sess(msg.sess)
    if not cosess then
        WARN("_response_dispatch not find session %s.", tostring(msg.sess))
        return
    end
    _coro_resume(cosess.coro, msg)
end

function srey.sock_session(fd, skid, sess)
    core.session(fd, skid, sess)
end
function srey.sock_pack_type(fd, skid, pktype)
    core.pack_type(fd, skid, pktype)
end
function srey.sock_status(fd, skid, status)
    core.status(fd, skid, status)
end
function srey.sock_bind_task(fd, skid, tname)
    core.bind_task(fd, skid, tname)
end

--func(pktype, fd, skid)
function srey.on_accepted(func)
    func_cbs[MSG_TYPE.ACCEPT] = func
end
--id  -1 error
function srey.listen(pktype, sslname, ip, port, appendev)
    local ssl
    if INVALID_TNAME ~= sslname then
        ssl = core.ssl_qury(sslname)
        if not ssl then
            ERROR("ssl_qury not find ssl name: %s",  sslname)
        end
    end
    return core.listen(pktype, ssl, ip, port, appendev)
end
function srey.unlisten(lsnid)
    core.unlisten(lsnid)
end
local function _net_accept_dispatch(msg)
    local func = func_cbs[MSG_TYPE.ACCEPT]
    if func then
        _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid)
    end
end

--func(pktype, fd, skid, sess, erro)
function srey.on_connected(func)
    func_cbs[MSG_TYPE.CONNECT] = func
end
--fd skid
function srey.connect(sess, pktype, sslname, ip, port, appendev)
    local ssl
    if INVALID_TNAME ~= sslname then
        ssl = core.ssl_qury(sslname)
        if not ssl then
            ERROR("ssl_qury not find ssl name: %s",  sslname)
        end
    end
    return core.connect(sess, pktype, ssl, ip, port, appendev)
end
--fd skid
function srey.syn_connect(pktype, sslname, ip, port, ms, appendev)
    local sess = srey.id()
    local fd, skid = srey.connect(sess, pktype, sslname, ip, port, appendev)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    local msg = _coro_wait(sess, 0, ms)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.sock_session(fd, skid, 0)
        srey.close(fd, skid)
        WARN("task %d session %s connect host %s:%d timeout.", srey.task_name(), tostring(sess), ip, port)
        return INVALID_SOCK
    end
    if ERR_OK ~= msg.erro then
        WARN("task %d session %s connect host %s:%d error.", srey.task_name(), tostring(sess), ip, port)
        return INVALID_SOCK
    end
    return fd, skid
end
local function _net_connect_dispatch(msg)
    if 0 ~= msg.sess then
        local cosess = _get_coro_sess(msg.sess)
        if cosess then
            _coro_resume(cosess.coro, msg)
        else
            WARN("_net_connect_dispatch not find session %s error %d.", tostring(msg.sess), msg.erro)
        end
    else
        local func = func_cbs[MSG_TYPE.CONNECT]
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.sess, msg.erro)
        end
    end
end

--func(pktype, fd, skid, client, sess, erro)
function srey.on_handshaked(func)
    func_cbs[MSG_TYPE.HANDSHAKED] = func
end
--bool
function srey.wait_handshaked(sess, ms)
    local msg = _coro_wait(sess, 0, ms)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        return false
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        return false
    end
    if ERR_OK ~= msg.erro then
        return false
    end
    return true
end
local function _net_handshaked_dispatch(msg)
    if 0 ~= msg.sess then
        local cosess = _get_coro_sess(msg.sess)
        if cosess then
            _coro_resume(cosess.coro, msg)
        end
    else
        local func = func_cbs[MSG_TYPE.HANDSHAKED]
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.sess, msg.erro)
        end
    end
end

--func(pktype, fd, skid, client, sess, slice, data, size)
function srey.on_recved(func)
    func_cbs[MSG_TYPE.RECV] = func
end
function srey.send(fd, skid, data, size, copy)
    core.send(fd, skid, data, size, copy)
end
--data size
function srey.syn_send(fd, skid, sess, data, size, ms, copy)
    srey.sock_session(fd, skid, sess)
    srey.send(fd, skid, data, size, copy)
    local msg = _coro_wait(sess, 0, ms)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.sock_session(fd, skid, 0)
        srey.close(fd, skid)
        WARN("task %d session %s send timeout.", srey.task_name(), tostring(sess))
        return nil
    end
    if MSG_TYPE.CLOSE == msg.type then
        WARN("task %d session %s connction closed.", srey.task_name(), tostring(sess))
        return nil
    end
    return msg.data, msg.size
end
--bool data size
function srey.syn_slice(fd, skid, assoc, ms)
    local sess = srey.id()
    slice_sess[skid] = sess
    local msg = _coro_wait(sess, assoc, ms)
    slice_sess[skid] = nil
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.sock_session(fd, skid, 0)
        srey.close(fd, skid)
        WARN("task: %d, slice timeout.", srey.task_name())
        return false
    end
    coro_sess[sess] = nil
    if MSG_TYPE.CLOSE == msg.mtype then
        WARN("task %d, connction closed.", srey.task_name())
        return false
    end
    return SLICE_TYPE.END == msg.slice, msg.data, msg.size
end
--bool  fd pktype HTTP
function srey.net_call(fd, skid, dst, reqtype, key, data, size, ms)
    local reqdata, reqsize = harbor.pack(dst, 1, reqtype, key, data, size)
    local respdata, _ = srey.syn_send(fd, skid, srey.id(), reqdata, reqsize, ms, 0)
    if not respdata then
        return false
    end
    local status = http.status(respdata)
    if not status then
        return false
    end
    return "200" == status[2]
end
--data size
function srey.net_request(fd, skid, dst, reqtype, key, data, size, ms)
    local reqdata, reqsize = harbor.pack(dst, 0, reqtype, key, data, size)
    local respdata, _ = srey.syn_send(fd, skid, srey.id(), reqdata, reqsize, ms, 0)
    if not respdata then
        return nil
    end
    local status = http.status(respdata)
    if not status then
        return nil
    end
    if "200" ~= status[2] then
        return nil
    end
    return http.data(respdata)
end
local function _net_recv_dispatch(msg)
    if 0 ~= msg.sess then
        local cosess = _get_coro_sess(msg.sess)
        if cosess then
            if SLICE_TYPE.START == msg.slice then
                _set_coro_sess(msg.sess, 0, coro_running, true)
            elseif SLICE_TYPE.END == msg.slice then
                coro_sess[msg.sess] = nil
            end
            if SLICE_TYPE.END == msg.slice or SLICE_TYPE.SLICE == msg.slice  then
                if slice_sess[msg.skid] then
                    _coro_resume(cosess.coro, msg)
                else
                    ERROR("%s", "slice error.")
                end
            else
                _coro_resume(cosess.coro, msg)
            end
        end
    else
        local func = func_cbs[MSG_TYPE.RECV]
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.sess, msg.slice, msg.data, msg.size)
        end
    end
end

--func(pktype, fd, skid, client, sess, size)
function srey.on_sended(func)
    func_cbs[MSG_TYPE.SEND] = func
end
local function _net_sended_dispatch(msg)
    local func = func_cbs[MSG_TYPE.SEND]
    if func then
        _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.sess, msg.size)
    end
end

--func(pktype, fd, skid, sess)
function srey.on_closed(func)
    func_cbs[MSG_TYPE.CLOSE] = func
end
function srey.close(fd, skid)
    core.close(fd, skid)
end
local function _net_close_dispatch(msg)
    if 0 ~= msg.sess then
        local cosess = _get_coro_sess(msg.sess)
        if cosess then
            if cosess.keep then
                coro_sess[msg.sess] = nil
            end
            _coro_resume(cosess.coro, msg)
        end
    end
    local func = func_cbs[MSG_TYPE.CLOSE]
    if func then
        _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.sess)
    end
end

--func(fd, skid, sess, ip, port, data, size)
function srey.on_recvedfrom(func)
    func_cbs[MSG_TYPE.RECVFROM] = func
end
--fd skid
function srey.udp(ip, port)
    if not ip then
        ip = "0.0.0.0"
        port = 0
    end
    return core.udp(ip, port)
end
function srey.sendto(fd, skid, ip, port, data, size)
    return core.sendto(fd, skid, ip, port, data, size)
end
--data size
function srey.syn_sendto(fd, skid, ip, port, data, size, ms)
    local sess = srey.id()
    srey.sock_session(fd, skid, sess)
    if not srey.sendto(fd, skid, ip, port, data, size) then
        srey.sock_session(fd, skid, 0)
        return nil
    end
    local msg = _coro_wait(sess, 0, ms)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.sock_session(fd, skid, 0)
        WARN("task %d, sendto timeout.", srey.task_name())
        return nil
    end
    return msg.udata, msg.size
end
local function _net_recvfrom_dispatch(msg)
    if 0 ~= msg.sess then
        local cosess = _get_coro_sess(msg.sess)
        if cosess then
            _coro_resume(cosess.coro, msg)
        end
    else
        local func = func_cbs[MSG_TYPE.RECVFROM]
        if func then
            _coro_run(_coro_cb, func, msg.fd, msg.skid, msg.sess, msg.ip, msg.port, msg.udata, msg.size)
        end
    end
end

function message_dispatch(msg)
    if MSG_TYPE.STARTUP == msg.mtype then
        _startup_dispatch()
    elseif MSG_TYPE.CLOSING == msg.mtype then
        _closing_dispatch()
    elseif MSG_TYPE.TIMEOUT == msg.mtype then
        _timeout_dispatch(msg)
    elseif MSG_TYPE.ACCEPT == msg.mtype then
        _net_accept_dispatch(msg)
    elseif MSG_TYPE.CONNECT == msg.mtype then
        _net_connect_dispatch(msg)
    elseif MSG_TYPE.HANDSHAKED == msg.mtype then
        _net_handshaked_dispatch(msg)
    elseif MSG_TYPE.RECV == msg.mtype then
        _net_recv_dispatch(msg)
    elseif MSG_TYPE.SEND == msg.mtype then
        _net_sended_dispatch(msg)
    elseif MSG_TYPE.CLOSE == msg.mtype then
        _net_close_dispatch(msg)
    elseif MSG_TYPE.RECVFROM == msg.mtype then
        _net_recvfrom_dispatch(msg)
    elseif MSG_TYPE.REQUEST == msg.mtype then
        _request_dispatch(msg)
    elseif MSG_TYPE.RESPONSE == msg.mtype then
        _response_dispatch(msg)
    end
end

return srey
