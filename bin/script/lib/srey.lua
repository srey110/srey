-- srey 核心框架模块（Lua 侧）。
-- 负责：协程池管理、会话挂起/恢复、消息分发，以及对外暴露全部网络/任务 API。
-- 每个 task 脚本通过 require("lib.srey") 获取此模块，所有 I/O 操作均在此封装。
-- 协程模型：所有网络操作均为"同步写法、异步执行"——调用方协程在 yield 处挂起，
-- 由 message_dispatch 在收到对应消息后 resume，对用户代码透明。

require("lib.define")
require("lib.utils")
require("lib.log")
local task   = require("srey.task")
local utils  = require("srey.utils")
local core   = require("srey.core")
local http   = require("srey.http")
local harbor = require("srey.harbor")
local trend  = require("srey.trend")
local xpcall           = xpcall
local tunpack          = table.unpack
local tremove          = table.remove
local tpack            = table.pack
local coroutine_create = coroutine.create
local coroutine_yield  = coroutine.yield
local coroutine_resume = coroutine.resume
local cur_task  = _curtask   -- 当前 task 的 C 层指针，由 loader 注入
local TASK_NAME = TASK_NAME
local SSL_NAME  = SSL_NAME
local REQUEST_TYPE = REQUEST_TYPE
local CORO_POOL_MAX      = 64    -- 协程池上限；超出后空闲协程自然退出由 GC 回收
local CORO_POOL_MIN_KEEP = 4     -- 协程池收缩底线
local coro_running   = nil   -- 当前正在执行的协程（用于 _set_coro_sess 记录 coro）
local coro_sess      = {}    -- 会话表：sess/skid → corosess，保存挂起协程的等待信息
local func_cbs       = {}    -- 消息类型 → 用户注册回调函数
local srey  = {}
local nyield = 0             -- 当前挂起等待的协程数量（closing 时用于告警）
local coro_pool       = {}            -- 空闲协程池
local coro_pool_trend = trend.new()   -- 协程池负载趋势
local fork_queue      = {}            -- srey.fork 待执行任务队列：{ func, args }，在 message_dispatch 末尾批量起协程

-- 消息类型枚举，与 C 层 MSG_TYPE 一一对应。
---@enum MSG_TYPE
local MSG_TYPE = {
    STARTUP      = 0x01,   -- task 启动
    CLOSING      = 0x02,   -- task 即将关闭
    TIMEOUT      = 0x03,   -- 定时器超时
    ACCEPT       = 0x04,   -- 新连接到来（服务端）
    CONNECT      = 0x05,   -- 连接建立完成（客户端）
    SSLEXCHANGED = 0x06,   -- TLS 握手完成
    HANDSHAKED   = 0x07,   -- 应用层握手完成（MySQL/SMTP/WebSocket）
    RECV         = 0x08,   -- 收到数据包
    SEND         = 0x09,   -- 数据发送完成
    CLOSE        = 0x0a,   -- 连接关闭
    RECVFROM     = 0x0b,   -- UDP 收到数据
    REQUEST      = 0x0c,   -- 收到跨 task 请求
    RESPONSE     = 0x0d,   -- 收到跨 task 响应
    FORK         = 0x0e    -- coro_fork 自发消息（C 层内部 mtype，Lua 业务一般不用）
}
-- 数据分片标志（对应 C 层 slice_type）
local SLICE_TYPE = {
    START = 0x01,   -- 分片开始
    SLICE = 0x02,   -- 中间分片
    END   = 0x04,   -- 最后一片（完整消息）
}

---带错误捕获的函数调用；异常时自动打印错误信息和调用栈
---@param func function 待调用函数
---@param ... any 函数参数
---@return boolean ok 是否成功
---@return any ... func 的返回值（成功）或错误信息（失败）
local function _xpcall_error(err)
    ERROR("%s\n%s.", err, debug.traceback())
    return err
end

function srey.xpcall(func, ...)
    return xpcall(func, _xpcall_error, ...)
end

---将字符串编译为函数后执行；编译或运行失败时打印错误
---@param str string Lua 源码字符串
---@return boolean ok 是否执行成功
function srey.dostring(str)
    local func, err = load(str)
    if not func then
        ERROR("%s.\n%s.", err, debug.traceback())
        return false
    end
    return srey.xpcall(func)
end

---从协程池取一个空闲协程绑定新任务，池为空时新建；执行完毕后协程归还池中复用，
---池满（&gt;= CORO_POOL_MAX）时协程退出由 GC 回收，避免池无界增长
---@param func function 任务函数
---@return thread coro 协程对象
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
        local ok, err = coroutine_resume(coro, func)
        if not ok then
            ERROR("coroutine error: %s", tostring(err))
        end
    end
    return coro
end

local function _coro_pool_shrink()
    local cur = #coro_pool
    if coro_pool_trend:busy(cur, 4, 5) then
        return
    end
    if cur <= CORO_POOL_MIN_KEEP then
        return
    end
    local keep = cur >> 1
    if keep < CORO_POOL_MIN_KEEP then
        keep = CORO_POOL_MIN_KEEP
    end
    while #coro_pool > keep do
        local coro = tremove(coro_pool)
        -- resume(coro, nil) 触发 _coro_create 内 `if not func then break` 守卫，
        -- 让协程主体主动退出释放栈帧/upvalue，避免仅靠 GC 延迟回收
        coroutine_resume(coro, nil)
    end
end

---恢复协程执行；同时更新 coro_running 以便 _set_coro_sess 能记录当前协程；
---协程内部 panic 时捕获错误并打印，不向上层抛出。
---所有唤醒协程的入口必须走此函数，禁止裸调 coroutine.resume：
---否则 coro_running 不同步，被唤醒协程内的 _coro_wait/sleep/call 会把已归还池的旧 coro 登记到 coro_sess，
---后续消息按错误协程 resume，触发 yield#1 注入 msg 表 → "attempt to call a table value" 类型错。
---此处还通过 task.active 通知 C 层 active_lua 字段，使跨 task 的 task.trap(name) 能定位到
---正在执行字节码的 thread 并安装中断 hook；resume 结束后还原回主 thread。
---@param coro thread 协程对象
---@param ... any 传给协程的参数
local function _coro_resume(coro, ...)
    coro_running = coro
    task.active(coro)
    local ok, err = coroutine_resume(coro_running, ...)
    task.active()
    if not ok then
        ERROR("coroutine error: %s", tostring(err))
    end
