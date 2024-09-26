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
local TASK_NAME = TASK_NAME
local SSL_NAME = SSL_NAME
local coro_running = nil
local coro_sess = {}
local func_cbs = {}
local srey = {}
local nyield = 0
local coro_pool = setmetatable({}, { __mode = "kv" })
local MSG_TYPE = {
    STARTUP    = 0x01,
    CLOSING    = 0x02,
    TIMEOUT    = 0x03,
    ACCEPT     = 0x04,
    CONNECT    = 0x05,
    SSLEXCHANGED = 0x06,
    HANDSHAKED = 0x07,
    RECV       = 0x08,
    SEND       = 0x09,
    CLOSE      = 0x0a,
    RECVFROM   = 0x0b,
    REQUEST    = 0x0c,
    RESPONSE   = 0x0d
}
local TIMEOUT = {
    REQUEST = 1500,
    CONNECT = 3000,
    NETREAD = 3000,
}
local SLICE_TYPE = {
    START = 0x01,
    SLICE = 0x02,
    END   = 0x04,
}

function srey.xpcall(func, ...)
    local function _error(err)
        ERROR("%s\n%s.", err, debug.traceback())
    end
    return xpcall(func, _error, ...)
end
function srey.dostring(str)
    local func, err = load(str)
    if not func then
        ERROR("%s.\n%s.", err, debug.traceback())
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
function srey.timer_ms()
    return task.timer_ms()
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
    if nyield > 0 then
        WARN("yield %d.", nyield)
    end
end

local function _set_coro_sess(coro, sess, mtype, ms, func, ...)
    assert(not coro_sess[sess], "repeat session")
    local timeout = 0
    if ms > 0 then
        timeout = srey.timer_ms() + ms
    end
    coro_sess[sess] = {
        timeout = timeout,
        coro = coro,
        mtype = mtype,
        func = func,
        args = {...}
    }
end
local function _get_coro_sess(sess, mtype)
    local corosess = coro_sess[sess]
    if not corosess then
        return nil
    end
    if mtype ~= corosess.mtype and MSG_TYPE.CLOSE ~= mtype then
        return nil
    end
    coro_sess[sess] = nil
    return corosess
end
local function _coro_wait(sess, mtype, ms)
    _set_coro_sess(coro_running, sess, mtype, ms)
    nyield = nyield + 1
    local msg = coroutine_yield()
    nyield = nyield - 1
    assert(sess == msg.sess, "different session.")
    return msg
end

function srey.sleep(ms)
    local sess = srey.id()
    core.timeout(sess, ms)
    _coro_wait(sess, MSG_TYPE.TIMEOUT, 0)
end
--func(...)
function srey.timeout(ms, func, ...)
    local sess = srey.id()
    _set_coro_sess(nil, sess, MSG_TYPE.TIMEOUT, 0, func, ...)
    core.timeout(sess, ms)
end
local function _timeout_dispatch(msg)
    local corosess = _get_coro_sess(msg.sess, MSG_TYPE.TIMEOUT)
    if not corosess then
        WARN("can't find session %s.", tostring(msg.sess))
        return
    end
    if corosess.func then
        _coro_run(_coro_cb, corosess.func, tunpack(corosess.args))
    else
        _coro_resume(corosess.coro, msg)
    end
end

--func(reqtype, sess, src, data, size)
function srey.on_requested(func)
    func_cbs[MSG_TYPE.REQUEST] = func
end
function srey.call(dst, reqtype, data, size, copy)
    if TASK_NAME.NONE == dst then
        WARN("parameter error.")
        return
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        WARN("grab task error.")
        return
    end
    core.call(dtask, reqtype, data, size, copy)
    srey.task_ungrab(dtask)
end
--data size
function srey.request(dst, reqtype, data, size, copy)
    if TASK_NAME.NONE == dst then
        WARN("parameter error.")
        return nil
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        WARN("grab task error.")
        return nil
    end
    local sess = srey.id()
    core.request(dtask, reqtype, sess, data, size, copy)
    srey.task_ungrab(dtask)
    local msg = _coro_wait(sess, MSG_TYPE.RESPONSE, TIMEOUT.REQUEST)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        WARN("request timeout, session %s.", tostring(sess))
        return nil
    end
    if ERR_OK ~= msg.erro then
        if msg.data then
            WARN("request error, session:%s code:%d message:%s.",
             tostring(sess), msg.erro, srey.ud_str(msg.data, msg.size))
        end
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
    if TASK_NAME.NONE == dst or 0 == sess then
        WARN("parameter error.")
        return
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        WARN("grab task error.")
        return
    end
    core.response(dtask, sess, erro, data, size, copy)
    srey.task_ungrab(dtask)
end
local function _response_dispatch(msg)
    local corosess = _get_coro_sess(msg.sess, MSG_TYPE.RESPONSE)
    if not corosess then
        WARN("can't find session %s.", tostring(msg.sess))
        return
    end
    _coro_resume(corosess.coro, msg)
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
function srey.listen(pktype, sslname, ip, port, netev)
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
        if not ssl then
            WARN("ssl_qury not find ssl name %d.", sslname)
            return ERR_FAILED
        end
    end
    return core.listen(pktype, ssl, ip, port, netev)
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

