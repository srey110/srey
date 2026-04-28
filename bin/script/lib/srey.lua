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
local CORO_POOL_MAX = 128
local coro_running = nil
local coro_sess = {}
local func_cbs = {}
local srey = {}
local nyield = 0
local coro_pool     = {}
local coroinfo_pool = {}
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
local SLICE_TYPE = {
    START = 0x01,
    SLICE = 0x02,
    END   = 0x04,
}

local function _coroinfo_acquire()
    return tremove(coroinfo_pool) or {}
end
local function _coroinfo_release(ci)
    ci.timeout = nil
    ci.coro    = nil
    ci.mtype   = nil
    ci.func    = nil
    ci.args    = nil
    coroinfo_pool[#coroinfo_pool + 1] = ci
end

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

-- 从协程池取一个空闲协程并绑定新任务函数 func，池为空时新建。
-- 协程生命周期：
--   1. 首次创建：以 func 为入口启动，执行完毕后进入复用循环。
--   2. 复用循环：
--      a. 将 func 置 nil（断开对上一个任务闭包的引用，便于 GC）。
--      b. 若池未满，将自身压入 coro_pool，然后 yield 让出控制权，
--         等待下一次被 coroutine_resume(coro, newFunc) 唤醒。
--      c. 被唤醒后收到 newFunc，再次 yield 等待调用方传入实参 (...)，
--         收到实参后执行 newFunc(...)，执行完毕回到步骤 a。
--      d. 若池已满（>= CORO_POOL_MAX），break 退出循环，协程自然结束，
--         由 GC 回收，避免池无界增长导致大量协程栈长期占用内存。
-- 复用路径（池命中）：
--   直接 coroutine_resume(coro, func) 将 func 注入已在 yield 等待的协程，
--   协程收到 func 后再次 yield 等待实参，调用方拿到 coro 后继续传参。
local function _coro_create(func)
    local coro = tremove(coro_pool)
    if not coro then
        -- 池为空，新建协程；协程体捕获 func/coro 两个 upvalue
        coro = coroutine_create(
            function(...)
                func(...)           -- 执行首次传入的任务
                while true do
                    func = nil      -- 释放上一个任务的引用
                    if #coro_pool >= CORO_POOL_MAX then
                        break       -- 池满，协程退出，交 GC 回收
                    end
                    coro_pool[#coro_pool + 1] = coro  -- 归还到池
                    func = coroutine_yield()           -- 等待下一个任务函数
                    if not func then                   -- nil 守卫：防止调用方传入 nil
                        break
                    end
                    func(coroutine_yield())            -- 等待实参后执行
                end
            end)
    else
        -- 池命中：将新 func 注入正在 yield 处等待的协程
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
function srey.set_request_timeout(ms)
    task.set_request_timeout(ms)
end
function srey.get_request_timeout()
    return task.get_request_timeout()
end
function srey.set_connect_timeout(ms)
    task.set_connect_timeout(ms)
end
function srey.get_connect_timeout()
    return task.get_connect_timeout()
end
function srey.set_netread_timeout(ms)
    task.set_netread_timeout(ms)
end
function srey.get_netread_timeout()
    return task.get_netread_timeout()
end
--func()
function srey.startup(func)
    func_cbs[MSG_TYPE.STARTUP] = func
end
local function _startup_dispatch()
    local func = func_cbs[MSG_TYPE.STARTUP]
    if func then
        _coro_run(_coro_cb, func)
    end
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

local function _set_coro_sess(disposable, coro, sess, mtype, ms, func, ...)
    local timeout = 0
    if ms > 0 then
        timeout = srey.timer_ms() + ms
    end
    local coroinfo = _coroinfo_acquire()
    coroinfo.timeout = timeout
    coroinfo.coro    = coro
    coroinfo.mtype   = mtype
    coroinfo.func    = func
    coroinfo.args    = func and {...} or nil
    if disposable then
        assert(not coro_sess[sess], "repeat session")
        coro_sess[sess] = {
            disposable = disposable,
            coroinfo = coroinfo
        }
    else
        local corosess = coro_sess[sess]
        if not corosess then
            coro_sess[sess] = {
                disposable = disposable,
                coroinfo = {coroinfo}
            }
        else
            table.insert(corosess.coroinfo, coroinfo)
        end
    end
end
local function _get_coro_sess(sess, mtype)
    local corosess = coro_sess[sess]
    if not corosess then
        return nil
    end
    if corosess.disposable then
        if mtype ~= corosess.coroinfo.mtype and MSG_TYPE.CLOSE ~= mtype then
            return nil
        end
        coro_sess[sess] = nil
        return corosess
    end
    if 0 == #corosess.coroinfo then
        return nil
    end
    if mtype ~= corosess.coroinfo[1].mtype and MSG_TYPE.CLOSE ~= mtype then
        return nil
    end
    if MSG_TYPE.CLOSE == mtype then
        coro_sess[sess] = nil
    end
    return corosess
end
local function _coro_info(corosess)
    if corosess.disposable then
        return corosess.coroinfo
    end
    if 0 == #corosess.coroinfo then
        return nil
    end
    local coroinfo = corosess.coroinfo[1]
    table.remove(corosess.coroinfo, 1)
    return coroinfo
end
local function _coro_wait(disposable, sess, mtype, ms)
    _set_coro_sess(disposable, coro_running, sess, mtype, ms)
    nyield = nyield + 1
    local msg = coroutine_yield()
    nyield = nyield - 1
    assert(sess == msg.sess, "different session.")
    return msg
end

function srey.sleep(ms)
    local sess = srey.id()
    core.timeout(sess, ms)
    _coro_wait(true, sess, MSG_TYPE.TIMEOUT, 0)
end
--func(...)
function srey.timeout(ms, func, ...)
    local sess = srey.id()
    _set_coro_sess(true, nil, sess, MSG_TYPE.TIMEOUT, 0, func, ...)
    core.timeout(sess, ms)
end
local function _timeout_dispatch(msg)
    local corosess = _get_coro_sess(msg.sess, MSG_TYPE.TIMEOUT)
    if not corosess then
        WARN("can't find session %s.", tostring(msg.sess))
        return
    end
    local coroinfo = _coro_info(corosess)
    if not coroinfo then
        WARN("can't find session %s.", tostring(msg.sess))
        return
    end
    if coroinfo.func then
        local func, args = coroinfo.func, coroinfo.args
        _coroinfo_release(coroinfo)
        _coro_run(_coro_cb, func, tunpack(args))
    elseif coroinfo.coro then
        local coro = coroinfo.coro
        _coroinfo_release(coroinfo)
        _coro_resume(coro, msg)
    else
        WARN("coroinfo has neither func nor coro, sess %s.", tostring(msg.sess))
        _coroinfo_release(coroinfo)
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
    local msg = _coro_wait(true, sess, MSG_TYPE.RESPONSE, srey.get_request_timeout())
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
    local coroinfo = _coro_info(corosess)
    if coroinfo then
        local coro = coroinfo.coro
        _coroinfo_release(coroinfo)
        _coro_resume(coro, msg)
    end
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
    local msg = _coro_wait(false, skid, MSG_TYPE.CONNECT, srey.get_connect_timeout())
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
    srey.sock_session(fd, skid, skid)
    return fd, skid
end
local function _net_connect_dispatch(msg)
    local func = func_cbs[MSG_TYPE.CONNECT]
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.CONNECT)
    if not corosess then
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.erro)
        end
    else
        local coroinfo = _coro_info(corosess)
        if coroinfo then
            local coro = coroinfo.coro
            _coroinfo_release(coroinfo)
            _coro_resume(coro, msg)
        else
            if func then
                _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.erro)
            end
        end
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
    return core.ssl_exchange(fd, skid, client, ssl)
