-- 通用有界连接池：管理 mysql / pgsql / mongo / smtp 这类 conn_pub 长连接，每个池最多 max 条。
-- 多协程按 FIFO 借还；连接内部的命令串行仍由各 conn 自己的 serial 负责，池只管“借哪条”。
-- 连接对象带 reset_session 方法时（mysql / pgsql 有），归还时先清会话状态（回滚没结束的事务）再交给下一个人。
--
-- 等待者用 srey._coro_wait 挂在池自己的 sess 上，put 用 srey._coro_wake 把连接直接交接给队头
-- （不经过空闲栈），唤醒统一在 message_dispatch 末尾摊平。池状态只在不会挂起的语句里改：
-- connect / ping / quit / reset_session 会挂起，调用前先把 busy、空闲栈等状态落好。
-- 配了 idle_timeout / min / leak_warn 时，第一次 get 起一个维护协程，每秒收缩空闲、补足 min、查借出过久。
-- 所有接口（含 free）都要在 srey 协程里调；free 应放在 task 的 closing 回调里：维护协程与一直等的 get
-- 只有它能叫醒，不调 task 关不掉。

local srey = require("lib.srey")

local TAG_DB_POOL <const> = srey.TAG_DB_POOL
local TAG_DB_POOL_TICK <const> = srey.TAG_DB_POOL_TICK
local TICK_MS <const> = 1000 -- 维护协程的巡检周期；协程超时本就是秒级粒度
local BACKOFF_SHIFT <const> = 5 -- 连续失败的冷却最多翻到 backoff 的 32 倍
local DEF_BACKOFF <const> = 1000 -- backoff 配 0 时的默认冷却毫秒，同 C 侧 DB_POOL_DEF_BACKOFF_MS
local WAKE_RETRY <const> = {} -- 叫醒等待者回去重查的载荷（池关闭时醒来自己看 closed）；只读，不带字段

local M = class("db_pool_ctx")

---池管理的连接对象：mysql / pgsql / mongo / smtp 的 ctx 都满足
---@class DbPoolConn
---@field established boolean 当前是否连着
---@field connect fun(self:DbPoolConn):boolean 建连/重连；会挂起
---@field ping fun(self:DbPoolConn):boolean 探活；会挂起
---@field quit fun(self:DbPoolConn) 断开；会挂起
---@field reset_session (fun(self:DbPoolConn):boolean)? 归还时清会话状态；会挂起

---构造配置：factory 必填，其余可省
---@class DbPoolOpts
---@field factory fun():DbPoolConn 新建一条未连接的连接对象
---@field name string? 日志用名字
---@field max integer? 连接对象上限，默认 4
---@field min integer? 至少保持的连着的空闲连接数：收缩时不低于它，维护协程每秒补足；默认 0
---@field get_timeout integer? get 默认超时毫秒，0=一直等
---@field ping_idle integer? 空闲超过这么久先 ping，0=不探活
---@field backoff integer? 建连/探活失败后该连接的冷却毫秒，连续失败按它翻倍、最多 32 倍，默认 1000
---@field max_lifetime integer? 连接寿命毫秒，每条随机少最多 1/8；0=不限制
---@field idle_timeout integer? 空闲连接收缩毫秒，维护协程每秒查一次；0=不收缩
---@field leak_warn integer? 借出超过这么久还没还，维护协程打一次 WARN；0=不查

---stat_get 拷出的计数快照
---@class DbPoolStat
---@field total integer 已创建连接数（空闲+借出+正在建）
---@field idle integer 空闲栈里的连接数（含冷却中 / 断开的）
---@field busy integer 占用数（借出、交接中，以及池内部正在建连 / 探活 / 清会话的）
---@field waiters integer 挂起等连接的协程数
---@field connects integer 成功建连次数
---@field connfails integer 建连/探活失败次数
---@field pings integer 成功探活次数
---@field timeouts integer get 超时次数
---@field gets integer 成功借出次数
---@field puts integer 业务归还次数（维护协程补 min 的内部归还不算）
---@field leaks integer 借出过久告警次数