end

---从池中取协程（或新建）并立即 resume 执行 func(...)
---@param func function 协程任务函数
---@param ... any 传给 func 的参数
local function _coro_run(func, ...)
    _coro_resume(_coro_create(func), ...)
end

---协程包装器：执行前 incref 防止 task 被提前销毁，执行后 ungrab 释放引用
---@param func function 业务回调
---@param ... any 传给 func 的参数
local function _coro_cb(func, ...)
    srey.task_incref(cur_task)
    srey.xpcall(func, ...)
    srey.task_ungrab(cur_task)
end

---将函数 f 与参数预绑定，返回一个无参 lambda，调用即 f(args)。
---用于 srey.fork / srey.fork_wait 等接收 () -> any 的 API，减少 function() return ... end 样板。
---@param f function 待绑定函数
---@param ... any 预绑定的参数（用闭包捕获，每次调用 lambda 都传给 f）
---@return fun(): any wrapped 无参 lambda
function srey.fork_bind(f, ...)
    local args = tpack(...)
    return function() return f(tunpack(args, 1, args.n)) end
end

---立即在新协程中执行 func(...)（fire-and-forget）；当前协程不让出；
---新协程在本条消息 dispatch 完成后的 _drain_fork_queue 里被起，不走时间轮；
---错误由 _coro_cb 的 xpcall 捕获并打 ERROR 日志（不会传播到主协程）。
---@param func function 协程任务函数
---@param ... any 传给 func 的参数
function srey.fork(func, ...)
    fork_queue[#fork_queue + 1] = { func = func, args = tpack(...) }
end

---并发执行 funcs 中所有无参函数，等全部完成（或抛错）后返回每个任务的状态与结果。
---结果数组与 funcs 同序，每项形如 { ok=true, val=<返回值> } 或 { ok=false, val=<错误信息字符串> }。
---调用方必须身处协程（startup/timeout/on_* 回调内部均满足）。
---@param funcs (fun(): any)[]  并发任务列表（每个为无参 lambda，参数用闭包捕获）
---@return { ok: boolean, val: any }[] results 与 funcs 同序的状态结果数组
function srey.fork_wait(funcs)
    local n = #funcs
    if 0 == n then
        return {}
    end
    local barrier = {
        results = {},
        pending = n,
        waiter  = coro_running,
    }
    for i = 1, n do
        local f = funcs[i]
        srey.fork(function()
            -- srey.xpcall 内部捕获 f 抛错，保证 barrier.pending 永远递减到 0，避免主协程死等
            local ok, ret = srey.xpcall(f)
            barrier.results[i] = { ok = ok, val = ret }
            barrier.pending = barrier.pending - 1
            if 0 == barrier.pending then
                _coro_resume(barrier.waiter)
            end
        end)
    end
    coroutine_yield()
    return barrier.results
end

---创建一个协程串行化执行器。同 task 内多协程对同一资源并发访问时串行进入，避免穿插；
---同一协程嵌套调用安全（ref 计数）；f 抛错由 srey.xpcall 捕获并打 ERROR 日志，锁照常释放，
---下个等待者继续。
---@return fun(f:function, ...):boolean,any serial 串行化调用器；返回 srey.xpcall 的 (ok, f 的首返回值)
function srey.serial()
    local current = nil   -- 当前持锁协程
    local ref = 0         -- 嵌套深度（同协程多次进入累加）
    local waiters = {}    -- 等待协程 FIFO 队列
    local function _release()
        ref = ref - 1
        if 0 == ref then
            local nxt = tremove(waiters, 1)
            if nxt then
                -- 唤醒前先设 current/ref，nxt 内查询时拿到一致状态
                current = nxt
                ref = 1
                _coro_resume(nxt)
            else
                current = nil
            end
        end
    end
    return function(f, ...)
        local self = coro_running
        if current and current ~= self then
            waiters[#waiters + 1] = self
            coroutine_yield()
            -- 被唤醒时 current=self, ref=1 已由 _release 设置
        else
            if not current then
                current = self
            end
            ref = ref + 1
        end
        local ok, ret = srey.xpcall(f, ...)
        _release()
        -- _release 内若唤醒 waiter 且 waiter 在 cs 内 yield，_coro_resume 已把 coro_running 改写为 waiter；
        -- 本协程返回上层前必须还原为 self，否则上层下次 srey.* yield 类 API 通过 _set_coro_sess 把
        -- stale waiter 登记到 sess，后续消息错协程 resume 触发 "attempt to call a table value"
        coro_running = self
        -- active_lua 同样被 _coro_resume 切到 waiter，需一并还原 self，否则窗口内 task.trap 把中断 hook 装主 thread 致延迟
        task.active(self)
        return ok, ret
    end
end

---消费 fork_queue 中所有待执行任务（含嵌套 fork），逐个起新协程；
---嵌套 fork 写入同一队列尾，本次循环持续消费直到全部完成
local function _drain_fork_queue()
    if 0 == #fork_queue then
        return
    end
    -- 头索引推进 + 不清 nil（避免 #fork_queue 出 hole），末尾一次性清空，O(N) 无 tremove memmove
    local i = 1
    while i <= #fork_queue do
        local item = fork_queue[i]
        i = i + 1
        _coro_run(_coro_cb, item.func, tunpack(item.args, 1, item.args.n))
    end
    for j = 1, i - 1 do
        fork_queue[j] = nil
    end
end

---生成全局唯一 64 位整数 ID（自增序列，由 C 层实现）
---@type fun():integer
srey.id = utils.id

---从 srey.id 生成的 ID 中解析出服务器 id（高 16 位，0..0x7FFF）
---@type fun(id:integer):integer
srey.parse_svid = utils.parse_svid

---将 C 层 userdata 指针转换为 Lua 字符串
---@type fun(data:lightuserdata, size:integer):string?
srey.ud_str = utils.ud_str

---将 C 层 userdata 转换为十六进制字符串（调试用）
---@type fun(data:lightuserdata, size:integer):string
srey.hex = utils.hex

---获取指定 fd 对端的 IP 地址和端口
---@type fun(fd:integer):string?, integer?
srey.remote_addr = utils.remote_addr

---注册新 task；变参作为脚本 chunk 的 `...` 传入，脚本顶层 `local a,b,...= ...` 接收
---@type fun(file:string, name:string?, mpqcap:integer, ...:any):lightuserdata?
srey.task_register = task.register

---关闭指定 task，发送 CLOSING 消息并等待其退出
---@type fun(taskctx:lightuserdata?)
srey.task_close = task.close

---按 name 查找 task 并增加引用计数；使用完毕后必须调用 task_ungrab 释放
---@type fun(name:TASK_NAME|integer):lightuserdata?
srey.task_grab = task.grab

---增加 task 引用计数，防止在使用期间被销毁
---@type fun(taskctx:lightuserdata)
srey.task_incref = task.incref

---释放 task 引用（与 grab/incref 配对）
---@type fun(taskctx:lightuserdata)
srey.task_ungrab = task.ungrab

---查询 task 是否正在关闭
---@type fun(taskctx:lightuserdata?):boolean
srey.isclosing = task.isclosing

---查询 task 是否正在关闭
---@type fun(taskctx:lightuserdata?):TASK_TYPE
srey.get_type = task.get_type

---返回 task 的字符串名；匿名 task 或不存在返回 nil
---@type fun(taskctx:lightuserdata?):TASK_NAME?
srey.task_name = task.name

---返回 task 的数字句柄（createid 生成，用于与消息回调里的 src 比对）
---@type fun(taskctx:lightuserdata?):integer
srey.task_handle = task.handle

---返回当前单调时钟毫秒数（用于超时计算）
---@type fun():integer
srey.timer_ms = task.timer_ms

---设置跨 task request/response 等待超时
---@type fun(ms:integer)
srey.set_request_timeout = task.set_request_timeout

---获取跨 task request/response 等待超时
---@type fun():integer
srey.get_request_timeout = task.get_request_timeout

---设置 TCP/TLS 连接建立超时
---@type fun(ms:integer)
srey.set_connect_timeout = task.set_connect_timeout

---获取 TCP/TLS 连接建立超时
---@type fun():integer
srey.get_connect_timeout = task.get_connect_timeout

---设置网络读（recv/handshake/ssl exchange）超时
---@type fun(ms:integer)
srey.set_netread_timeout = task.set_netread_timeout

---获取网络读超时
---@type fun():integer
srey.get_netread_timeout = task.get_netread_timeout

---设置当前 task 调度优先级。priority 越大单轮消费消息越多;每 +8 翻倍,每 +1 +12.5%;0..16,超界自动 clamp
---@type fun(priority:integer)
srey.set_priority = task.set_priority

---获取当前 task 调度优先级 (0..16)
---@type fun():integer
srey.get_priority = task.get_priority

---注册 task 启动回调；task 进入事件循环后首先触发一次
---@param func fun() 启动回调
function srey.startup(func)
    func_cbs[MSG_TYPE.STARTUP] = func
end

local function _startup_dispatch()
    local func = func_cbs[MSG_TYPE.STARTUP]
    if func then
        _coro_run(_coro_cb, func)
    end
end

---注册 task 关闭回调；在收到 CLOSING 消息时调用，用于清理资源
---@param func fun() 关闭回调
function srey.closing(func)
    func_cbs[MSG_TYPE.CLOSING] = func
end

---内部关闭处理：调用用户注册的 closing 回调，结束后 ungrab cur_task 允许 loader 销毁
local function _closing()
    local func = func_cbs[MSG_TYPE.CLOSING]
    if func then
        srey.xpcall(func)
    end
    srey.task_ungrab(cur_task)
end

---CLOSING 消息分发：在协程中执行关闭逻辑，若此时仍有协程挂起则打印告警
local function _closing_dispatch()
    _coro_run(_coro_cb, _closing)
    if nyield > 0 then
        WARN("coro yield %d.", nyield)
    end
end

---@class CoroInfo
---@field timeout integer      到期时刻（毫秒，timer_ms 单位）；0 表示永不超时
---@field since   integer      挂起起始时刻（毫秒，timer_ms 单位）；用于 debug 计算挂起时长
---@field coro    thread?      等待唤醒的协程；func 模式下为 nil
---@field mtype   integer      期望唤醒的消息类型（MSG_TYPE.*）
---@field func    function?    定时回调；非 nil 时由新协程执行而非 resume coro
---@field args    any[]?       传给 func 的参数列表；func 为 nil 时不存在

---@class CoroSession
---@field disposable boolean           true=一次性（单协程等待）；false=可重入（FIFO 队列）
---@field coroinfo   CoroInfo|CoroInfo[]  disposable 时为单个 CoroInfo，否则为 CoroInfo 数组

---将协程或回调函数注册到会话表，等待指定消息类型唤醒
---@param disposable boolean true=一次性会话；false=可重入（同一 skid 可有多等待者，FIFO 消费）
---@param coro thread? 等待唤醒的协程；func 模式下可为 nil
---@param sess integer 会话 id
---@param mtype integer 期望唤醒的消息类型（MSG_TYPE.*）
---@param ms integer 超时毫秒数；0 表示永不超时
---@param func function? 定时回调；非 nil 时消息到达后新建协程执行而非 resume coro
---@param ... any 传给 func 的参数
local function _set_coro_sess(disposable, coro, sess, mtype, ms, func, ...)
    local timeout = 0
    local now = srey.timer_ms()
    if ms > 0 then
        timeout = now + ms
    end
    local coroinfo = {
        timeout = timeout,
        since   = now,
        coro    = coro,
        mtype   = mtype,
        func    = func,
        args    = func and {...} or nil,
    }
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

---从会话表中查找匹配的 corosess；mtype 须与登记时一致或为 CLOSE（强制唤醒）
---@param sess integer 会话 id
---@param mtype integer 消息类型
---@return CoroSession|nil corosess 匹配的会话对象；不匹配返回 nil
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
        coro_sess[sess] = nil
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

---从 corosess 中取出一个 coroinfo；disposable 直接返回，非 disposable 按 FIFO 出队
---@param corosess CoroSession 会话对象
---@return CoroInfo|nil coroinfo 协程信息表
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

---挂起当前协程，等待指定会话的消息
---@param disposable boolean true=一次性会话；false=可重入
---@param sess integer 会话 id
---@param mtype integer 期望唤醒的消息类型
---@param ms integer 超时毫秒数；0 表示永不超时
---@return Message msg 触发 resume 的消息表
local function _coro_wait(disposable, sess, mtype, ms)
    _set_coro_sess(disposable, coro_running, sess, mtype, ms)
    nyield = nyield + 1
    local msg = coroutine_yield()
    nyield = nyield - 1
    assert(sess == msg.sess, "different session.")
    return msg
end

---阻塞当前协程指定毫秒（底层使用定时器，不阻塞事件线程）
---@param ms integer 睡眠毫秒数
function srey.sleep(ms)
    local sess = srey.id()
    core.timeout(sess, ms)
    _coro_wait(true, sess, MSG_TYPE.TIMEOUT, 0)
end

---异步定时器：ms 毫秒后在新协程中调用 func(...)，当前协程不挂起
---@param ms integer 延迟毫秒数
---@param func function 超时回调
---@param ... any 传给 func 的参数
function srey.timeout(ms, func, ...)
    local sess = srey.id()
    _set_coro_sess(true, nil, sess, MSG_TYPE.TIMEOUT, 0, func, ...)
    core.timeout(sess, ms)
end

---@param msg Message
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
        _coro_run(_coro_cb, func, tunpack(args))
    elseif coroinfo.coro then
        local coro = coroinfo.coro
        _coro_resume(coro, msg)
    else
        WARN("coroinfo has neither func nor coro, sess %s.", tostring(msg.sess))
    end
end

---注册跨 task 请求处理回调；收到 REQUEST 消息时在新协程中调用，需主动调 srey.response 发回结果
---@param func fun(reqtype:REQUEST_TYPE, sess:integer, src:integer, data:lightuserdata?, size:integer) 请求回调（src 为发送方数字句柄）
function srey.on_requested(func)
    func_cbs[MSG_TYPE.REQUEST] = func
end

---同步跨 task 请求：挂起当前协程直到收到对端 response 或超时
---@param dst TASK_NAME 目标 task name
---@param reqtype REQUEST_TYPE 业务请求类型
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
---@return lightuserdata|nil rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝；失败/超时返回 nil
---@return integer? rsize 响应数据长度
function srey.request(dst, reqtype, data, size, copy)
    -- 调 core.request 前的早退出路径：copy=0 时调用方已转移所有权,主动 utils.ud_free 兜底
    -- （utils.ud_free 内部仅对 lightuserdata 生效,非 lightuserdata 自动跳过）
    if TASK_NAME.NONE == dst then
        WARN("parameter error.")
        if 0 == copy then
            utils.ud_free(data)
        end
        return nil
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        WARN("grab task error.")
        if 0 == copy then
            utils.ud_free(data)
        end
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

---单向跨 task 消息（fire-and-forget），不等待响应
---@param dst TASK_NAME 目标 task name
---@param reqtype REQUEST_TYPE 业务请求类型
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
function srey.call(dst, reqtype, data, size, copy)
    -- 调 core.call 前的早退出路径：copy=0 时调用方已转移所有权,主动 utils.ud_free 兜底
    if TASK_NAME.NONE == dst then
        WARN("parameter error.")
        if 0 == copy then
            utils.ud_free(data)
        end
        return
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        WARN("grab task error.")
        if 0 == copy then
            utils.ud_free(data)
        end
        return
    end
    core.call(dtask, reqtype, data, size, copy)
    srey.task_ungrab(dtask)
end

-- 多播 grab 公共逻辑：遍历 dsts 跳过 TASK_NAME.NONE 与 grab 失败项,顺序填入 task_ctx 指针数组。
-- 返回数组无 nil 空洞,调用方用 #tasks 取实际有效数,配 _ungrab_multi_tasks 归还引用。
---@param dsts TASK_NAME[]
---@return lightuserdata[] tasks
local function _grab_multi_tasks(dsts)
    local tasks = {}
    for i = 1, #dsts do
        if TASK_NAME.NONE ~= dsts[i] then
            local dtask = srey.task_grab(dsts[i])
            if dtask then
                tasks[#tasks + 1] = dtask
            end
        end
    end
    return tasks
end

-- 多播 ungrab 公共逻辑：归还 _grab_multi_tasks 返回的 tasks 引用
---@param tasks lightuserdata[]
local function _ungrab_multi_tasks(tasks)
    for i = 1, #tasks do
        srey.task_ungrab(tasks[i])
    end
end

---广播请求（fire-and-forget RPC）：同一份 data 投递给 N 个 task,各 dst 可独立 task_response 回当前 task
---（共用同一 sess）。框架不挂起协程不做聚合,响应到达时触发 srey.on_responsed 回调,业务在回调内据 sess
---累计 / 区分。sess 由调用方传入(非 0),典型用法是配合 srey.id() 分配避免与 srey.request 自动 sess 冲突。
---@param dsts TASK_NAME[] 目标 task name 数组；TASK_NAME.NONE 与 grab 失败的项被跳过
---@param reqtype REQUEST_TYPE 业务请求类型
---@param sess integer 会话 id(非 0),N 个 dst 共用此 sess
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
---@return integer valid 实际成功投递的 dst 数（0 表示全部跳过）
function srey.multi_request(dsts, reqtype, sess, data, size, copy)
    -- 所有不进入 core.multi_request 的退出路径都先 utils.ud_free 兜底,
    -- 避免 C 端 longjmp / short-circuit return 时漏 FREE 调用方已转移的 data
    -- （utils.ud_free 内部仅对 lightuserdata 生效,非 lightuserdata 自动跳过）
    if 0 == sess then
        if 0 == copy then
            utils.ud_free(data)
        end
        error("multi_request sess must be non-zero")
    end
    local tasks = _grab_multi_tasks(dsts)
    if 0 == #tasks then
        if 0 == copy then
            utils.ud_free(data)
        end
        return 0
    end
    local valid = core.multi_request(tasks, reqtype, sess, data, size, copy)
    _ungrab_multi_tasks(tasks)
    return valid
end

---单向广播跨 task 消息（fire-and-forget）：同一份 data 投递给 N 个 task，
---C 层 shared_data 引用计数自动释放，比 N 次 srey.call 节省 N-1 份内存拷贝
---@param dsts TASK_NAME[] 目标 task name 数组；TASK_NAME.NONE 与 grab 失败的项被跳过
---@param reqtype REQUEST_TYPE 业务请求类型
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
function srey.multi_call(dsts, reqtype, data, size, copy)
    local tasks = _grab_multi_tasks(dsts)
    if 0 == #tasks then
        -- 调用方 copy=0 时所有权已转移,但本路径不进入 core.multi_call,需主动 utils.ud_free 兜底
        if 0 == copy then
            utils.ud_free(data)
        end
        return
    end
    core.multi_call(tasks, reqtype, data, size, copy)
    _ungrab_multi_tasks(tasks)
end

local _debug_request    -- 懒加载缓存
local _sc_client        -- 懒加载缓存(收到 REQ_SC_DELIVER 时初始化)
-- task 请求分发：REQ_DEBUG 走 lib.debug_request,REQ_SC_DELIVER 走 lib.sc_client._on_deliver,
-- 其余转交用户注册的 on_requested 回调
---@param msg Message
local function _request_dispatch(msg)
    if REQUEST_TYPE.REQ_DEBUG == msg.pktype then
        if not _debug_request then
            _debug_request = require("lib.debug_request")
            _debug_request._set_coro_sess(coro_sess)
            _debug_request._set_response(srey.response)
            _debug_request._set_mtype_names(MSG_TYPE)
            -- 未知 debug 命令透传业务 on_requested；_dispatch 已在协程内,直接调 func 同协程跑
            _debug_request._set_fallback(function(reqtype, sess, src, data, size)
                local func = func_cbs[MSG_TYPE.REQUEST]
                if func then
                    func(reqtype, sess, src, data, size)
                else
                    srey.response(src, sess, ERR_FAILED, "not register request function.")
                end
            end)
        end
        _coro_run(_coro_cb, _debug_request._dispatch, msg.pktype, msg.sess, msg.src, msg.data, msg.size)
    elseif REQUEST_TYPE.REQ_SC_DELIVER == msg.pktype then
        if not _sc_client then
            _sc_client = require("lib.sc_client")
        end
        _coro_run(_coro_cb, _sc_client._on_deliver, msg.data, msg.size)
    else
        local func = func_cbs[MSG_TYPE.REQUEST]
        if not func then
            srey.response(msg.src, msg.sess, ERR_FAILED, "not register request function.")
            return
        end
        _coro_run(_coro_cb, func, msg.pktype, msg.sess, msg.src, msg.data, msg.size)
    end
end

---向请求方 task 回复响应
---@param dst TASK_NAME 请求方 task name
---@param sess integer 请求会话 id
---@param erro integer 错误码，0 表示成功
---@param data string|lightuserdata|nil 响应数据
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
function srey.response(dst, sess, erro, data, size, copy)
    -- 调 core.response 前的早退出路径：copy=0 时调用方已转移所有权,主动 utils.ud_free 兜底
    if TASK_NAME.NONE == dst or 0 == sess then
        WARN("parameter error.")
        if 0 == copy then
            utils.ud_free(data)
        end
        return
    end
    local dtask = srey.task_grab(dst)
    if not dtask then
        WARN("grab task error.")
        if 0 == copy then
            utils.ud_free(data)
        end
        return
    end
    core.response(dtask, sess, erro, data, size, copy)
    srey.task_ungrab(dtask)
end

---@param msg Message
local function _response_dispatch(msg)
    local corosess = _get_coro_sess(msg.sess, MSG_TYPE.RESPONSE)
    if corosess then
        local coroinfo = _coro_info(corosess)
        if coroinfo then
            _coro_resume(coroinfo.coro, msg)
        end
        return
    end
    -- sess 不在 coro_sess（srey.multi_request 等场景）：起新协程跑全局 on_responsed 回调,
    -- 与其他 on_* 回调一致(_coro_cb 自带 task_incref/ungrab + xpcall),
    -- 用户回调内可 yield(coro_wait/sleep/call 等)而不影响主分发循环
    local cb = func_cbs[MSG_TYPE.RESPONSE]
    if cb then
        _coro_run(_coro_cb, cb, msg.sess, msg.erro, msg.data, msg.size)
        return
    end
    WARN("can't find session %s.", tostring(msg.sess))
end

---注册全局 response 回调；srey.request 等协程同步 API 不走此回调,仅当 sess 不在协程等待表时触发
---（典型场景：srey.multi_request 广播 N 个响应共用 sess,框架不做聚合,业务在此回调中据 sess 累计）
---@param func fun(sess:integer, erro:integer, data:lightuserdata?, size:integer) 响应回调
function srey.on_responsed(func)
    func_cbs[MSG_TYPE.RESPONSE] = func
end

---设置 socket 的会话键（sess），后续该 socket 消息携带此值；0 表示清除
---@type fun(fd:integer, skid:integer, sess:integer)
srey.sock_session = core.session

---切换 socket 的应用层协议类型
---@type fun(fd:integer, skid:integer, pktype:PACK_TYPE)
srey.sock_pack_type = core.pack_type

---设置 socket 状态标志（具体含义由协议层定义）
---@type fun(fd:integer, skid:integer, status:integer)
srey.sock_status = core.status

---将 socket 绑定到指定 task（跨 task 推送场景）
---@type fun(fd:integer, skid:integer, tname:TASK_NAME)
srey.sock_bind_task = core.bind_task

---注册新连接 accept 回调；每次有连接进来在新协程中调用
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer) accept 回调
function srey.on_accepted(func)
    func_cbs[MSG_TYPE.ACCEPT] = func
end

---开始监听指定地址
---@param pktype PACK_TYPE 应用层协议类型
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param ip string 监听 IP
---@param port integer 监听端口
---@param netev NET_EV? 事件订阅掩码
---@return integer lsnid 监听 id；失败返回 ERR_FAILED(-1)
function srey.listen(pktype, sslname, ip, port, netev)
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
        if not ssl then
            WARN("ssl_qury not find ssl name %s.", sslname)
            return ERR_FAILED
        end
    end
    return core.listen(pktype, ssl, ip, port, netev)
end

---停止监听（关闭监听 socket），已建立的连接不受影响
---@type fun(lsnid:integer)
srey.unlisten = core.unlisten
---@param msg Message
local function _net_accept_dispatch(msg)
    local func = func_cbs[MSG_TYPE.ACCEPT]
    if func then
        _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid)
    end
end

---注册主动连接结果回调；仅在没有协程等待该连接时触发（非 srey.connect 发起的）
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, err:integer) connect 回调
function srey.on_connected(func)
    func_cbs[MSG_TYPE.CONNECT] = func
