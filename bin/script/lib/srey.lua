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
local select           = select
local tunpack          = table.unpack
local tremove          = table.remove
local tpack            = table.pack
local tinsert          = table.insert
local coroutine_create = coroutine.create
local coroutine_yield  = coroutine.yield
local coroutine_resume = coroutine.resume
-- 判"是否在框架协程里"一律比 coroutine.running() 与 coro_running，不用 coroutine.isyieldable()：
-- 业务在框架协程内再套一个裸 coroutine.create/wrap 时 isyieldable 照样为真，而 coro_running
-- 仍指着外层那个——据此把"自己"登记进等待队列，登记的就是外层协程，醒来 resume 的也是它
local coroutine_running = coroutine.running
local message_may_keep = core.message_may_keep
local may_resume       = core.may_resume
-- 只提 C 绑定的直通别名:hotfix 不支持 C function,热更新碰不到它们。
-- srey.xpcall 是 Lua 函数、能被热修,绝不能提
local task_incref      = task.incref
local task_ungrab      = task.ungrab
local cur_task  = _curtask -- 当前 task 的 C 层指针，由 loader 注入
local TASK_NAME = TASK_NAME
local SSL_NAME  = SSL_NAME
local REQUEST_TYPE = REQUEST_TYPE
local CORO_POOL_MAX      = 128 -- 协程池上限；超出后空闲协程自然退出由 GC 回收
local CORO_POOL_MIN_KEEP = 4 -- 协程池收缩底线
local CORO_SHRINK_TICKS  = 10 -- 收缩门控：_coro_timeout 每 1s 触发，累计 10 次(≈SHRINK_TIME 10s)收缩一次，与 C 侧统一
local coro_running   = nil -- 当前正在执行的协程（用于 _set_coro_sess 记录 coro）
local coro_sess      = {} -- 会话表：sess/skid → corosess，保存挂起协程的等待信息
local func_cbs       = {} -- 消息类型 → 用户注册回调函数
local srey  = {}
local nyield = 0 -- 当前挂起等待的协程总数（closing 时用于告警）
-- 上面那些里"登记进了 coro_sess、因而可能被超时扫描找到"的那部分。两个计数不能合并：
-- nyield 还含 fork_wait 与 serial 排队的协程，它们裸 yield、不进 coro_sess，
-- 拿 nyield 当扫描门槛的话，一个协程在 serial 队列里趴多久，就白扫多少秒整张 coro_sess。
-- C 侧只有 nyield 一个计数不是漏改：coro.c 的 _coro_timeout_monitor 门禁判的是
-- timeout_heap.root，不入堆的挂起协程堆空时直接短路，且扫描体堆顶未到期即 break
local nwait = 0
-- 下一个到期时刻(ms)。0 = 未知必须全扫，math.huge = 没有带超时的等待者。
-- 只在插入时取 min、在全扫后重算：条目被摘除而不重算只会让它偏小，后果是多白扫一次
-- 然后自我修正，永远不会偏大，故不会漏唤醒
local next_timeout = 0
local coro_pool       = {}-- 空闲协程池
local coro_pool_trend = trend.new()-- 协程池负载趋势
local fork_queue      = {}-- srey.fork 待执行任务队列：{ func, args }，在 message_dispatch 末尾批量起协程
local serial_wakes    = {}-- srey.serial 待唤醒的队头等待者，同样在 message_dispatch 末尾摊平唤醒
---@class ForkBarrier
---@field results ForkResult[]  与 funcs 同序的结果数组，子协程各自回填
---@field pending integer       未完成的子协程数；归零时唤醒 waiter
---@field waiter  thread        等待归零的父协程
---@field since   integer       挂起起始时刻（毫秒，timer_ms 单位）；coros 据此报"等了多久"

---@class SerialWaiter
---@field coro  thread     排队中的协程
---@field since integer    入队时刻（毫秒，timer_ms 单位）

---@class SerialState
---@field current thread?        当前持锁协程；nil 表示无锁
---@field ref     integer        嵌套深度（同协程多次进入累加）
---@field since   integer?       current 转为非 nil 的时刻（毫秒，timer_ms 单位）；coros 据此报"持锁多久"
---@field waiters SerialWaiter[] 等待协程 FIFO 队列

-- fork_wait 屏障与 serial 执行器的活跃登记，仅供 debug_request 的 coros 遍历：这两类等待者
-- 走裸 yield、不进 coro_sess，不登记就只剩 nyield 一个数字，看不出是谁卡在哪。
-- 对齐 C 侧 coro_ctx.fork_waited / coro_ctx.serials。
-- 屏障的加与删都在 fork_wait 一个函数内，确定成对，用普通表；
-- 执行器没有显式销毁点，必须弱键，否则每连接一个执行器连同它的 waiters 永久留在表里
local fork_barriers   = {}
local serial_execs    = setmetatable({}, { __mode = "k" })

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
srey.MSG_TYPE = MSG_TYPE
local MTYPE_RECV = MSG_TYPE.RECV-- 每包分发都要取,单独提一份
-- 数据分片标志（对应 C 层 slice_type）
local SLICE_TYPE = {
    START = 0x01,   -- 分片开始
    SLICE = 0x02,   -- 中间分片
    END   = 0x04,   -- 最后一片（完整消息）
}
srey.SLICE_TYPE = SLICE_TYPE
-- CLOSE 消息 erro 的取值（对应 C 层 close_type），连接是怎么断的
---@enum CLOSE_TYPE
local CLOSE_TYPE = {
    ORDERLY   = 0,  -- 对端有序结束发送方向：裸 TCP 收到 FIN，SSL 收到 close_notify
    LOCAL     = 1,  -- 本地主动：close / task 拆除 / 发队列溢出 / 解析错误
    ABORT     = 2,  -- 异常中断：RST、读写错误、SSL 协议错
    NEVERCONN = 3,  -- 连接/会话从未建立，本消息只为唤醒等待方，不触发 on_closed
    TRUNCATED = 4   -- TLS 没发 close_notify 就断了：字节可能已收全，也可能被截断，框架分不出
}
srey.CLOSE_TYPE = CLOSE_TYPE

-- 早退路径统一兜底：copy=0 时调用方已转移 data 所有权,需主动 utils.ud_free 释放
-- （utils.ud_free 内部仅对 lightuserdata 生效,非 lightuserdata 自动跳过）
local function _ud_free_copy(data, copy)
    if 0 == copy then
        utils.ud_free(data)
    end
end
---早退路径释放 data：copy=0 表示调用方已转移所有权，须由被调方释放（仅对 lightuserdata 生效）
---@param data string|lightuserdata|nil 待释放数据
---@param copy integer? 1 或缺省=已复制，不释放；0=所有权已转移，须释放
srey._ud_free_copy = _ud_free_copy

-- xpcall 的错误处理器：打印错误与调用栈后把 err 原样交回，供 xpcall 当第二个返回值
local function _xpcall_error(err)
    ERROR("%s\n%s.", err, debug.traceback())
    return err
end

---带错误捕获的函数调用；异常时自动打印错误信息和调用栈。
---第一个返回值恒为 ok，func 自己的返回值从第二个起——写成 local ok, ret = srey.xpcall(f)
---会把第二个之后的悄悄丢掉
---@param func fun(...):any 待调用函数
---@param ... any 函数参数
---@return boolean ok 是否成功
---@return any ... func 的返回值（成功）或错误信息（失败）
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