end
function srey.syn_ssl_exchange(fd, skid, client, sslname)
    if not srey.ssl_exchange(fd, skid, client, sslname) then
        return false
    end
    return srey.wait_ssl_exchanged(fd, skid)
end
function srey.wait_ssl_exchanged(fd, skid)
    local msg = _coro_wait(false, skid, MSG_TYPE.SSLEXCHANGED, srey.get_netread_timeout())
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
    local func = func_cbs[MSG_TYPE.SSLEXCHANGED]
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.SSLEXCHANGED)
    if not corosess then
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client)
        end
    else
        local coroinfo = _coro_info(corosess)
        if coroinfo then
            local coro = coroinfo.coro
            _coroinfo_release(coroinfo)
            _coro_resume(coro, msg)
        else
            if func then
                _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client)
            end
        end
    end
end

--func(pktype, fd, skid, client, erro, data, size)
function srey.on_handshaked(func)
    func_cbs[MSG_TYPE.HANDSHAKED] = func
end
--bool data size
function srey.wait_handshaked(fd, skid)
    local msg = _coro_wait(false, skid, MSG_TYPE.HANDSHAKED, srey.get_netread_timeout())
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
    local func = func_cbs[MSG_TYPE.HANDSHAKED]
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.HANDSHAKED)
    if not corosess then
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.erro, msg.data, msg.size)
        end
    else
        local coroinfo = _coro_info(corosess)
        if coroinfo then
            local coro = coroinfo.coro
            _coroinfo_release(coroinfo)
            _coro_resume(coro, msg)
        else
            if func then
                _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.erro, msg.data, msg.size)
            end
        end
    end