end

---同步等待异步 connect 完成（由 task_connect / core.connect 异步发起后调用）；ssl 非 nil 时同时等待 SSL 握手
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param ssl any? 非 nil 时表示需要等待 SSL 握手
---@return boolean ok 成功 true；超时/失败时已关闭 fd 并返回 false
function srey.wait_connect(fd, skid, ssl)
    if INVALID_SOCK == fd then
        return false
    end
    local msg = _coro_wait(false, skid, MSG_TYPE.CONNECT, srey.get_connect_timeout())
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid, 1)
        WARN("connect timeout, skid %s.", tostring(skid))
        return false
    end
    if ERR_OK ~= msg.erro then
        WARN("connect error, skid %s.", tostring(skid))
        return false
    end
    if nil ~= ssl then
        if not srey.wait_ssl_exchanged(fd, skid) then
            return false
        end
    end
    return true
end

---同步发起 TCP/TLS 连接：挂起协程等待连接结果，超时则关闭并返回 INVALID_SOCK；
---成功后若启用 TLS 自动等待 SSL 握手完成，再设置会话键
---@param pktype PACK_TYPE 应用层协议类型
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param ip string 对端 IP
---@param port integer 对端端口
---@param netev NET_EV? 事件订阅掩码
---@param extra lightuserdata? 协议专用附加参数（如 WebSocket 握手验证 key）
---@return integer fd socket fd；失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；仅在 fd 有效时返回
function srey.connect(pktype, sslname, ip, port, netev, extra)
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
        if not ssl then
            WARN("ssl_qury not find ssl name %s.", sslname)
            -- extra 尚未传给 C 层，由本函数释放；
            -- 此路径以外 extra 所有权已转移 C 层（ev_connect 失败由 cbs->ud_free 释放）
            if extra then
                utils.ud_free(extra)
            end
            return INVALID_SOCK
        end
    end
    local fd, skid = core.connect(pktype, ssl, ip, port, netev, extra)
    if INVALID_SOCK == fd then
        WARN("connect %s:%d error.", ip, port)
        return INVALID_SOCK
    end
    if not srey.wait_connect(fd, skid, ssl) then
        return INVALID_SOCK
    end
    srey.sock_session(fd, skid, skid)
    return fd, skid