--func(pktype, fd, skid, err)
function srey.on_connected(func)
    func_cbs[MSG_TYPE.CONNECT] = func
end
--fd skid
function srey.connect(pktype, sslname, ip, port, netev)
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
        if not ssl then
            WARN("ssl_qury not find ssl name %d.", sslname)
            return INVALID_SOCK
        end
    end
    local fd, skid = core.connect(pktype, ssl, ip, port, netev)
    if INVALID_SOCK == fd then
        WARN("connect %s:%d error.", ip, port)
        return INVALID_SOCK
    end
    local msg = _coro_wait(skid, MSG_TYPE.CONNECT, TIMEOUT.CONNECT)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid)
        WARN("connect %s:%d timeout, skid %s.", ip, port, tostring(skid))
        return INVALID_SOCK
    end
    if ERR_OK ~= msg.erro then
        WARN("connect %s:%d error, skid %s.", ip, port, tostring(skid))
        return INVALID_SOCK
    end
    if nil ~= ssl then
        if not srey.wait_ssl_exchanged(fd, skid) then
            return INVALID_SOCK
        end
    end
    return fd, skid
end
local function _net_connect_dispatch(msg)
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.CONNECT)
    if not corosess then
        local func = func_cbs[MSG_TYPE.CONNECT]
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.erro)
        end
    else
        _coro_resume(corosess.coro, msg)
    end
end

--func(pktype, fd, skid, client)
function srey.on_ssl_exchanged(func)
    func_cbs[MSG_TYPE.SSLEXCHANGED] = func
end
function srey.ssl_exchange(fd, skid, client, sslname)
    if SSL_NAME.NONE == sslname then
        return false
    end
    local ssl = core.ssl_qury(sslname)
    if not ssl then
        WARN("ssl_qury not find ssl name %d.", sslname)
        return false
    end
    core.ssl_exchange(fd, skid, client, ssl)
    return true
end
function srey.syn_ssl_exchange(fd, skid, client, sslname)
    if not srey.ssl_exchange(fd, skid, client, sslname) then
        return false
    end
    return srey.wait_ssl_exchanged(fd, skid)
end
function srey.wait_ssl_exchanged(fd, skid)
    local msg = _coro_wait(skid, MSG_TYPE.SSLEXCHANGED, TIMEOUT.NETREAD)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid)
        WARN("ssl exchange timeout, skid %s.", tostring(skid))
        return false
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        WARN("connction closed, skid %s.", tostring(skid))
        return false
    end
    return true
end
local function _net_ssl_exchanged_dispatch(msg)
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.SSLEXCHANGED)
    if not corosess then
        local func = func_cbs[MSG_TYPE.SSLEXCHANGED]
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client)
        end
    else
        _coro_resume(corosess.coro, msg)
    end
end

--func(pktype, fd, skid, client, erro)
function srey.on_handshaked(func)
    func_cbs[MSG_TYPE.HANDSHAKED] = func
end
--bool data size
function srey.wait_handshaked(fd, skid)
    local msg = _coro_wait(skid, MSG_TYPE.HANDSHAKED, TIMEOUT.NETREAD)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid)
        WARN("handshake timeout, skid %s.", tostring(skid))
        return false
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        WARN("handshake connction closed, skid %s.", tostring(skid))
        return false
    end
    return ERR_OK == msg.erro, msg.data, msg.size
end
local function _net_handshaked_dispatch(msg)
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.HANDSHAKED)
    if not corosess then
        local func = func_cbs[MSG_TYPE.HANDSHAKED]
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.erro)
        end
    else
        _coro_resume(corosess.coro, msg)
    end
end

--func(pktype, fd, skid, client, slice, data, size)
function srey.on_recved(func)
    func_cbs[MSG_TYPE.RECV] = func
end
function srey.send(fd, skid, data, size, copy)
    core.send(fd, skid, data, size, copy)
end
local function _wait_net_recv(fd, skid)
    local msg = _coro_wait(skid, MSG_TYPE.RECV, TIMEOUT.NETREAD)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid)
        WARN("send timeout, skid %s.", tostring(skid))
        return nil
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        WARN("connction closed, skid %s.", tostring(skid))
        return nil
    end
    return msg
end
--data size
function srey.syn_send(fd, skid, data, size, copy)
    srey.sock_session(fd, skid, skid)
    srey.send(fd, skid, data, size, copy)
    local msg = _wait_net_recv(fd, skid)
    if not msg then
        return nil
    end
    return msg.data, msg.size
end
--bool fin data size
function srey.syn_slice(fd, skid)
    local msg = _wait_net_recv(fd, skid)
    if not msg then
        return false
    end
    return true, SLICE_TYPE.END == msg.slice, msg.data, msg.size