-- 新建协程；协程体捕获 func/coro 两个 upvalue
local function _coro_new(func)
    local coro
    coro = coroutine_create(
        function(...)
            func(...)           -- 执行首次传入的任务
            while true do
                func = nil -- 释放上一个任务的引用
                if #coro_pool >= CORO_POOL_MAX then
                    break       -- 池满，协程退出，交 GC 回收
                end
                coro_pool[#coro_pool + 1] = coro-- 归还到池
                func = coroutine_yield()-- 等待下一个任务函数
                if not func then-- nil 守卫：池收缩时用 resume(coro, nil) 让它退出
                    break
                end
                if "function" ~= type(func) then
                    -- 池里的协程被当成某个 coro_sess 等待者 resume 了：登记了却没被摘掉的陈旧条目
                    ERROR("coroutine pool: resumed with %s, expected function.", type(func))
                    break
                end
                func(coroutine_yield())            -- 等待实参后执行
            end
        end)
    return coro
end
---从协程池取一个空闲协程绑定新任务，池为空时新建；执行完毕后协程归还池中复用，
---池满（>= CORO_POOL_MAX）时协程退出由 GC 回收，避免池无界增长
---@param func fun(...) 任务函数
---@return thread coro 协程对象
local function _coro_create(func)
    local coro = tremove(coro_pool)
    if not coro then
        return _coro_new(func)
    end
    -- 池命中：将新 func 注入正在 yield 处等待的协程
    local ok, err = coroutine_resume(coro, func)
    if ok then
        return coro
    end
    -- 注入失败说明池里这个协程已经死了（来源见协程体里那两条 break）。
    -- 不能把它返回出去——调用方还会 resume 一次、再失败一次，这条消息就被静默丢掉
    ERROR("coroutine error: %s", tostring(err))
    return _coro_new(func)
end

local coro_shrink_tick = 0 -- _coro_pool_shrink 距上次实际收缩的累计触发次数（每次 = 一轮 _coro_timeout = 1s）
local function _coro_pool_shrink()
    coro_shrink_tick = coro_shrink_tick + 1
    if coro_shrink_tick < CORO_SHRINK_TICKS then
        return
    end
    coro_shrink_tick = 0
    local cur = #coro_pool
    if coro_pool_trend:busy(cur) then
        return
    end
    if cur <= CORO_POOL_MIN_KEEP then
        return
    end
    local keep = cur - (cur // 5)
    if keep < CORO_POOL_MIN_KEEP then
        keep = CORO_POOL_MIN_KEEP
    end
    while #coro_pool > keep do
        local coro = tremove(coro_pool)
        -- resume(coro, nil) 触发 _coro_new 协程体内 `if not func then break` 守卫，
        -- 让协程主体主动退出释放栈帧/upvalue，避免仅靠 GC 延迟回收
        coroutine_resume(coro, nil)
    end
end

---恢复协程执行；同时更新 coro_running 以便 _set_coro_sess 能记录当前协程；
---协程内部 panic 时捕获错误并打印，不向上层抛出。
---所有唤醒协程的入口必须走此函数，禁止裸调 coroutine.resume：否则 coro_running 不同步，
---会把已归还池的旧 coro 登记进 coro_sess，后续消息按错协程 resume。
---resume 前后由本函数自己存取上一个 coro_running 并还原（顶层调用时原值就是 nil），
---调用方不必各写一份；口径同 C 侧 _coro_resume_switch。
---@param coro thread 协程对象
---@param ... any 传给协程的参数
local function _coro_resume(coro, ...)
    local prev = coro_running
    coro_running = coro
    local ok, err = coroutine_resume(coro_running, ...)
    coro_running = prev
    if not ok then
        ERROR("coroutine error: %s", tostring(err))
    end
end

---从池中取协程（或新建）并立即 resume 执行 func(...)
---@param func fun(...) 协程任务函数
---@param ... any 传给 func 的参数
local function _coro_run(func, ...)
    _coro_resume(_coro_create(func), ...)
end

---协程包装器：执行前 incref 防止 task 被提前销毁，执行后 ungrab；并持有 msg 至回调返回——
---使 msg 的 __gc(释放 msg.data)推迟到回调结束，避免"首次 yield 前未消费 data → dispatch 返回后
---msg 被 GC → 恢复时解引用悬空"(与 C 侧 _message_clean 延后对齐)。data 型消息传其 msg；
---无 data 的调用点传 nil(无实参时可省略，默认 nil)。
---@param func fun(...) 业务回调
---@param msg Message|nil 仅保活，不传给 func
---@param ... any 传给 func 的实参
local function _coro_cb(func, msg, ...)
    task_incref(cur_task)
    srey.xpcall(func, ...)
    task_ungrab(cur_task)
    return msg -- 引用至函数尾，保证 msg 栈槽在 func yield 期间被 GC 标记存活
end

---将函数 f 与参数预绑定，返回一个无参 lambda，调用即 f(args)。
---用于 srey.fork / srey.fork_wait 等接收 () -> any 的 API，减少 function() return ... end 样板。
---@param f fun(...):any 待绑定函数
---@param ... any 预绑定的参数（用闭包捕获，每次调用 lambda 都传给 f）
---@return fun(): any wrapped 无参 lambda
function srey.fork_bind(f, ...)
    local args = tpack(...)
    return function() return f(tunpack(args, 1, args.n)) end
end

---立即在新协程中执行 func(...)（fire-and-forget）；当前协程不让出；
---新协程在本条消息 dispatch 完成后由 message_dispatch 末尾的 _drain 起，不走时间轮；
---错误由 _coro_cb 的 xpcall 捕获并打 ERROR 日志（不会传播到主协程）。
---@param func fun(...) 协程任务函数
---@param ... any 传给 func 的参数
function srey.fork(func, ...)
    fork_queue[#fork_queue + 1] = { func = func, args = tpack(...) }
end

---fork_wait 的单项结果。val 只是 [1] 的别名，多返回值的任务要从 [1..n] 取
---@class ForkResult
---@field ok  boolean  f 正常返回为 true；抛错为 false，此时 [1] 是错误信息字符串
---@field val any      f 的第一个返回值，等同 [1]
---@field n   integer  f 的返回值个数；中间可能有 nil 洞，不能用 # 代替
---收拢 xpcall 的返回值：ok 之后的全部值都留住。写成 local ok, ret = srey.xpcall(f) 会把第二个
---之后的悄悄丢掉，而 mongo 的命令普遍返 (ok, n) 双值（同 srey.serial 的 _done）
---@return ForkResult
local function _fork_result(ok, ...)
    return { ok = ok, val = ..., n = select("#", ...), ... }
end

---并发执行 funcs 中所有无参函数，等全部完成（或抛错）后返回每个任务的状态与结果。
---结果数组与 funcs 同序，每项形如 { ok=true, [1..n]=<返回值> } 或 { ok=false, [1]=<错误信息字符串> }。
---调用方必须身处协程（startup/timeout/on_* 回调内部均满足）。
---@param funcs (fun(): any...)[]  并发任务列表（每个为无参 lambda，参数用闭包捕获）
---@return ForkResult[] results 与 funcs 同序的状态结果数组
function srey.fork_wait(funcs)
    local n = #funcs
    if 0 == n then
        return {}
    end
    -- 协程守卫排在空表早退之后：C 侧 coro_fork_wait 的 n<=0 同样先返回，不看 curco
    if coroutine_running() ~= coro_running then
        error("srey.fork_wait must be called from within a srey coroutine", 2)
    end
    ---@type ForkBarrier
    local barrier = {
        results = {},
        pending = n,
        waiter  = coro_running,
        since   = srey.timer_ms(),
    }
    fork_barriers[barrier] = true
    for i = 1, n do
        local f = funcs[i]
        srey.fork(function()
            local res = _fork_result(srey.xpcall(f))
            barrier.results[i] = res
            barrier.pending = barrier.pending - 1
            if 0 == barrier.pending then
                _coro_resume(barrier.waiter)
            end
        end)
    end
    -- 与 C 侧 coro_fork_wait 口径对齐：挂起期间计入 nyield,否则 task 关闭时只剩 fork_wait
    -- 挂起的父协程,_closing_dispatch 会静默通过、少一条"还有协程没退"的告警
    nyield = nyield + 1
    coroutine_yield()
    nyield = nyield - 1
    fork_barriers[barrier] = nil
    return barrier.results
end

---创建一个协程串行化执行器。同 task 内多协程对同一资源并发访问时串行进入，避免穿插；
---同一协程嵌套调用安全（ref 计数）；f 抛错由 srey.xpcall 捕获并打 ERROR 日志，锁照常释放，
---下个等待者继续。
---调用返回时锁已交接给下一个等待者，但该等待者要到本条消息 dispatch 末尾才起跑
---（与 C 侧 coro_serial_call 就地唤醒不同，原因见 _release 内注释）。
---@return fun(f:fun(...):any, ...):boolean,... serial 串行化调用器；返回 ok 加上 f 的全部返回值
function srey.serial()
    -- 状态放表而不是闭包局部：debug_request 的 coros 要遍历 waiters,闭包局部它读不到
    ---@type SerialState
    local st = { current = nil, ref = 0, waiters = {} }
    serial_execs[st] = true
    local function _release()
        st.ref = st.ref - 1
        -- 负数说明有人没持锁就 leave。放着不管 ref 再也回不到 0，这个 serial 既不交接
        -- 也不释放，排队者全部永久挂起；C 镜像 coro_serial_leave 同处置
        assert(st.ref >= 0, "serial leave without a matching enter")
        if 0 == st.ref then
            local nxt = tremove(st.waiters, 1)
            if nxt then
                -- 先设 current/ref 完成交接，nxt 醒来时拿到一致状态；此刻起其他协程进来一律排队
                st.current = nxt.coro
                st.ref = 1
                st.since = srey.timer_ms()
                -- 只入队不就地 resume：Lua 的 resume 嵌在同一条 C 栈上，链式唤醒会撞
                -- LUAI_MAXCCALLS 而整个 serial 死锁。改由 message_dispatch 末尾摊平，
                -- 嵌套深度恒为 1；C 侧 minicoro 切栈没有这个上限，就地 resume 是对的
                serial_wakes[#serial_wakes + 1] = nxt.coro
            else
                st.current = nil
            end
        end
    end
    -- 先解锁再原样吐出 f 的全部返回值。不用 table.pack 中转是因为变参走的是栈,
    -- 每条命令省一个临时表；写成 local ok, ret = ... 则会把第二个之后的返回值悄悄丢掉,
    -- 而 mongo 的命令普遍返 (ok, n) 双值
    local function _done(...)
        _release()
        return ...
    end
    return function(f, ...)
        if coroutine_running() ~= coro_running then
            error("srey.serial executor must be called from within a srey coroutine", 2)
        end
        local self = coro_running
        if st.current and st.current ~= self then
            st.waiters[#st.waiters + 1] = { coro = self, since = srey.timer_ms() }
            -- 与 fork_wait 同口径计入 nyield:排在 waiters 里的协程同样是"挂起没退"的,
            -- 不计的话 task 关闭时 _closing_dispatch 看到 nyield==0 就静默通过,
            -- 操作者拿不到"还有协程卡在临界区队列上"这条线索
            nyield = nyield + 1
            coroutine_yield()
            nyield = nyield - 1
            -- 被唤醒时 current=self, ref=1 已由 _release 设置
        else
            if not st.current then
                st.current = self
                st.since = srey.timer_ms()
            end
            st.ref = st.ref + 1
        end
        return _done(srey.xpcall(f, ...))
    end
end

---收敛 serial 执行器的返回值：执行器返回 (ok, f 的全部返回值)，ok=false 表示 f 内抛了错
---（已由 srey.xpcall 打过 ERROR 日志）。把那种情形折成调用方约定的失败值，其余原样透传。
---用法 `return srey.serial_ret(nil, self.serial(self._query, self, sql))`——
---失败值各模块不同（有的 nil 有的 false），故由调用方传入而不是写死
---@param fail any f 抛错时代替返回的值
---@param ok boolean 执行器的第一个返回值
---@return any ... ok 为真时是 f 的全部返回值，否则是 fail
function srey.serial_ret(fail, ok, ...)
    if not ok then
        return fail
    end
    return ...
end

---排空一条延迟队列：头索引推进而不清 nil（清了 #qu 会出 hole），末尾一次性清空，
---O(N) 且不走 tremove 的 memmove。
---处理 item 期间往同一队列尾追加是允许的（fork 里再 fork、被唤醒者出 cs 时再唤醒下一个），
---本循环会继续消费到空，所以嵌套深度恒为 1。
---act 必须包 xpcall：调用点（message_dispatch 末尾）不在任何保护里，抛出会连末尾那趟清空
---一起跳过，已消费的元素被下一条消息再跑一遍
---@param qu any[] 待排空的队列
---@param act fun(item:any) 对每个元素执行的动作
local function _drain(qu, act)
    if 0 == #qu then
        return
    end
    local i = 1
    while i <= #qu do
        local item = qu[i]
        i = i + 1
        srey.xpcall(act, item)
    end
    for j = 1, i - 1 do
        qu[j] = nil
    end
end
srey._drain = _drain -- 仅供单元测试直接驱动，业务勿用

---srey.fork 的排队项：待执行函数与它的参数（table.pack 形态，n 为参数个数，含 nil 洞）
---@class ForkItem
---@field func fun(...):any
---@field args { n: integer, [integer]: any }  table.pack 结果：n 为参数个数，[1..n] 为参数值
---起一个 srey.fork 排队的新协程
---@param item ForkItem
local function _run_fork(item)
    _coro_run(_coro_cb, item.func, nil, tunpack(item.args, 1, item.args.n))
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

---将数据转为十六进制字符串（调试用）。data 为字符串时**不得**传 size——lower 紧跟在 data 之后，
---多传一个就占掉 lower 的位置并报 boolean expected
---@type fun(data:string|lightuserdata, size:integer?, lower:boolean?):string
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

---查询 task 类型
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

---设置跨 task request/response 等待超时；ms 须 大于 0（非法值告警并忽略），超上界 clamp
---@type fun(ms:integer)
srey.set_request_timeout = task.set_request_timeout

---获取跨 task request/response 等待超时
---@type fun():integer
srey.get_request_timeout = task.get_request_timeout

---设置 TCP/TLS 连接建立超时；ms 须 大于 0（非法值告警并忽略），超上界 clamp
---@type fun(ms:integer)
srey.set_connect_timeout = task.set_connect_timeout

---获取 TCP/TLS 连接建立超时
---@type fun():integer
srey.get_connect_timeout = task.get_connect_timeout

---设置网络读（recv/handshake/ssl exchange）超时；ms 须 大于 0（非法值告警并忽略），超上界 clamp
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
---@field func    fun(...)?    定时回调；非 nil 时由新协程执行而非 resume coro
---@field args    any[]?       传给 func 的参数列表；func 为 nil 时不存在

---@class CoroSession
---@field keep    boolean     waiters 摘空后是否保留本条目：false 立即删除；true 保留（TCP/UDP 同一
---                            skid 高频复用，免去反复 table 删除+插入），仅 _net_close_dispatch 会强制清 false
---@field waiters CoroInfo[]  挂起协程数组，严格按 FIFO 顺序等待/唤醒（[1] 为队头，队头 mtype 匹配才摘除）

---将协程或回调函数注册到会话表，等待指定消息类型唤醒；keep 仅在新建条目时按 mtype 由 message_may_keep 推导，
---追加到已有条目不覆盖旧值——keep 描述的是 sess 本身的性质，一旦被 _net_close_dispatch 清 false 就应保持 false
---@param coro thread? 等待唤醒的协程；func 模式下可为 nil
---@param sess integer 会话 id
---@param mtype integer 期望唤醒的消息类型（MSG_TYPE.*）
---@param ms integer 超时毫秒数；0 表示永不超时
---@param func fun(...)? 定时回调；非 nil 时消息到达后新建协程执行而非 resume coro
---@param ... any 传给 func 的参数
local function _set_coro_sess(coro, sess, mtype, ms, func, ...)
    local timeout = 0
    local now = srey.timer_ms()
    if ms > 0 then
        timeout = now + ms
        if timeout < next_timeout then
            next_timeout = timeout
        end
    end
    local coroinfo = {
        timeout = timeout,
        since   = now,
        coro    = coro,
        mtype   = mtype,
        func    = func,
        args    = func and tpack(...) or nil,
    }
    local corosess = coro_sess[sess]
    if not corosess then
        coro_sess[sess] = {
            keep = message_may_keep(mtype),
            waiters = {coroinfo}
        }
    else
        tinsert(corosess.waiters, coroinfo)
    end
end

---从会话表中查找匹配的队头 coroinfo 并摘除：仅检测队头，mtype 匹配才摘除返回，
---队头不匹配（含 keep 保留的空条目）视为无等待者，不越过队头继续查找（保持严格 FIFO）；
---摘除后 waiters 为空且 !keep 时才删除会话表条目
---@param sess integer 会话 id
---@param mtype integer 消息类型
---@return CoroInfo|nil coroinfo 匹配的队头协程信息；不匹配返回 nil
local function _get_coro_sess(sess, mtype)
    local corosess = coro_sess[sess]
    if not corosess or 0 == #corosess.waiters then
        return nil
    end
    local coroinfo = corosess.waiters[1]
    if mtype ~= coroinfo.mtype then
        return nil
    end
    tremove(corosess.waiters, 1)
    if 0 == #corosess.waiters and not corosess.keep then
        coro_sess[sess] = nil
    end
    return coroinfo
end

---删除 sess 的空会话表条目:仅无挂起等待者时删,避免误删他协程正在该 sess 等待的会话。
---只由 _net_close_dispatch 在 CLOSE 到达后调用——kcp 等以用户 sess(非 skid)注册的 keep=true 条目,
---无论会话是否建立成功都有 CLOSE 可清(未建立时由 _kcp_start 补发 NEVERCONN 的合成 CLOSE),
---故无需再向业务侧导出手动清理入口
---@param sess integer 会话 id
local function _coro_sess_del_empty(sess)
    local cs = coro_sess[sess]
    if cs and 0 == #cs.waiters then
        coro_sess[sess] = nil
    end
end

---找到匹配等待者就唤醒它。与 _dispatch_cb 分成两个而不是一个带变参的尾部，是因为回调实参
---（msg.subtype / msg.fd / … 那一串）在调用点就地求值：合成一个函数的话，命中等待者时那 4~7 次
---字符串键查表也照算一遍再当变参丢掉，而命中是客户端类 task 每个包的常态
---@param msg Message
---@param mtype integer 期望匹配的消息类型
---@return boolean resumed 已唤醒等待者返回 true，调用方无需再走 _dispatch_cb
local function _resume_waiter(msg, mtype)
    local coroinfo = _get_coro_sess(msg.sess, mtype)
    if not coroinfo then
        return false
    end
    _coro_resume(coroinfo.coro, msg)
    return true
end
---没有协程等待时的兜底：注册了业务回调就起新协程跑它。
---sess==0 不必由调用方另判：sess 只可能来自 srey.id() 或 skid，两者都出自 C 的 createid，
---而 createid 的低位计数从 1 起、恒非 0，coro_sess[0] 永远不存在，必然落到这里
---@param msg Message
---@param func fun(...)? 业务回调；nil 表示未注册
local function _dispatch_cb(msg, func, ...)
    if func then
        _coro_run(_coro_cb, func, msg, ...)
    end
end

---挂起当前协程，等待指定会话的消息。
---不在框架协程里调用直接抛出：拿错的 coro_running 去登记 coro_sess，会让 nyield/nwait 永不归零，
---那个 sess 的消息到了还按错协程 resume。
---@param sess integer 会话 id
---@param mtype integer 期望唤醒的消息类型
---@param ms integer 超时毫秒数；0 表示永不超时
---@return Message msg 触发 resume 的消息表
function srey._coro_wait(sess, mtype, ms)
    if coroutine_running() ~= coro_running then
        error("srey._coro_wait must be called from within a srey coroutine", 2)
    end
    _set_coro_sess(coro_running, sess, mtype, ms)
    nyield = nyield + 1
    nwait = nwait + 1
    local msg = coroutine_yield()
    nwait = nwait - 1
    nyield = nyield - 1
    assert(sess == msg.sess, "different session.")
    return msg
end

---阻塞当前协程指定毫秒（底层使用定时器，不阻塞事件线程）
---@param ms integer 睡眠毫秒数；非正数直接返回不挂起，超 UINT32_MAX(约 49.7 天)由 C 层钳到该上界
function srey.sleep(ms)
    if ms <= 0 then
        return
    end
    local sess = srey.id()
    core.timeout(sess, ms)
    srey._coro_wait(sess, MSG_TYPE.TIMEOUT, 0)
end

---异步定时器：ms 毫秒后在新协程中调用 func(...)，当前协程不挂起
---@param ms integer 延迟毫秒数；非正数立即触发，超 UINT32_MAX(约 49.7 天)钳到该上界（均由 core.timeout 统一处理）
---@param func fun(...) 超时回调
---@param ... any 传给 func 的参数
function srey.timeout(ms, func, ...)
    local sess = srey.id()
    -- 必须先 core.timeout 再登记:它的 ms 校验会 longjmp,先登记就留下一条三条清理路径
    -- 都够不着的孤儿条目。换序无副作用——ms<=0 时定时器只往消息队列 push,不重入当前 Lua 栈
    core.timeout(sess, ms)
    _set_coro_sess(nil, sess, MSG_TYPE.TIMEOUT, 0, func, ...)
end

---@param msg Message
local function _timeout_dispatch(msg)
    local coroinfo = _get_coro_sess(msg.sess, MSG_TYPE.TIMEOUT)
    if not coroinfo then
        WARN("can't find session %s.", tostring(msg.sess))
        return
    end
    if coroinfo.func then
        local func, args = coroinfo.func, coroinfo.args
        _coro_run(_coro_cb, func, nil, tunpack(args, 1, args.n))
    elseif coroinfo.coro then
        local coro = coroinfo.coro
        _coro_resume(coro, msg)
    else
        WARN("coroinfo has neither func nor coro, sess %s.", tostring(msg.sess))
    end
end

---注册跨 task 请求处理回调；收到 REQUEST 消息时在新协程中调用，需主动调 srey.response 发回结果
---@param func fun(reqtype:integer, sess:integer, src:integer, data:lightuserdata?, size:integer) 请求回调（src 为发送方数字句柄）
function srey.on_requested(func)
    func_cbs[MSG_TYPE.REQUEST] = func
end

---同步跨 task 请求：挂起当前协程直到收到对端 response 或超时
---@param dst TASK_NAME 目标 task name
---@param reqtype integer 业务请求类型(uint16);REQUEST_TYPE 内的值为框架保留,业务请避开
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
---@return lightuserdata|nil rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝。
---       对端以 srey.response(..., ERR_OK) 无载荷应答时也是 nil，故不可用它判成败
---@return integer|nil rsize 响应数据长度；请求成功时非 nil（无载荷为 0），失败/超时为 nil。判成败用 `if not rsize`
function srey.request(dst, reqtype, data, size, copy)
    if TASK_NAME.NONE == dst then
        WARN("parameter error.")
        _ud_free_copy(data, copy)
        return nil
    end
    local sess = srey.id()
    if not core.request(dst, reqtype, sess, data, size, copy) then
        WARN("target task not found: %s.", tostring(dst))
        return nil
    end
    local msg = srey._coro_wait(sess, MSG_TYPE.RESPONSE, srey.get_request_timeout())
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
---@param reqtype integer 业务请求类型(uint16);REQUEST_TYPE 内的值为框架保留,业务请避开
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
function srey.call(dst, reqtype, data, size, copy)
    -- 调 core.call 前的早退出路径：copy=0 时调用方已转移所有权,主动 utils.ud_free 兜底
    if TASK_NAME.NONE == dst then
        WARN("parameter error.")
        _ud_free_copy(data, copy)
        return
    end
    if not core.call(dst, reqtype, data, size, copy) then
        WARN("target task not found: %s.", tostring(dst))
    end
end

---广播请求（fire-and-forget RPC）：同一份 data 投递给 N 个 task,各 dst 可独立 task_response 回当前 task
---（共用同一 sess）。框架不挂起协程不做聚合,响应到达时触发 srey.on_responsed 回调,业务在回调内据 sess
---累计 / 区分。sess 由调用方传入(非 0),典型用法是配合 srey.id() 分配避免与 srey.request 自动 sess 冲突。
---每一条抛出路径(reqtype 越界 / sess 为 0 / dsts 非法)都发生在 C 层取载荷之前,抛出时 copy=0 的
---载荷所有权仍在调用方手上,须自行 utils.ud_free；无处可投返回 0 那条走到了取载荷之后,由 C 释放。
---口径同 srey.send_multi。逐参说明见 core.multi_request
---@type fun(dsts:TASK_NAME[], reqtype:integer, sess:integer, data:string|lightuserdata|nil, size:integer?, copy:integer?):integer
srey.multi_request = core.multi_request

---单向广播跨 task 消息（fire-and-forget）：同一份 data 投递给 N 个 task，
---C 层 shared_data 引用计数自动释放，比 N 次 srey.call 节省 N-1 份内存拷贝
---@param dsts TASK_NAME[] 目标 task name 数组；TASK_NAME.NONE 与 grab 失败的项被跳过
---@param reqtype integer 业务请求类型(uint16);REQUEST_TYPE 内的值为框架保留,业务请避开
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
function srey.multi_call(dsts, reqtype, data, size, copy)
    core.multi_call(dsts, reqtype, data, size, copy)
end

---回应"目标 task 没注册 on_requested"。srey.call 走 task_call，src 恒为 INVALID_TNAME、
---sess 恒为 0，压根回不了响应；照旧调 srey.response 会被它自己的参数守卫拦下打出
---"parameter error."，把配置错误伪装成参数错误、带偏排查方向
---@param subtype integer 请求类型
---@param sess integer 会话 id
---@param src integer 请求方 task name
local function _no_request_func(subtype, sess, src)
    if TASK_NAME.NONE == src or 0 == sess then
        WARN("not register request function, subtype %s from %s.", tostring(subtype), tostring(src))
        return
    end
    srey.response(src, subtype, sess, ERR_FAILED, "not register request function.")
end

local _debug_request -- 懒加载缓存
-- task 请求分发：REQ_DEBUG 走 lib.debug_request，其余转交用户注册的 on_requested 回调
---@param msg Message
local function _request_dispatch(msg)
    if REQUEST_TYPE.REQ_DEBUG == msg.subtype then
        if not _debug_request then
            _debug_request = require("lib.debug_request")
            _debug_request._set_coro_sess(coro_sess)
            _debug_request._set_response(srey.response)
            _debug_request._set_mtype_names(MSG_TYPE)
            _debug_request._set_suspend_registries(fork_barriers, serial_execs,
                function() return nyield end)
            -- 未知 debug 命令透传业务 on_requested；_dispatch 已在协程内,直接调 func 同协程跑
            _debug_request._set_fallback(function(reqtype, sess, src, data, size)
                local func = func_cbs[MSG_TYPE.REQUEST]
                if func then
                    func(reqtype, sess, src, data, size)
                else
                    _no_request_func(reqtype, sess, src)
                end
            end)
        end
        _coro_run(_coro_cb, _debug_request._dispatch, msg, msg.subtype, msg.sess, msg.src, msg.data, msg.size)
    else
        local func = func_cbs[MSG_TYPE.REQUEST]
        if not func then
            _no_request_func(msg.subtype, msg.sess, msg.src)
            return
        end
        _coro_run(_coro_cb, func, msg, msg.subtype, msg.sess, msg.src, msg.data, msg.size)
    end
end

---向请求方 task 回复响应
---@param dst TASK_NAME 请求方 task name
---@param reqtype integer 请求类型(uint16,回带给请求方)
---@param sess integer 请求会话 id
---@param erro integer 错误码，0 表示成功
---@param data string|lightuserdata|nil 响应数据
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer? 是否复制数据，默认 1
function srey.response(dst, reqtype, sess, erro, data, size, copy)
    -- 调 core.response 前的早退出路径：copy=0 时调用方已转移所有权,主动 utils.ud_free 兜底
    if TASK_NAME.NONE == dst or 0 == sess then
        WARN("parameter error.")
        _ud_free_copy(data, copy)
        return
    end
    if not core.response(dst, reqtype, sess, erro, data, size, copy) then
        WARN("target task not found: %s.", tostring(dst))
    end
end

---@param msg Message
local function _response_dispatch(msg)
    -- sess 不在 coro_sess 时静默起新协程跑全局 on_responsed，不告警：口径同 C 侧
    -- MSG_TYPE_RESPONSE 那行。srey.multi_request 广播的 N 个响应本就没有等待者，
    -- 在这里告警等于每次广播刷 N 条；真孤儿由 srey.request 自己的超时告警报出
    if not _resume_waiter(msg, MSG_TYPE.RESPONSE) then
        _dispatch_cb(msg, func_cbs[MSG_TYPE.RESPONSE], msg.subtype, msg.sess, msg.erro, msg.data, msg.size)
    end
end

---注册全局 response 回调；srey.request 等协程同步 API 不走此回调,仅当 sess 不在协程等待表时触发
---（典型场景：srey.multi_request 广播 N 个响应共用 sess,框架不做聚合,业务在此回调中据 sess 累计）
---@param func fun(reqtype:integer, sess:integer, erro:integer, data:lightuserdata?, size:integer) 响应回调
function srey.on_responsed(func)
    func_cbs[MSG_TYPE.RESPONSE] = func
end

---把 socket 的会话键设为它自己的 skid，后续该 socket 消息携带此值。
---会话键不可自定义，理由见 core.session
---@type fun(fd:integer, skid:integer):boolean
srey.sock_session = core.session

---清除 socket 的会话键，此后该 socket 消息走注册的回调而非协程等待
---@type fun(fd:integer, skid:integer):boolean
srey.sock_session_clear = core.session_clear

---切换 socket 的应用层协议类型
---@type fun(fd:integer, skid:integer, pktype:PACK_TYPE):boolean
srey.sock_pack_type = core.pack_type

---设置 socket 状态标志（具体含义由协议层定义）
---@type fun(fd:integer, skid:integer, status:integer):boolean
srey.sock_status = core.status

---将 socket 绑定到指定 task（跨 task 推送场景）；目标不存在（名字未注册 / 数字句柄对应 task 已退出）返回 false。
---仅保证调用时目标存在：目标若在绑定之后才退出，该连接下一条消息会被静默关闭
---@type fun(fd:integer, skid:integer, tname:TASK_NAME):boolean
srey.sock_bind_task = core.bind_task

---查 SSL 上下文:NONE→(true,nil) 明文;查到→(true,ssl);name 未注册→(false,nil);error/WARN 由调用处按需处理
---@param sslname SSL_NAME
---@return boolean ok  name 已注册(或 NONE)为 true;未注册 false
---@return lightuserdata? ssl  NONE 时 nil
function srey.ssl_qury(sslname)
    if SSL_NAME.NONE == sslname then
        return true
    end
    local ssl = core.ssl_qury(sslname)
    if not ssl then
        return false
    end
    return true, ssl
end

---注册新连接 accept 回调；每次有连接进来在新协程中调用
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer) accept 回调
function srey.on_accepted(func)
    func_cbs[MSG_TYPE.ACCEPT] = func
end

---开始监听指定地址
---@param pktype PACK_TYPE 应用层协议类型
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param ip string 监听 IP。"::" 只收 IPv6(强制 IPV6_V6ONLY)，要同时收两种就 "0.0.0.0" 与 "::" 各监听一次
---@param port integer 监听端口
---@param netev NET_EV? 事件订阅掩码
---@return integer lsnid 监听 id；失败返回 ERR_FAILED(-1)
function srey.listen(pktype, sslname, ip, port, netev)
    local ok, ssl = srey.ssl_qury(sslname)
    if not ok then
        WARN("ssl_qury not find ssl name %s.", sslname)
        return ERR_FAILED
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
        _coro_run(_coro_cb, func, nil, msg.subtype, msg.fd, msg.skid)
    end
end

---注册主动连接结果回调；仅在没有协程等待该连接时触发（非 srey.connect 发起的）
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, err:integer) connect 回调
function srey.on_connected(func)
    func_cbs[MSG_TYPE.CONNECT] = func
end

---内部辅助：等一条指定类型的消息。超时则关连接并告警，连接已关也告警，两种都返 nil。
---四个等待点(connect / ssl exchange / handshake / recv)只差 mtype、超时值与告警里的动作名。
---C 侧 coro.c 的 _coro_wait_msg 结构同一套，但它的 CLOSE 分支不告警（那边调用方是每命令
---一轮的循环，一条连接断掉能刷几十条）。改任一端的结构要同步改另一端
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param mtype integer MSG_TYPE.* 期望的消息类型
---@param ms integer 超时毫秒
---@param tag string 告警文案里的动作名
---@return Message|nil msg 收到的消息表；超时/断开返回 nil
local function _wait_msg(fd, skid, mtype, ms, tag)
    -- 连接已 teardown 就别挂上去,理由同 C 侧 _coro_wait_msg;各 wait_* / syn_* 入口都经本函数
    if INVALID_SOCK == fd then
        return nil
    end
    local msg = srey._coro_wait(skid, mtype, ms)
    if MSG_TYPE.TIMEOUT == msg.mtype then
        srey.close(fd, skid)
        WARN("%s timeout, skid %s.", tag, tostring(skid))
        return nil
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        WARN("%s connection closed, skid %s.", tag, tostring(skid))
        return nil
    end
    return msg
end

---同步等待异步 connect 完成（由 task_connect / core.connect 异步发起后调用）；ssl 非 nil 时同时等待 SSL 握手
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param ssl any? 非 nil 时表示需要等待 SSL 握手
---@return boolean ok 成功 true；超时/失败时已关闭 fd 并返回 false
function srey.wait_connect(fd, skid, ssl)
    local msg = _wait_msg(fd, skid, MSG_TYPE.CONNECT, srey.get_connect_timeout(), "connect")
    if not msg then
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
---成功后若启用 TLS 自动等待 SSL 握手完成；连接建立即置 ud->sess=skid（同步请求/响应模式）
---@param pktype PACK_TYPE 应用层协议类型
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param ip string 对端 IP
---@param port integer 对端端口
---@param netev NET_EV? 事件订阅掩码
---@param extra lightuserdata? 协议专用附加参数（如 WebSocket 握手验证 key）；所有权一律在本函数内交出——
---正常返回时归 C 层（连接失败也由 C 侧 ud_free 回收），抛出前本函数已自行释放。调用方无论哪条路径都不要再碰它
---@return integer fd socket fd；失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；失败为 nil（失败与成功的返回值个数一致，见 lpub_rtn_nil）
function srey.connect(pktype, sslname, ip, port, netev, extra)
    local ok, ssl = srey.ssl_qury(sslname)
    if not ok then
        WARN("ssl_qury not find ssl name %s.", sslname)
        -- extra 尚未传给 C 层，由本函数释放
        if extra then
            utils.ud_free(extra)
        end
        -- 三条失败路径都得补上第二个值：成功返 (fd, skid)，少返一个会让
        -- srey.close(srey.connect(...)) 这类转发在失败分支上参数错位
        return INVALID_SOCK, nil
    end
    local fd, skid
    ok, fd, skid = pcall(core.connect, pktype, ssl, ip, port, netev, extra, 1)
    if not ok then
        -- core.connect 的入参检查都排在取 extra 之前，抛到这里说明所有权还没交出去
        if extra then
            utils.ud_free(extra)
        end
        error(fd, 0)
    end
    if INVALID_SOCK == fd then
        WARN("connect %s:%d error.", ip, port)
        return INVALID_SOCK, nil
    end
    if not srey.wait_connect(fd, skid, ssl) then
        return INVALID_SOCK, nil
    end
    return fd, skid
end

---@param msg Message
local function _net_connect_dispatch(msg)
    if not _resume_waiter(msg, MSG_TYPE.CONNECT) then
        _dispatch_cb(msg, func_cbs[MSG_TYPE.CONNECT], msg.subtype, msg.fd, msg.skid, msg.erro)
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
    local ok, ssl = srey.ssl_qury(sslname)
    if not ok then
        WARN("ssl_qury not find ssl name %s.", sslname)
        return false
    end
    if not ssl then -- SSL_NAME.NONE:无 SSL 可交换
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
    return nil ~= _wait_msg(fd, skid, MSG_TYPE.SSLEXCHANGED, srey.get_netread_timeout(), "ssl exchange")
end

---@param msg Message
local function _net_ssl_exchanged_dispatch(msg)
    if not _resume_waiter(msg, MSG_TYPE.SSLEXCHANGED) then
        _dispatch_cb(msg, func_cbs[MSG_TYPE.SSLEXCHANGED], msg.subtype, msg.fd, msg.skid, msg.client)
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
    local msg = _wait_msg(fd, skid, MSG_TYPE.HANDSHAKED, srey.get_netread_timeout(), "handshake")
    if not msg then
        return false
    end
    return ERR_OK == msg.erro, msg.data, msg.size
end

---@param msg Message
local function _net_handshaked_dispatch(msg)
    if not _resume_waiter(msg, MSG_TYPE.HANDSHAKED) then
        _dispatch_cb(msg, func_cbs[MSG_TYPE.HANDSHAKED],
                     msg.subtype, msg.fd, msg.skid, msg.client, msg.erro, msg.data, msg.size)
    end
end

---注册数据接收回调；仅在没有协程通过 syn_send 等待该 socket 时触发
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, client:integer, slice:integer, data:lightuserdata?, size:integer) RECV 回调
function srey.on_recved(func)
    func_cbs[MSG_TYPE.RECV] = func
end

---异步发送数据（不等待响应）
---发送数据（参数详见 core.send）
---@type fun(fd:integer, skid:integer, data:string|lightuserdata, size:integer?, copy:integer?):boolean
srey.send = core.send

---多播发送：把同一份 data 零拷贝广播给多个 fd；C 层 shared_data 引用计数自动释放。
---每一条抛出路径(fds/skids 非 table / 长度不等 / 元素非整数)都发生在 C 层取载荷之前，抛出时
---copy=0 的载荷所有权仍在调用方手上，须自行 utils.ud_free；空数组返回 false 那条走到了取载荷
---之后，由 C 释放。口径同 srey.multi_request。逐参说明见 core.send_multi
---@type fun(fds:integer[], skids:integer[], data:string|lightuserdata, size:integer?, copy:integer?):boolean
srey.send_multi = core.send_multi

---内部辅助：挂起协程等待该 socket 的下一个 RECV 消息（含超时/断开处理）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@return Message|nil msg 收到的消息表；超时/断开返回 nil
local function _wait_net_recv(fd, skid)
    return _wait_msg(fd, skid, MSG_TYPE.RECV, srey.get_netread_timeout(), "netread")
end

---同步发送并等待响应：发送后挂起协程，收到回包后返回数据；适用于请求-响应模式
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param data string|lightuserdata 数据
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer 1=复制；0=零拷贝
---@return lightuserdata|nil rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝；失败/超时返回 nil
---@return integer? rsize 响应数据长度
---@return integer? rslice 分片类型（SLICE_TYPE.*，0 为非分片）；为 SLICE_TYPE.START 时本条只是首片，
---调用方须接着用 syn_slice 循环收到 fin 为止。哪些响应算分片由协议层判定，不要在这里另抄一套规则
function srey.syn_send(fd, skid, data, size, copy)
    if not srey.send(fd, skid, data, size, copy) then
        return nil
    end
    local msg = _wait_net_recv(fd, skid)
    if not msg then
        return nil
    end
    return msg.data, msg.size, msg.slice
end
---同步接收下一个响应包（不发送）：用于一次请求产生多个响应的场景（如 MySQL 多结果集续接）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@return lightuserdata|nil rdata 响应数据指针；同 syn_send 语义，仅本协程下次 yield 前有效；
---超时/断开返回 nil；fd 为 INVALID_SOCK 时不挂起直接返回 nil
---@return integer? rsize 响应数据长度
---@return integer? slice 分片标记，取值同 srey.SLICE_TYPE；0 表示非分片完整消息
function srey.syn_recv(fd, skid)
    local msg = _wait_net_recv(fd, skid)
    if not msg then
        return nil
    end
    return msg.data, msg.size, msg.slice
end

---同步接收下一个数据分片（不发送）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@return boolean ok 接收成功 true
---@return boolean? fin 是否为最后一片或非分片完整消息（仅 ok=true 时）
---@return lightuserdata? data 分片数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝
---@return integer? size 分片字节数
function srey.syn_slice(fd, skid)
    local msg = _wait_net_recv(fd, skid)
    if not msg then
        return false
    end
    return true, (SLICE_TYPE.END == msg.slice or 0 == msg.slice), msg.data, msg.size
end

-- net_call / net_request 的共同前半：校验 dst → harbor 组包 → 同步发送 → 取 HTTP 状态行。
-- oneway 传给 harbor.pack：1 为单向不等业务回执，0 要回执。name 只用于告警文案
---@param oneway integer 1 单向 / 0 要回执
---@param name string 调用方名字，仅用于 WARN 文案
---@return string[]|nil status 状态行分段；任一步失败为 nil（失败原因已打过 WARN）
---@return lightuserdata? respdata 成功时的完整响应包，供调用方继续取 heads / body
local function _net_rpc(fd, skid, dst, oneway, reqtype, data, size, name)
    if "number" ~= type(dst) then
        WARN("%s dst must be a remote task handle(integer).", name)
        return nil
    end
    local reqdata, reqsize = harbor.pack(dst, oneway, reqtype, data, size)
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
    return status, respdata
end

---通过 harbor 协议向目标 task 发起单向 call（HTTP 封装，不等待返回数据）
---@param fd integer harbor 连接 fd（pktype 必须为 HTTP）
---@param skid integer 连接 skid
---@param dst integer 远端 task 的数字句柄（harbor 在对端按此值 task_grab）。
---       句柄由对端 createid 运行期生成，本地无从推导，须业务自行获取（由对端上报）；
---       不可传 TASK_NAME 字符串——那是本地名字，对远端无意义
---@param reqtype integer 业务请求类型(uint16);REQUEST_TYPE 内的框架保留值会被对端 harbor 拒为 404
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@return boolean ok 远端返回 200 OK 时 true
function srey.net_call(fd, skid, dst, reqtype, data, size)
    local status = _net_rpc(fd, skid, dst, 1, reqtype, data, size, "net_call")
    if not status then
        return false
    end
    return "200" == status[2]
end

---通过 harbor 协议向目标 task 发起同步请求，等待返回数据
---@param fd integer harbor 连接 fd（pktype 必须为 HTTP）
---@param skid integer 连接 skid
---@param dst integer 远端 task 的数字句柄（harbor 在对端按此值 task_grab）。
---       句柄由对端 createid 运行期生成，本地无从推导，须业务自行获取（由对端上报）；
---       不可传 TASK_NAME 字符串——那是本地名字，对远端无意义
---@param reqtype integer 业务请求类型(uint16);REQUEST_TYPE 内的框架保留值会被对端 harbor 拒为 404
---@param data string|lightuserdata|nil 消息内容
---@param size integer? data 为 lightuserdata 时必填
---@return boolean ok 对端返回 200 即 true（目标已处理）；网络失败、无状态行、非 200 均为 false。
---       判成败只看这个值——目标成功但无负载时 rdata 也是 nil，据 rdata 判会把成功当失败而重发 RPC
---@return lightuserdata? rdata 响应数据指针；仅在本协程下次 yield（再调任意挂起 API）前有效，下次 resume 时框架自动释放，需保留请自行拷贝；目标未回负载时为 nil
---@return integer? rsize 响应数据长度，无负载为 0
---@return integer? erro 目标真实错误码，取自对端 X-Srey-Erro 头（十进制）；对端未带该头时为 nil
function srey.net_request(fd, skid, dst, reqtype, data, size)
    local status, respdata = _net_rpc(fd, skid, dst, 0, reqtype, data, size, "net_request")
    if not status then
        return false
    end
    -- 用 http.head 而不是 heads[...]:头名按 RFC 大小写无关,中间件归一化成小写后裸查表取不到;
    -- 顺带省掉物化整张头表
    local erro = tonumber(http.head(respdata, "X-Srey-Erro"))
    if "200" ~= status[2] then
        WARN("net request return code %s erro %s skid %s.", status[2], tostring(erro), tostring(skid))
        return false, nil, 0, erro
    end
    local rdata, rsize = http.data(respdata)
    return true, rdata, rsize, erro
end

---@param msg Message
local function _net_recv_dispatch(msg)
    local func = func_cbs[MTYPE_RECV]
    if 0 == msg.sess or not may_resume(msg.subtype, msg.data) then
        if func then
            _coro_run(_coro_cb, func, msg, msg.subtype, msg.fd, msg.skid, msg.client, msg.slice, msg.data, msg.size)
        end
        return
    end
    if not _resume_waiter(msg, MTYPE_RECV) then
        _dispatch_cb(msg, func, msg.subtype, msg.fd, msg.skid, msg.client, msg.slice, msg.data, msg.size)
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
        _coro_run(_coro_cb, func, nil, msg.subtype, msg.fd, msg.skid, msg.client, msg.size)
    end
end

---注册连接关闭回调；CLOSE 消息会先唤醒所有在该 skid 上挂起等待的协程，再调用此回调
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, client:integer) CLOSE 回调
function srey.on_closed(func)
    func_cbs[MSG_TYPE.CLOSE] = func
end

local close_watchers = {}-- 库级 CLOSE 观察者；业务的 on_closed 仍是单槽，互不影响
---注册库级连接关闭观察者。on_closed 是单槽、后注册者静默覆盖前者，库拿不到关闭事件只能
---在文档里要求业务代为转接，漏接就是资源无声常驻（router 的流式请求上下文即如此）。
---本表与 func_cbs 并存，先于业务回调按注册顺序同步调用，故观察者内不得挂起。
---没有反注册：库对象与 task 同生命周期，用完就随 task 一起没了
---@param func fun(subtype:integer, fd:integer, skid:integer, client:integer) 观察者
function srey.watch_closed(func)
    close_watchers[#close_watchers + 1] = func
end

---主动关闭 TCP 连接（发送 FIN）。关闭前对发送队列冲一次：能写进内核的送达，写不进去的连同
---连接一起丢弃。没有"等发完再关"的模式——要保证大块数据送达，须自行确认对端已收齐再关
---@param fd integer socket fd
---@param skid integer 连接 skid
function srey.close(fd, skid)
    core.close(fd, skid)
end

---同步关闭：发起关闭后挂起协程等 CLOSE，保证协议层 close 回调（含 ctx->fd 复位）已执行；
---重连前用，避免旧连接异步 teardown 与新连接 try_connect 共享同一 ctx 时清掉新 fd。
---未发数据的丢弃契约同 srey.close：等的是"关完了"，不是"发完了"
---@param fd integer socket fd
---@param skid integer 连接 skid
function srey.sync_close(fd, skid)
    if INVALID_SOCK == fd then
        return
    end
    core.close(fd, skid)
    srey._coro_wait(skid, MSG_TYPE.CLOSE, srey.get_netread_timeout())
end

---处理连接关闭消息：进入时探测一次会话表，把该 sess(连接类即 skid)下全部挂起等待者转移到本地数组再消费，
---resume 期间业务代码在同一 sess 上重新注册的等待不会被本轮循环看到，而是追加回同一个会话表条目；
---回调关闭事件后重新查询该条目，仍为空才删除，避免 resume 期间的重新注册被误删/重复插入；
---不需要 C 侧那个"防 hashmap resize 导致 cofind 悬空"的技巧，Lua table 是引用类型，摘出来的 waiters 数组本身就独立有效
---@param msg Message
local function _net_close_dispatch(msg)
    local sess = msg.sess
    local corosess = coro_sess[sess]
    if corosess then
        corosess.keep = false -- 连接已关闭，不再保留，之后追加也不会被改回
        local waiters = corosess.waiters
        corosess.waiters = {}
        for i = 1, #waiters do
            _coro_resume(waiters[i].coro, msg)
        end
    end
    -- NEVERCONN 的合成 CLOSE 只为唤醒上面那批等待方，不触发 on_closed 观察者
    if CLOSE_TYPE.NEVERCONN ~= msg.erro then
        -- 库级观察者就地同步调：它们只做摘表/释放，起协程反而让清理排到本条消息之后
        for i = 1, #close_watchers do
            srey.xpcall(close_watchers[i], msg.subtype, msg.fd, msg.skid, msg.client)
        end
        local func = func_cbs[MSG_TYPE.CLOSE]
        if func then
            _coro_run(_coro_cb, func, nil, msg.subtype, msg.fd, msg.skid, msg.client)
        end
    end
    _coro_sess_del_empty(sess)
end

---注册 UDP 数据接收回调
---@param func fun(pktype:PACK_TYPE, fd:integer, skid:integer, ip:string, port:integer, data:lightuserdata?, size:integer) RECVFROM 回调
function srey.on_recvedfrom(func)
    func_cbs[MSG_TYPE.RECVFROM] = func
end

---创建 UDP socket 并绑定到 ip:port
---@param pktype integer 封包协议类型，参考 PACK_TYPE（原始透传用 PACK_TYPE.NONE）
---@param ip string? 绑定 IP，默认 "0.0.0.0"。"::" 只收 IPv6(强制 IPV6_V6ONLY)；多播时组地址须与此同族
---@param port integer? 绑定端口，默认 0（由 OS 分配）
---@return integer fd socket fd；失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；失败为 nil（失败与成功的返回值个数一致，见 lpub_rtn_nil）
function srey.udp(pktype, ip, port)
    if not ip then
        ip = "0.0.0.0"
    end
    if not port then
        port = 0
    end
    return core.udp(pktype, ip, port)
end

---UDP socket 加入多播组(按 group_ip 的 family 选 IPv4 / IPv6 选项)。组地址不合法或与 socket 绑定地址不同族直接返 false。
---返回 true 只表示参数合法且命令已入队,setsockopt 在事件线程执行、成败不回传(失败只有一条日志),
---下面 leave / ttl / loop 同此契约
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

---同步 UDP 发送并等待响应：设置会话键 → sendto → 挂起协程等 RECVFROM；
---同一 skid 上可连续/并发多次调用，多次调用与多次响应按到达顺序 FIFO 配对；
---网络乱序时配对结果仍可能与发送顺序不一致——UDP 协议本身无法避免的限制
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
    -- 调 core.sendto 前的早退出路径：copy=0 时调用方已转移所有权,主动 utils.ud_free 兜底
    -- （utils.ud_free 内部仅对 lightuserdata 生效,非 lightuserdata 自动跳过）
    if not srey.sock_session(fd, skid) then
        _ud_free_copy(data, copy)
        return nil
    end
    if not srey.sendto(fd, skid, ip, port, data, size, copy) then
        WARN("sendto error, skid %s.", tostring(skid))
        return nil
    end
    local msg = srey._coro_wait(skid, MSG_TYPE.RECVFROM, srey.get_netread_timeout())
    if MSG_TYPE.TIMEOUT == msg.mtype then
        WARN("sendto timeout, skid %s.", tostring(skid))
        return nil
    end
    if MSG_TYPE.CLOSE == msg.mtype then
        return nil
    end
    return msg.udata, msg.size
end

---@param msg Message
local function _net_recvfrom_dispatch(msg)
    -- UDP 本身不保证顺序与送达，找不到等待者（迟到/孤儿包）是正常场景，故 warn 传 false
    if not _resume_waiter(msg, MSG_TYPE.RECVFROM) then
        _dispatch_cb(msg, func_cbs[MSG_TYPE.RECVFROM],
                     msg.subtype, msg.fd, msg.skid, msg.ip, msg.port, msg.udata, msg.size)
    end
end

-- 唤醒一个会话里所有已到期的等待者。单独提出来是为了让调用方逐条 xpcall：游标已经按
-- "这些都会被摘掉"算过了，一条抛出(memlimit 下建 msg 表 OOM)就把
-- 后面几条一起跳过的话，它们再也等不到下一次扫描
local function _timeout_wake(sess, now)
    local corosess = coro_sess[sess]
    if not corosess then
        return
    end
    local msg = {mtype = MSG_TYPE.TIMEOUT, sess = sess}
    local coroinfo
    local j = 1
    while j <= #corosess.waiters do
        coroinfo = corosess.waiters[j]
        if coroinfo.timeout > 0 and now >= coroinfo.timeout then
            tremove(corosess.waiters, j)
            _coro_resume(coroinfo.coro, msg)
            WARN("resume timeout session %s.", tostring(sess))
        else
            j = j + 1
        end
    end
    -- 超时路径无视 keep：理由同 C 侧 _coro_timeout_monitor
    if 0 == #corosess.waiters then
        coro_sess[sess] = nil
    end
end

-- 扫描体提成模块级函数,不每拍现造闭包
local function _timeout_scan()
    -- 用 nwait 不用 nyield：只有 _coro_wait 挂起的协程才在 coro_sess 里，
    -- 下面这趟走的就是它；fork_wait / serial 排队的协程扫也扫不到
    if nwait > 0 then
        local now = srey.timer_ms()
        -- coro_sess 的条目数是"活跃会话数"而不是"挂起协程数"：keep 的 sess 摘空 waiters 后
        -- 仍留到 CLOSE 才清。只按 nwait 开闸的话，持几百条连接的 client task 每秒都要白走
        -- 一遍全表，而真到期的通常是 0 个。C 侧 _coro_timeout_monitor 判的是堆顶，同一道理
        if now < next_timeout then
            return
        end
        local cnt = 0
        local nearest = math.huge
        local _timeout_buf = {}
        local hit
        local dl
        for sess, corosess in pairs(coro_sess) do
            hit = false
            -- 不能一撞到期就 break：排在它后面的未到期项就进不了 nearest，游标会偏大而漏唤醒
            for i = 1, #corosess.waiters do
                dl = corosess.waiters[i].timeout
                if dl > 0 then
                    if now >= dl then
                        hit = true
                    elseif dl < nearest then
                        nearest = dl
                    end
                end
            end
            if hit then
                cnt = cnt + 1
                _timeout_buf[cnt] = sess
            end
        end
        -- 全扫过一遍游标就准了；本轮要唤醒的那些下面会摘掉，不计入 nearest。
        -- 赋值排在唤醒之前：resume 期间新登记的更近 deadline 才 min 得进来
        next_timeout = nearest
        local cur_sess
        for i = 1, cnt do
            cur_sess = _timeout_buf[i]
            _timeout_buf[i] = nil
            -- nearest 是按"这批都会被摘掉"算的。摘不掉就得把游标退回 0 重扫，
            -- 否则那些 deadline 比 next_timeout 还早的等待者从此永远扫不到：
            -- 它们的协程不会醒、nwait 回不到 0、task 退不掉
            if not srey.xpcall(_timeout_wake, cur_sess, now) then
                next_timeout = 0
            end
        end
    end
end

---定时扫描所有挂起的协程，将已超时者强制 resume（携带 TIMEOUT 消息）；每 1 秒触发一次，
---通过 srey.timeout 自我调度形成循环；扫描主体用 xpcall 包裹保证循环不被异常中断。
---续下一拍必须排在最前：放末尾时扫描体一抛异常，这条 1 秒链就永久断掉，没有补挂路径
local function _coro_timeout()
    srey.timeout(1 * 1000, _coro_timeout)
    srey.xpcall(_timeout_scan)
    _coro_pool_shrink()
end
---消息表：全部字段只读。带载荷的消息（RECV/RECVFROM/HANDSHAKED/REQUEST/RESPONSE）挂了 __gc，
---回收时按 mtype 选释放函数、按 data/shared 取指针——改写它们等于换掉 C 侧的释放契约：
---mtype 写成别的类型会用错释放器（例如 RECV 的 http_pack_ctx 被当成裸 buffer 直接 FREE），
---data 换成别的指针则是拿它去做一次任意释放。元表本身已由 __metatable 挡住，字段挡不住。
---@class Message
---@field mtype   MSG_TYPE       消息类型（MSG_TYPE.*），始终存在
---@field sess    integer?       会话 id；TIMEOUT/RECV/CLOSE/CONNECT/SSLEXCHANGED/HANDSHAKED/RECVFROM/REQUEST/RESPONSE 携带
---@field fd      integer?       socket fd；网络消息(ACCEPT/RECV/SEND/CLOSE/CONNECT/SSLEXCHANGED/HANDSHAKED/RECVFROM)携带
---@field skid    integer?       连接 skid；同 fd 一起携带
---@field subtype PACK_TYPE?     封包协议类型；上述网络消息及 REQUEST/RESPONSE 携带
---@field erro    integer?       错误码；CONNECT/HANDSHAKED/RESPONSE 携带。CLOSE 上是 CLOSE_TYPE.*，表示连接是怎么断的
---@field client  integer?       1=客户端 0=服务端（非地址）；RECV/SEND/CLOSE/SSLEXCHANGED/HANDSHAKED 携带
---@field data    lightuserdata? 数据指针；RECV/HANDSHAKED/RECVFROM/REQUEST/RESPONSE 携带（仅数据非空）
---@field size    integer?       数据字节数；RECV/SEND/HANDSHAKED/RECVFROM/REQUEST/RESPONSE 携带
---@field slice   integer?       分片类型（SLICE_TYPE.*）；RECV 携带
---@field src     integer?       请求方 task 数字句柄；REQUEST 携带
---@field ip      string?        UDP 源 IP；RECVFROM 携带
---@field port    integer?       UDP 源端口；RECVFROM 携带
---@field udata   lightuserdata? UDP 数据指针；RECVFROM 携带
---@field shared  lightuserdata? 广播共享数据（内部，__gc 释放用）；REQUEST 广播（multi_call/multi_request）时携带

-- STARTUP 消息处理：注册 1s 定时器驱动协程池收缩，再跑业务 startup 回调
local function _startup_msg_dispatch(msg)
    srey.timeout(1 * 1000, _coro_timeout)
    _startup_dispatch()
end

local _dispatchers = {
    [MSG_TYPE.STARTUP] = _startup_msg_dispatch,
    [MSG_TYPE.CLOSING] = _closing_dispatch,
    [MSG_TYPE.TIMEOUT] = _timeout_dispatch,
    [MSG_TYPE.ACCEPT] = _net_accept_dispatch,
    [MSG_TYPE.CONNECT] = _net_connect_dispatch,
    [MSG_TYPE.SSLEXCHANGED] = _net_ssl_exchanged_dispatch,
    [MSG_TYPE.HANDSHAKED] = _net_handshaked_dispatch,
    [MSG_TYPE.RECV] = _net_recv_dispatch,
    [MSG_TYPE.SEND] = _net_sended_dispatch,
    [MSG_TYPE.CLOSE] = _net_close_dispatch,
    [MSG_TYPE.RECVFROM] = _net_recvfrom_dispatch,
    [MSG_TYPE.REQUEST] = _request_dispatch,
    [MSG_TYPE.RESPONSE] = _response_dispatch,
}

---全局消息分发入口，由 C 层 loader 在每条消息到达时调用；按 msg.mtype 查表路由到对应内部分发函数。
---dispatcher 包 xpcall 是为了让下面两个 drain 无条件跑到：分发过程中可能已有协程走完
---srey.serial 的 _release，此时 current/ref 已写死、后继者只是排进 serial_wakes 等着被起跑，
---抛出跳过 drain 就把锁留给一个没在跑的协程。抛出点确实存在——_request_dispatch 里有惰性 require，
---模块或其依赖加载失败即抛，而它跑在协程外，没有 _coro_cb 的 xpcall 兜着
---@param msg Message 由 C 层 _ltask_pack_msg 打包的消息表
function message_dispatch(msg)
    local dispatcher = _dispatchers[msg.mtype]
    if dispatcher then
        srey.xpcall(dispatcher, msg)
    end
    -- 两个队列互相喂：被唤醒的 serial 等待者可能 srey.fork，新起的协程也可能进 serial 排队并
    -- 触发交接，故循环到两者都空为止
    while 0 ~= #fork_queue or 0 ~= #serial_wakes do
        _drain(fork_queue, _run_fork)-- srey.fork 在此起新协程
        _drain(serial_wakes, _coro_resume)-- srey.serial 交接后的队头等待者在此起跑
    end
end

return srey