end

---@param msg Message
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
            _coro_resume(coro, msg)
        else
            if func then
                _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.erro)
            end
        end
    end
end

---注册 TLS 握手完成回调；仅在没有协程等待该事件时触发
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, client:integer) SSL 握手完成回调
function srey.on_ssl_exchanged(func)
    func_cbs[MSG_TYPE.SSLEXCHANGED] = func
end

---异步触发 TLS 握手（非阻塞，结果通过 SSLEXCHANGED 消息通知）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param client integer 1=客户端（发 ClientHello），0=服务端
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 时返回 false
---@return boolean ok 发起成功 true
function srey.ssl_exchange(fd, skid, client, sslname)
    if SSL_NAME.NONE == sslname then
        return false
    end
    local ssl = core.ssl_qury(sslname)
    if not ssl then
        WARN("ssl_qury not find ssl name %s.", sslname)
        return false
    end
    return core.ssl_exchange(fd, skid, client, ssl)
end

---同步 TLS 握手：触发握手并挂起协程等待完成（或超时/断开）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param client integer 1=客户端，0=服务端
---@param sslname SSL_NAME SSL 上下文名
---@return boolean ok 握手成功 true
function srey.syn_ssl_exchange(fd, skid, client, sslname)
    if not srey.ssl_exchange(fd, skid, client, sslname) then
        return false
    end
    return srey.wait_ssl_exchanged(fd, skid)