end

--func(pktype, fd, skid, client, slice, data, size)
function srey.on_recved(func)
    func_cbs[MSG_TYPE.RECV] = func
end
function srey.send(fd, skid, data, size, copy)
    return core.send(fd, skid, data, size, copy)
end
local function _wait_net_recv(fd, skid)
    local msg = _coro_wait(false, skid, MSG_TYPE.RECV, srey.get_netread_timeout())
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
    if not srey.send(fd, skid, data, size, copy) then
        return nil
    end
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
    local func = func_cbs[MSG_TYPE.RECV]
    if 0 == msg.sess or not core.may_resume(msg.pktype, msg.data) then
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.slice, msg.data, msg.size)
        end
        return
    end
    local corosess = _get_coro_sess(msg.sess, MSG_TYPE.RECV)
    if not corosess then
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.slice, msg.data, msg.size)
        end
        return
    end
    local coroinfo = _coro_info(corosess)
    if coroinfo then
        local coro = coroinfo.coro
        _coroinfo_release(coroinfo)
        _coro_resume(coro, msg)
    else
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.slice, msg.data, msg.size)
        end
    end
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
    local func = func_cbs[MSG_TYPE.CLOSE]
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.CLOSE)
    if corosess then
        local coroinfo, coro
        while true do
            coroinfo = _coro_info(corosess)
            if not coroinfo then
                break
            end
            coro = coroinfo.coro
            _coroinfo_release(coroinfo)
            _coro_resume(coro, msg)
        end
    end
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
    end
    if not port then
        port = 0
    end
    return core.udp(ip, port)
end
function srey.sendto(fd, skid, ip, port, data, size, copy)
    return core.sendto(fd, skid, ip, port, data, size, copy)
end
--data size
function srey.syn_sendto(fd, skid, ip, port, data, size, copy)
    srey.sock_session(fd, skid, skid)
    if not srey.sendto(fd, skid, ip, port, data, size, copy) then
        srey.sock_session(fd, skid, 0)
        WARN("sendto error, skid %s.", tostring(skid))
        return nil
    end
    local msg = _coro_wait(true, skid, MSG_TYPE.RECVFROM, srey.get_netread_timeout())
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.sock_session(fd, skid, 0)
        WARN("sendto timeout, skid %s.", tostring(skid))
        return nil
    end
    return msg.udata, msg.size
end
local function _net_recvfrom_dispatch(msg)
    local func = func_cbs[MSG_TYPE.RECVFROM]
    if 0 == msg.sess then
        if func then
            _coro_run(_coro_cb, func, msg.fd, msg.skid, msg.ip, msg.port, msg.udata, msg.size)
        end
        return
    end
    local corosess = _get_coro_sess(msg.skid, MSG_TYPE.RECVFROM)
    if not corosess then
        if func then
            _coro_run(_coro_cb, func, msg.fd, msg.skid, msg.ip, msg.port, msg.udata, msg.size)
        end
        return
    end
    local coroinfo = _coro_info(corosess)
    if coroinfo then
        local coro = coroinfo.coro
        _coroinfo_release(coroinfo)
        _coro_resume(coro, msg)
    else
        if func then
            _coro_run(_coro_cb, func, msg.fd, msg.skid, msg.ip, msg.port, msg.udata, msg.size)
        end
    end
end

local function _coro_timeout()
    if nyield > 0 then
        local now = srey.timer_ms()
        local timeout = {}
        for sess, corosess in pairs(coro_sess) do
            local coroinfo
            if corosess.disposable then
                coroinfo = corosess.coroinfo
            else
                if #corosess.coroinfo > 0 then
                    coroinfo = corosess.coroinfo[1]
                end
            end
            if coroinfo and coroinfo.timeout > 0 and now >= coroinfo.timeout then
                timeout[#timeout + 1] = sess
            end
        end
        local corosess
        local coroinfo
        for _, sess in ipairs(timeout) do
            corosess = coro_sess[sess]
            if not corosess then
                goto continue
            end
            if corosess.disposable then
                coro_sess[sess] = nil
            end
            coroinfo = _coro_info(corosess)
            if coroinfo then
                local coro = coroinfo.coro
                _coroinfo_release(coroinfo)
                local msg = {
                    mtype = MSG_TYPE.TIMEOUT,
                    sess = sess
                }
                _coro_resume(coro, msg)
                WARN("resume timeout session %s.", tostring(sess))
            end
            ::continue::
        end
    end
    srey.timeout(3 * 1000, _coro_timeout)
end
function message_dispatch(msg)
    if MSG_TYPE.STARTUP == msg.mtype then
        srey.timeout(3 * 1000, _coro_timeout)
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