---构造函数
---@param opts DbPoolOpts 配置
function M:ctor(opts)
    assert("table" == type(opts), "db_pool.new: opts must be a table")
    assert("function" == type(opts.factory), "db_pool.new: factory required")
    local max = opts.max or 4
    assert("number" == type(max) and max > 0 and max % 1 == 0, "db_pool.new: max must be a positive integer")
    local min = opts.min or 0
    assert("number" == type(min) and min >= 0 and min % 1 == 0 and min <= max,
        "db_pool.new: min out of range")
    local get_timeout = opts.get_timeout or 0
    local ping_idle = opts.ping_idle or 0
    local backoff = opts.backoff or 0
    local max_lifetime = opts.max_lifetime or 0
    local idle_timeout = opts.idle_timeout or 0
    local leak_warn = opts.leak_warn or 0
    local function _check_ms(name, v)
        assert("number" == type(v) and v >= 0 and v % 1 == 0,
            "db_pool.new: " .. name .. " must be a non-negative integer")
    end
    _check_ms("get_timeout", get_timeout)
    _check_ms("ping_idle", ping_idle)
    _check_ms("backoff", backoff)
    _check_ms("max_lifetime", max_lifetime)
    _check_ms("idle_timeout", idle_timeout)
    _check_ms("leak_warn", leak_warn)
    self.name = opts.name or "db_pool"
    self.factory = opts.factory
    self.max = max
    self.min = min
    self.get_timeout = get_timeout
    self.ping_idle = ping_idle
    self.backoff = (0 == backoff) and DEF_BACKOFF or backoff
    self.max_lifetime = max_lifetime
    self.idle_timeout = idle_timeout
    self.leak_warn = leak_warn
    self.sess = srey.id()
    self.tick_sess = srey.id() -- 维护协程定时等待用，与等待者分开（取等待者只看队头）
    self.ticking = false
    self.idle = {} -- 空闲连接栈（LIFO，热连接优先）
    self.busy = {} -- conn -> true，借出/交接中
    self.nbusy = 0 -- 借出数（含已交接、还没被 resume 的）
    self.total = 0 -- 已创建连接数（空闲+借出+正在建）
    self.waiters = 0 -- 挂在 _coro_wait 上的协程数
    self.closed = false
    self.cool_until = {} -- conn -> 冷却截止
    self.fails = {} -- conn -> 连续建连/探活失败次数，冷却按它翻倍
    self.idle_at = {} -- conn -> 进入空闲的时刻；冷却中的不记
    self.born = {} -- conn -> 上次连上的时刻
    self.life = {} -- conn -> 本连接的寿命（max_lifetime 随机少最多 1/8）
    self.out_at = {} -- conn -> 交给调用方的时刻，借出过久告警用
    self.warned = {} -- conn -> true，本次借出已告警过
    self.stat = {
        connects = 0, -- 成功建连次数
        connfails = 0, -- 建连/探活失败次数
        pings = 0, -- 成功探活次数
        timeouts = 0, -- get 超时次数
        gets = 0, -- 成功借出次数
        puts = 0, -- 业务归还次数
        leaks = 0, -- 借出过久告警次数
    }
end

---丢弃一条连接：释放计数与元数据，再断开。调用方保证它已不在空闲栈里
---@param c DbPoolConn 连接对象
function M:_drop(c)
    self.total = self.total - 1
    self.cool_until[c] = nil
    self.fails[c] = nil
    self.idle_at[c] = nil
    self.born[c] = nil
    self.life[c] = nil
    self.out_at[c] = nil
    self.warned[c] = nil
    srey.xpcall(c.quit, c)-- 连接实现抛错不能把池的计数与关闭流程带停
end

---丢弃一条借出中的连接
---@param c DbPoolConn 连接对象
function M:_discard(c)
    if self.busy[c] then
        self.busy[c] = nil
        self.nbusy = self.nbusy - 1
    end
    self:_drop(c)
end

---池已关闭：丢掉这条借出中的连接，报 pool closed
---@param c DbPoolConn 连接对象
---@return nil
---@return string err
function M:_closed_drop(c)
    self:_discard(c)
    return nil, "pool closed"
end