end

---挂起协程等待 TLS 握手完成事件（SSLEXCHANGED 或 CLOSE / TIMEOUT）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@return boolean ok 握手成功 true；超时/断开返回 false
function srey.wait_ssl_exchanged(fd, skid)
    if INVALID_SOCK == fd then
        return false
    end
    local msg = _coro_wait(false, skid, MSG_TYPE.SSLEXCHANGED, srey.get_netread_timeout())
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid, 1)
        WARN("ssl exchange timeout, skid %s.", tostring(skid))
        return false
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        WARN("connction closed, skid %s.", tostring(skid))
        return false
    end
    return true
end

---@param msg Message
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
            _coro_resume(coro, msg)
        else
            if func then
                _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client)
            end
        end
    end
end

---注册应用层握手完成回调；适用于 MySQL 认证 / SMTP 欢迎行 / WebSocket Upgrade 等协议
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, client:integer, erro:integer, data:lightuserdata?, size:integer) 握手回调
function srey.on_handshaked(func)
    func_cbs[MSG_TYPE.HANDSHAKED] = func
end

---挂起协程等待应用层握手结果
---@param fd integer socket fd
---@param skid integer 连接 skid
---@return boolean ok 握手成功 true；超时/断开/错误返回 false
---@return lightuserdata? data 握手附带数据（可为 nil）；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝
---@return integer? size 数据长度
function srey.wait_handshaked(fd, skid)
    if INVALID_SOCK == fd then
        return false
    end
    local msg = _coro_wait(false, skid, MSG_TYPE.HANDSHAKED, srey.get_netread_timeout())
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid, 1)
        WARN("handshake timeout, skid %s.", tostring(skid))
        return false
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        WARN("handshake connction closed, skid %s.", tostring(skid))
        return false
    end
    return ERR_OK == msg.erro, msg.data, msg.size