end
--bool  fd pktype HTTP
function srey.net_call(fd, skid, dst, reqtype, key, data, size)
    local reqdata, reqsize = harbor.pack(dst, 1, reqtype, key, data, size)
    local respdata, _ = srey.syn_send(fd, skid, reqdata, reqsize, 0)
    if not respdata then
        WARN("syn_send error, skid %s.", tostring(skid))
        return false
    end
    local status = http.status(respdata)
    if not status then
        WARN("not have status, skid %s.", tostring(skid))
        return false
    end
    return "200" == status[2]
end
--data size
function srey.net_request(fd, skid, dst, reqtype, key, data, size)
    local reqdata, reqsize = harbor.pack(dst, 0, reqtype, key, data, size)
    local respdata, _ = srey.syn_send(fd, skid, reqdata, reqsize, 0)
    if not respdata then
        WARN("syn_send error, skid %s.", tostring(skid))
        return nil
    end
    local status = http.status(respdata)
    if not status then
        WARN("not have status, skid %s.", tostring(skid))
        return nil
    end
    if "200" ~= status[2] then
        WARN("net request return code %s skid %s.", status[2], tostring(skid))
        return nil
    end
    return http.data(respdata)
end
local function _net_recv_dispatch(msg)
    if 0 == msg.sess then
        if 0 ~=  msg.slice then
            local corosess = _get_coro_sess(msg.skid, MSG_TYPE.RECV)
            if not corosess then
                local func = func_cbs[MSG_TYPE.RECV]
                if func then
                    _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.slice, msg.data, msg.size)
                end
            else
                msg.sess = msg.skid
                _coro_resume(corosess.coro, msg)
            end
        else
            local func = func_cbs[MSG_TYPE.RECV]
            if func then
                _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.slice, msg.data, msg.size)
            end
        end
        return
    end
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.RECV)
    if not corosess then
        WARN("can't find session %s.", tostring(msg.skid))
        return
    end
    _coro_resume(corosess.coro, msg)
end

--func(pktype, fd, skid, client, size)
function srey.on_sended(func)
    func_cbs[MSG_TYPE.SEND] = func
end
local function _net_sended_dispatch(msg)
    local func = func_cbs[MSG_TYPE.SEND]
    if func then
        _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.size)
    end
end

--func(pktype, fd, skid, client)
function srey.on_closed(func)
    func_cbs[MSG_TYPE.CLOSE] = func
end
function srey.close(fd, skid)
    core.close(fd, skid)
end
local function _net_close_dispatch(msg)
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.CLOSE)
    if corosess then
        _coro_resume(corosess.coro, msg)
    end
    local func = func_cbs[MSG_TYPE.CLOSE]
    if func then
        _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client)
    end
end

--func(fd, skid, ip, port, data, size)
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
function srey.syn_sendto(fd, skid, ip, port, data, size)
    srey.sock_session(fd, skid, skid)
    if not srey.sendto(fd, skid, ip, port, data, size) then
        srey.sock_session(fd, skid, 0)
        WARN("sendto error, skid %s.", tostring(skid))
        return nil
    end
    local msg = _coro_wait(skid, MSG_TYPE.RECVFROM, TIMEOUT.NETREAD)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.sock_session(fd, skid, 0)
        WARN("sendto timeout, skid %s.", tostring(skid))
        return nil
    end
    return msg.udata, msg.size
end
local function _net_recvfrom_dispatch(msg)
    if 0 == msg.sess then
        local func = func_cbs[MSG_TYPE.RECVFROM]
        if func then
            _coro_run(_coro_cb, func, msg.fd, msg.skid, msg.ip, msg.port, msg.udata, msg.size)
        end
        return
    end
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.RECVFROM)
    if not corosess then
        WARN("can't find session %s.", tostring(msg.skid))
        return
    end
    _coro_resume(corosess.coro, msg)
end

local function _coro_timeout()
    if nyield > 0 then
        local now = srey.timer_ms()
        local del = {}
        for key, value in pairs(coro_sess) do
            if value.timeout > 0 and now >= value.timeout then
                del[#del + 1] = key
            end
        end
        local corosess
        for _, value in ipairs(del) do
            corosess = _get_coro_sess(value)
            if corosess then
                local msg = {
                    mtype = MSG_TYPE.TIMEOUT,
                    sess = value
                }
                WARN("resume timeout session %s.", tostring(value))
                _coro_resume(corosess.coro, msg)
            end
        end
    end
    srey.timeout(200, _coro_timeout)
end
function message_dispatch(msg)
    if MSG_TYPE.STARTUP == msg.mtype then
        srey.timeout(200, _coro_timeout)
        _startup_dispatch()
    elseif MSG_TYPE.CLOSING == msg.mtype then
        _closing_dispatch()
    elseif MSG_TYPE.TIMEOUT == msg.mtype then
        _timeout_dispatch(msg)
    elseif MSG_TYPE.ACCEPT == msg.mtype then
        _net_accept_dispatch(msg)
    elseif MSG_TYPE.CONNECT == msg.mtype then
        _net_connect_dispatch(msg)
    elseif MSG_TYPE.SSLEXCHANGED == msg.mtype then
        _net_ssl_exchanged_dispatch(msg)
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