---把借出中的连接放回空闲栈；cool_until 非 nil 表示冷却到该时刻（冷却中的不记空闲时刻，同 C 侧）
---@param c DbPoolConn 连接对象
---@param cool_until integer? 冷却截止毫秒；nil=不冷却
function M:_push_idle(c, cool_until)
    if self.busy[c] then
        self.busy[c] = nil
        self.nbusy = self.nbusy - 1
    end
    self.cool_until[c] = cool_until
    if nil == cool_until then
        self.idle_at[c] = srey.timer_ms()
    else
        self.idle_at[c] = nil
    end
    self.idle[#self.idle + 1] = c
end

---从空闲栈第 i 格摘掉（末格挪过来补位）
---@param i integer 下标
function M:_idle_remove_at(i)
    local idle = self.idle
    idle[i] = idle[#idle]
    idle[#idle] = nil
end

---从空闲栈取一条不在冷却中的，连着的优先（断开的要重连）；全在冷却返回 nil（此时空闲栈非空）。
---now 由调用方取，与随后的 _cool_wait 用同一个时刻，免得两次之间正好有冷却到点、两边都不认它
---@param now integer 当前毫秒
---@return DbPoolConn? conn
function M:_take_usable(now)
    local idle = self.idle
    local pick
    for i = #idle, 1, -1 do
        local c = idle[i]
        local cool = self.cool_until[c]
        if not cool or cool <= now then
            if c.established then
                pick = i
                break
            end
            pick = pick or i
        end
    end
    if not pick then
        return nil
    end
    local c = idle[pick]
    self:_idle_remove_at(pick)
    return c
end

---从空闲栈取一条断开着且不在冷却中的；没有返回 nil
---@return DbPoolConn? conn
function M:_take_cold()
    local now = srey.timer_ms()
    local idle = self.idle
    for i = #idle, 1, -1 do
        local c = idle[i]
        local cool = self.cool_until[c]
        if not c.established and (not cool or cool <= now) then
            self:_idle_remove_at(i)
            return c
        end
    end
    return nil
end

---空闲栈里连着的连接数
---@return integer
function M:_ready_idle()
    local n = 0
    for i = 1, #self.idle do
        if self.idle[i].established then
            n = n + 1
        end
    end
    return n
end

---离最早一条冷却结束还有多少毫秒；没有冷却中的返回 0
---@param now integer 当前毫秒
---@return integer
function M:_cool_wait(now)
    local min
    for i = 1, #self.idle do
        local cool = self.cool_until[self.idle[i]]
        if cool and cool > now and (not min or cool < min) then
            min = cool
        end
    end
    return min and (min - now) or 0
end

---连上了：失败计数清零，起算寿命
---@param c DbPoolConn 连接对象
---@param now integer 当前毫秒
function M:_born(c, now)
    self.born[c] = now
    self.fails[c] = nil
    if self.max_lifetime > 0 then
        self.life[c] = self.max_lifetime - math.random(0, self.max_lifetime // 8)
    end
end

---连接是否到寿命
---@param c DbPoolConn 连接对象
---@param now integer 当前毫秒
---@return boolean
function M:_expired(c, now)
    local born = self.born[c]
    return self.max_lifetime > 0 and nil ~= born and now - born >= self.life[c]
end

---建连/探活失败后的冷却截止：连续失败按 backoff 翻倍，最多翻 BACKOFF_SHIFT 次
---@param c DbPoolConn 连接对象
---@return integer cool_until
function M:_cool_after_fail(c)
    local fails = self.fails[c] or 0
    self.fails[c] = fails + 1
    return srey.timer_ms() + self.backoff * (1 << math.min(fails, BACKOFF_SHIFT))
end

---取出 / 新建失败后：已没有别的借出时叫醒等待者，让它们也快速失败
---@return boolean woke 叫醒过返回 true，调用方本次也直接失败（同 C 侧）
function M:_wake_on_fail()
    if self.waiters > 0 and 0 == self.nbusy then
        srey._coro_wake_all(self.sess, TAG_DB_POOL, WAKE_RETRY)
        return true
    end
    return false
end

---交给调用方：计成功借出，借出过久告警从这里起算
---@param c DbPoolConn 连接对象
---@return DbPoolConn c
function M:_hand_out(c)
    self.stat.gets = self.stat.gets + 1
    self.out_at[c] = srey.timer_ms()
    self.warned[c] = nil
    return c
end

---取出后确保连接可用：断了就 connect，空闲太久先 ping；失败放回空闲栈并冷却
---@param c DbPoolConn 连接对象
---@return DbPoolConn? conn 成功返回连接
---@return string? err 失败原因
function M:_checkout(c)
    local now = srey.timer_ms()
    if c.established and not self.born[c] then
        self:_born(c, now)-- 工厂给的是已连着的，首次取到起算寿命
    end
    if c.established and self:_expired(c, now) then
        self:_discard(c)
        return nil, "connection expired"
    end
    self.busy[c] = true
    self.nbusy = self.nbusy + 1
    if not c.established then
        local cok, connected = srey.xpcall(c.connect, c)
        if not cok or not connected then
            self.stat.connfails = self.stat.connfails + 1
            if self.closed then
                return self:_closed_drop(c)
            end
            self:_push_idle(c, self:_cool_after_fail(c))
            return nil, "connect failed"
        end
        self.stat.connects = self.stat.connects + 1
        self:_born(c, srey.timer_ms())
        if self.closed then
            return self:_closed_drop(c)
        end
    elseif self.ping_idle > 0 then
        local idle_at = self.idle_at[c]
        if idle_at and srey.timer_ms() - idle_at >= self.ping_idle then
            local pok, pinged = srey.xpcall(c.ping, c)
            if not pok or not pinged then
                self.stat.connfails = self.stat.connfails + 1
                if self.closed then
                    return self:_closed_drop(c)
                end
                self:_push_idle(c, self:_cool_after_fail(c))
                return nil, "ping failed"
            end
            self.stat.pings = self.stat.pings + 1
            self.fails[c] = nil
            if self.closed then
                return self:_closed_drop(c)
            end
        end
    end
    self.idle_at[c] = nil
    self.cool_until[c] = nil
    return c
end

---空闲收缩：连着的空闲连接只留 min 条，超过 idle_timeout 的断开丢掉
function M:_maybe_shrink()
    if self.idle_timeout <= 0 then
        return
    end
    -- 额度只算连着的，断开 / 冷却中的不占 min；quit 会挂起，额度与 now 每轮现算
    local i = 1
    while i <= #self.idle and self:_ready_idle() > self.min do
        local c = self.idle[i]
        local idle_at = self.idle_at[c]
        if c.established and idle_at and srey.timer_ms() - idle_at >= self.idle_timeout then
            self:_idle_remove_at(i)
            self:_drop(c)
        else
            i = i + 1
        end
    end
end

---新建并连上一条连接。建连失败的留在空闲栈里冷却（同 C 侧的槽），closed 由调用方在 yield 后复查
---@return DbPoolConn? conn 连上的连接
---@return string? err 失败原因
function M:_new_conn()
    self.total = self.total + 1
    local ok, c = srey.xpcall(self.factory)
    if not ok or not c then
        self.total = self.total - 1
        return nil, "factory failed"
    end
    self.busy[c] = true
    self.nbusy = self.nbusy + 1
    if not c.established then
        local cok, connected = srey.xpcall(c.connect, c)
        if not cok or not connected then
            self.stat.connfails = self.stat.connfails + 1
            if self.closed then
                self:_discard(c)
            else
                self:_push_idle(c, self:_cool_after_fail(c))
            end
            return nil, "connect failed"
        end
        self.stat.connects = self.stat.connects + 1
    end
    self:_born(c, srey.timer_ms())
    return c
end

---借出超过 leak_warn 还没还的，每次借出告警一次
function M:_leak_check()
    if self.leak_warn <= 0 then
        return
    end
    local now = srey.timer_ms()
    for c, t in pairs(self.out_at) do
        if not self.warned[c] and now - t >= self.leak_warn then
            self.warned[c] = true
            self.stat.leaks = self.stat.leaks + 1
            WARN("%s: connection checked out %dms ago, not returned yet.", self.name, now - t)
        end
    end
end

---连着的空闲连接不足 min 时补上：先用断开的空闲连接重连，没有就新建；建好走归还路径（有人等就直接交接）。
---一轮最多建 max 次：寿命极短时建好的归还即到期丢掉、又空出名额，不设上限会一直建
function M:_fill_min()
    local n = 0
    while not self.closed and n < self.max and self:_ready_idle() < self.min do
        n = n + 1
        local c = self:_take_cold()
        local got
        if c then
            got = self:_checkout(c)
        elseif self.total < self.max then
            got = self:_new_conn()
        end
        if not got then
            self:_wake_on_fail()
            return
        end
        if self.closed then
            self:_discard(got)
            return
        end
        self:_return(got)
    end
end

---维护协程：定时收缩空闲、查借出过久、补足 min；free 叫醒它就退出
function M:_tick_loop()
    while not self.closed do
        srey._coro_wait(self.tick_sess, TAG_DB_POOL_TICK, TICK_MS)
        if self.closed then
            break
        end
        self:_leak_check()
        self:_maybe_shrink()
        self:_fill_min()
    end
    self.ticking = false
end

---用到收缩、补 min 或借出过久告警时起维护协程
function M:_tick_start()
    if self.ticking or (self.idle_timeout <= 0 and self.min <= 0 and self.leak_warn <= 0) then
        return
    end
    self.ticking = true
    srey.fork(self._tick_loop, self)
end

---挂起等一条连接：有人归还就直接交接过来；有冷却中的，最晚等到最早那条冷却结束就回来重试
---@param deadline integer 调用方期限（毫秒时刻），0=一直等
---@param now integer 当前毫秒，与本轮 _take_usable 用的同一个
---@return DbPoolConn? conn 交接过来的连接
---@return string? err 池关闭、超时或全在冷却；两个都是 nil 表示回去重查
function M:_wait_conn(deadline, now)
    local wait_ms = 0
    if deadline > 0 then
        if now >= deadline then
            self.stat.timeouts = self.stat.timeouts + 1
            return nil, "get timeout"
        end
        wait_ms = deadline - now
    end
    -- 没有借出的连接、空闲的又全在冷却：等也没人来叫醒，快速失败
    if 0 == self.nbusy then
        return nil, "all cooling"
    end
    local cool = self:_cool_wait(now)
    if cool > 0 and (0 == wait_ms or cool < wait_ms) then
        wait_ms = cool
    end
    self.waiters = self.waiters + 1
    local msg = srey._coro_wait(self.sess, TAG_DB_POOL, wait_ms)
    self.waiters = self.waiters - 1
    if self.closed then
        if msg and msg.conn then
            self:_discard(msg.conn)
        end
        return nil, "pool closed"
    end
    if msg and msg.conn then
        return msg.conn
    end
    -- 超时消息或叫醒重试：回去再看一次空闲栈，期限到了由下一轮开头报超时
    return nil, nil
end

---借一条连接。没有空闲时挂起当前协程，直到有人归还、冷却到期、池关闭或超时；
---没有借出中的连接、空闲的又全在冷却时不等，立刻失败
---@param timeout integer? 等连接毫秒数；nil 用构造时的 get_timeout，0=一直等
---@return DbPoolConn? conn 借到的连接
---@return string? err 失败原因
function M:get(timeout)
    if nil ~= timeout and ("number" ~= type(timeout) or timeout < 0) then
        error("db_pool_ctx:get: timeout must be a non-negative number", 2)
    end
    srey._check_wait("db_pool_ctx:get")
    if self.closed then
        return nil, "pool closed"
    end
    self:_tick_start()
    local ms = timeout
    if nil == ms then
        ms = self.get_timeout
    end
    local deadline = 0
    if ms and ms > 0 then
        deadline = srey.timer_ms() + ms
    end
    while true do
        local now = srey.timer_ms()
        local c = self:_take_usable(now)
        if c then
            local got, err = self:_checkout(c)
            if got then
                return self:_hand_out(got)
            end
            if self.closed or self:_wake_on_fail() then
                return nil, err
            end
        elseif self.total < self.max then
            -- 先看还能不能新建：冷却中的空闲连接不该占着 max 的名额不让建
            local nconn, err = self:_new_conn()
            if nconn then
                if self.closed then
                    return self:_closed_drop(nconn)
                end
                return self:_hand_out(nconn)
            end
            -- 建连失败的已留在空闲栈里冷却。还有借出中的就回去重查（每次建连都会挂起，至多 max 次就到排队），
            -- 其余失败直接返回
            if self.closed or "connect failed" ~= err or 0 == self.nbusy then
                self:_wake_on_fail()
                return nil, err
            end
        else
            local got, err = self:_wait_conn(deadline, now)
            if got then
                return self:_hand_out(got)
            end
            if err then
                return nil, err
            end
        end
    end
end

---丢掉一条不能再交给别人的借出连接，空出名额叫醒队头自己新建，别让它等到别人归还或超时
---@param c DbPoolConn 连接对象
function M:_discard_wake(c)
    self:_discard(c)
    if self.waiters > 0 then
        srey._coro_wake(self.sess, TAG_DB_POOL, WAKE_RETRY)
    end
end

---归还的实现，put 与维护协程补 min 共用
---@param c DbPoolConn 借出中的连接
function M:_return(c)
    self.out_at[c] = nil
    self.warned[c] = nil
    if self.closed then
        self:_discard(c)
        return
    end
    if c.established and self:_expired(c, srey.timer_ms()) then
        self:_discard_wake(c)
        return
    end
    if c.established and c.reset_session then
        local rok, clean = srey.xpcall(c.reset_session, c)
        if self.closed then
            self:_discard(c)
            return
        end
        if not rok or not clean then
            self:_discard_wake(c)-- 清不干净就不再交给别人
            return
        end
    end
    if not c.established then
        self:_push_idle(c, nil)
        -- 空闲栈多了一条待重连的：叫醒队头来重试；它再失败且没有借出时由 get 叫醒其余的
        if self.waiters > 0 then
            srey._coro_wake(self.sess, TAG_DB_POOL, WAKE_RETRY)
        end
        return
    end
    -- 直接交接给队头；busy 保持标记，对方醒来时已经算借出
    if srey._coro_wake(self.sess, TAG_DB_POOL, { conn = c }) then
        return
    end
    self:_push_idle(c, nil)
end

---归还连接。借出期间断了也照常还，下一次借出会自动重连；池关闭后归还的连接会被断开。
---连接对象带 reset_session 时先清会话状态再交给下一个人，期间可能挂起
---@param c DbPoolConn db_pool_get 返回的连接
function M:put(c)
    if not self.busy[c] then
        error("db_pool_ctx: put a connection not checked out", 2)
    end
    srey._check_wait("db_pool_ctx:put")
    self.stat.puts = self.stat.puts + 1
    self:_return(c)
end

---预热：建连接直到空闲栈有 n 条（冷却中 / 断开的也算），不超过 max；n 默认 min
---@param n integer? 建几条
---@return boolean ok
---@return string? err
function M:warm(n)
    if self.closed then
        return false, "pool closed"
    end
    n = n or self.min
    if "number" ~= type(n) or n < 0 or n % 1 ~= 0 then
        return false, "invalid warm count"
    end
    if n > self.max then
        return false, "warm count exceeds max"
    end
    while #self.idle < n and self.total < self.max do
        local c, err = self:_new_conn()
        if not c then
            return false, err
        end
        if self.closed then
            self:_discard(c)
            return false, "pool closed"
        end
        self:_push_idle(c, nil)
    end
    return #self.idle >= n
end

-- with 的收尾：先归还，再原样转发 fn 的返回值或错误
local function _with_ret(self, c, ok, ...)
    self:put(c)
    if not ok then
        return nil, ...
    end
    return ...
end

---借一条连接执行 fn，结束自动归还；fn 抛错也归还
---@param fn fun(c:DbPoolConn):any 业务函数
---@return any ... fn 的返回值；fn 抛错时返回 nil, err
function M:with(fn)
    local c, err = self:get()
    if not c then
        return nil, err
    end
    return _with_ret(self, c, srey.xpcall(fn, c))
end

---当前计数快照
---@return DbPoolStat stat
function M:stat_get()
    return {
        total = self.total,
        idle = #self.idle,
        busy = self.nbusy,
        waiters = self.waiters,
        connects = self.stat.connects,
        connfails = self.stat.connfails,
        pings = self.stat.pings,
        timeouts = self.stat.timeouts,
        gets = self.stat.gets,
        puts = self.stat.puts,
        leaks = self.stat.leaks,
    }
end

---关闭池：唤醒所有等待者与维护协程，断开空闲连接；还有借出的连接，等 put 时断开。
---断开会挂起，须在协程里调（见文件头）
function M:free()
    if self.closed then
        return
    end
    srey._check_wait("db_pool_ctx:free")
    self.closed = true
    srey._coro_wake_all(self.sess, TAG_DB_POOL, WAKE_RETRY)
    srey._coro_wake_all(self.tick_sess, TAG_DB_POOL_TICK, WAKE_RETRY)
    local idle = self.idle
    self.idle = {}
    for i = 1, #idle do
        self:_drop(idle[i])
    end
end

return M