end

---@param msg Message
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
            _coro_resume(coro, msg)
        else
            if func then
                _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.erro, msg.data, msg.size)
            end
        end
    end
end

---注册数据接收回调；仅在没有协程通过 syn_send 等待该 socket 时触发
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, client:integer, slice:integer, data:lightuserdata?, size:integer) RECV 回调
function srey.on_recved(func)
    func_cbs[MSG_TYPE.RECV] = func
end

---异步发送数据（不等待响应）
---发送数据（参数详见 core.send）
---@type fun(fd:integer, skid:integer, data:string|lightuserdata, size:integer?, copy:integer):boolean
srey.send = core.send

---多播发送：把同一份 data 零拷贝广播给多个 fd；C 层 shared_data 引用计数自动释放
---@param fds integer[] socket fd 数组
---@param skids integer[] 连接 skid 数组，与 fds 一一配对
---@param data string|lightuserdata 数据
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据,默认 1
---@return boolean ok 至少 1 个 fd 投递成功 true
function srey.send_multi(fds, skids, data, size, copy)
    -- 所有不进入 core.send_multi 的退出路径都先 utils.ud_free 兜底,
    -- 避免 C 端 luaL_error longjmp / 空表 return 漏 FREE 调用方已转移的 data
    if #fds ~= #skids then
        if 0 == copy then
            utils.ud_free(data)
        end
        error(string.format("send_multi fds and skids length mismatch (%d vs %d)", #fds, #skids))
    end
    if 0 == #fds then
        if 0 == copy then
            utils.ud_free(data)
        end
        return false
    end
    return core.send_multi(fds, skids, data, size, copy)
end

---内部辅助：挂起协程等待该 socket 的下一个 RECV 消息（含超时/断开处理）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@return Message|nil msg 收到的消息表；超时/断开返回 nil
local function _wait_net_recv(fd, skid)
    local msg = _coro_wait(false, skid, MSG_TYPE.RECV, srey.get_netread_timeout())
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid, 1)
        WARN("send timeout, skid %s.", tostring(skid))
        return nil
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        WARN("connction closed, skid %s.", tostring(skid))
        return nil
    end
    return msg
end

---同步发送并等待响应：发送后挂起协程，收到回包后返回数据；适用于请求-响应模式
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param data string|lightuserdata 数据
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer 1=复制；0=零拷贝
---@return lightuserdata|nil rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝；失败/超时返回 nil
---@return integer? rsize 响应数据长度
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

---同步接收下一个数据分片（不发送）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@return boolean ok 接收成功 true
---@return boolean? fin 当前分片是否为最后一片（仅 ok=true 时）
---@return lightuserdata? data 分片数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝
---@return integer? size 分片字节数
function srey.syn_slice(fd, skid)
    if INVALID_SOCK == fd then
        return false
    end
    local msg = _wait_net_recv(fd, skid)
    if not msg then
        return false
    end
    return true, SLICE_TYPE.END == msg.slice, msg.data, msg.size
end

---通过 harbor 协议向目标 task 发起单向 call（HTTP 封装，不等待返回数据）
---@param fd integer harbor 连接 fd（pktype 必须为 HTTP）
---@param skid integer 连接 skid
---@param dst TASK_NAME 目标 task name
---@param reqtype REQUEST_TYPE 业务请求类型
---@param key string 路由 key（一致性哈希定位节点）
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@return boolean ok 远端返回 200 OK 时 true
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

---通过 harbor 协议向目标 task 发起同步请求，等待返回数据
---@param fd integer harbor 连接 fd（pktype 必须为 HTTP）
---@param skid integer 连接 skid
---@param dst TASK_NAME 目标 task name
---@param reqtype REQUEST_TYPE 业务请求类型
---@param key string 路由 key
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@return lightuserdata|nil rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝；失败或非 200 返回 nil
---@return integer? rsize 响应数据长度
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

---@param msg Message
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
        _coro_resume(coro, msg)
    else
        if func then
            _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.slice, msg.data, msg.size)
        end
    end
end

---注册数据发送完成回调；仅在 NET_EV.SEND 标志启用时触发，可用于流控或写缓冲监控
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, client:integer, size:integer) SEND 回调
function srey.on_sended(func)
    func_cbs[MSG_TYPE.SEND] = func
end

---@param msg Message
local function _net_sended_dispatch(msg)
    local func = func_cbs[MSG_TYPE.SEND]
    if func then
        _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client, msg.size)
    end
end

---注册连接关闭回调；CLOSE 消息会先唤醒所有在该 skid 上挂起等待的协程，再调用此回调
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, client:integer) CLOSE 回调
function srey.on_closed(func)
    func_cbs[MSG_TYPE.CLOSE] = func
end

---主动关闭 TCP 连接（发送 FIN）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param immed? integer 0=优雅关闭(等 send queue 发完);1=立即关闭(丢弃未发数据);默认 0
function srey.close(fd, skid, immed)
    core.close(fd, skid, immed or 0)
end

---同步关闭：发起关闭后挂起协程等 CLOSE，保证协议层 close 回调（含 ctx->fd 复位）已执行；
---重连前用，避免旧连接异步 teardown 与新连接 try_connect 共享同一 ctx 时清掉新 fd
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param immed? integer 0=优雅(默认,buf 空时退化为立即);1=立即
function srey.sync_close(fd, skid, immed)
    if INVALID_SOCK == fd then
        return
    end
    core.close(fd, skid, immed or 0)
    _coro_wait(false, skid, MSG_TYPE.CLOSE, srey.get_netread_timeout())
end

---@param msg Message
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
            _coro_resume(coro, msg)
            if corosess.disposable then
                break
            end
        end
    end
    if func then
        _coro_run(_coro_cb, func, msg.pktype, msg.fd, msg.skid, msg.client)
    end
end

---注册 UDP 数据接收回调
---@param func fun(fd:integer, skid:integer, ip:string, port:integer, data:lightuserdata?, size:integer) RECVFROM 回调
function srey.on_recvedfrom(func)
    func_cbs[MSG_TYPE.RECVFROM] = func
end

---创建 UDP socket 并绑定到 ip:port
---@param ip string? 绑定 IP，默认 "0.0.0.0"
---@param port integer? 绑定端口，默认 0（由 OS 分配）
---@return integer fd socket fd
---@return integer skid 连接 skid
function srey.udp(ip, port)
    if not ip then
        ip = "0.0.0.0"
    end
    if not port then
        port = 0
    end
    return core.udp(ip, port)
end

---UDP socket 加入多播组(IPv4/IPv6 自动按 socket family 分支)
---@type fun(fd:integer, skid:integer, group_ip:string, iface_str:string?):boolean
srey.udp_join = core.udp_join

---UDP socket 离开多播组,参数同 udp_join
---@type fun(fd:integer, skid:integer, group_ip:string, iface_str:string?):boolean
srey.udp_leave = core.udp_leave

---设置 UDP 多播 TTL(IPv4)/Hop Limit(IPv6)。默认 1 仅本网段,32 跨网段,255 跨广域
---@type fun(fd:integer, skid:integer, ttl:integer):boolean
srey.udp_ttl = core.udp_ttl

---设置 UDP 多播本机回环。默认 1(发出去自己也能收到),0=不收
---@type fun(fd:integer, skid:integer, enable:integer):boolean
srey.udp_loop = core.udp_loop

---异步 UDP 发送（参数详见 core.sendto）
---@type fun(fd:integer, skid:integer, ip:string, port:integer, data:string|lightuserdata, size:integer?, copy:integer):boolean
srey.sendto = core.sendto

---同步 UDP 发送并等待响应：设置会话键 → sendto → 挂起协程等 RECVFROM
---@param fd integer UDP socket fd
---@param skid integer 连接 skid
---@param ip string 目标 IP
---@param port integer 目标端口
---@param data string|lightuserdata 数据
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer 1=复制；0=零拷贝
---@return lightuserdata|nil rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝；超时/失败返回 nil
---@return integer|nil rsize 响应数据长度
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
    -- 成功路径不调 sock_session(0)：C 端 _net_recvfrom 接收时已 ud->sess = 0，
    -- 错误/超时路径无 RECVFROM 事件，需 lua 手动清以免 ud->sess 残留影响下一包路由
    return msg.udata, msg.size
end

---@param msg Message
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
        _coro_resume(coro, msg)
    else
        if func then
            _coro_run(_coro_cb, func, msg.fd, msg.skid, msg.ip, msg.port, msg.udata, msg.size)
        end
    end
end

---定时扫描所有挂起的协程，将已超时者强制 resume（携带 TIMEOUT 消息）；每 1 秒触发一次，
---通过 srey.timeout 自我调度形成循环；扫描主体用 xpcall 包裹保证循环不被异常中断
local function _coro_timeout()
    srey.xpcall(function()
        if nyield > 0 then
            local now = srey.timer_ms()
            local cnt = 0
            local _timeout_buf = {}
            for sess, corosess in pairs(coro_sess) do
                if corosess.disposable then
                    local coroinfo = corosess.coroinfo
                    if coroinfo.timeout > 0 and now >= coroinfo.timeout then
                        cnt = cnt + 1
                        _timeout_buf[cnt] = sess
                    end
                else
                    for i = 1, #corosess.coroinfo do
                        if corosess.coroinfo[i].timeout > 0 and now >= corosess.coroinfo[i].timeout then
                            cnt = cnt + 1
                            _timeout_buf[cnt] = sess
                            break
                        end
                    end
                end
            end
            local cur_corosess
            local cur_coroinfo
            local cur_sess
            local cur_coro
            for i = 1, cnt do
                cur_sess = _timeout_buf[i]
                _timeout_buf[i] = nil
                cur_corosess = coro_sess[cur_sess]
                if not cur_corosess then
                    goto continue
                end
                local msg = {mtype = MSG_TYPE.TIMEOUT, sess = cur_sess}
                if cur_corosess.disposable then
                    coro_sess[cur_sess] = nil
                    cur_coroinfo = _coro_info(cur_corosess)
                    if cur_coroinfo then
                        cur_coro = cur_coroinfo.coro
                        _coro_resume(cur_coro, msg)
                        WARN("resume timeout session %s.", tostring(cur_sess))
                    end
                else
                    local j = 1
                    while j <= #cur_corosess.coroinfo do
                        cur_coroinfo = cur_corosess.coroinfo[j]
                        if cur_coroinfo.timeout > 0 and now >= cur_coroinfo.timeout then
                            tremove(cur_corosess.coroinfo, j)
                            cur_coro = cur_coroinfo.coro
                            _coro_resume(cur_coro, msg)
                            WARN("resume timeout session %s.", tostring(cur_sess))
                        else
                            j = j + 1
                        end
                    end
                    if #cur_corosess.coroinfo == 0 then
                        coro_sess[cur_sess] = nil
                    end
                end
                ::continue::
            end
        end
    end)
    _coro_pool_shrink()
    srey.timeout(1 * 1000, _coro_timeout)
end
---@class Message
---@field mtype  MSG_TYPE             消息类型（MSG_TYPE.*），始终存在
---@field sess   integer?             会话 id；TIMEOUT/REQUEST/RESPONSE/RECV 携带
---@field fd     integer?             socket fd；网络消息携带
---@field skid   integer?             连接 skid；网络消息携带
---@field pktype PACK_TYPE?           封包协议类型；RECV/CONNECT/REQUEST 携带
---@field erro   integer?             错误码；CONNECT/SEND/RESPONSE/SSLEXCHANGED 携带
---@field client string?              客户端地址；ACCEPT/SSLEXCHANGED/RECV 携带
---@field data   lightuserdata?       数据指针；RECV/REQUEST/RESPONSE 携带
---@field size   integer?             数据字节数；与 data 同步出现
---@field slice  integer?             分片类型（SLICE_TYPE.*）；RECV 携带
---@field src    integer?            请求方 task 数字句柄；REQUEST 携带
---@field ip     string?              UDP 源 IP；UDP_RECV 携带
---@field port   integer?             UDP 源端口；UDP_RECV 携带
---@field udata  lightuserdata?       UDP 数据指针；UDP_RECV 携带

---全局消息分发入口，由 C 层 loader 在每条消息到达时调用；按 msg.mtype 路由到对应内部分发函数
---@param msg Message 由 C 层 _ltask_pack_msg 打包的消息表
function message_dispatch(msg)
    if MSG_TYPE.STARTUP == msg.mtype then
        srey.timeout(1 * 1000, _coro_timeout)
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
    _drain_fork_queue()    -- srey.fork 在此起新协程
end

return srey
