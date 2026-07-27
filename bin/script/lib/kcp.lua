-- KCP 会话封装:class 包住 C 层 userdata 句柄(lkcp.c),new 时 kcp_init 一次,后续方法复用同一句柄。
-- 数据到达以创建时所在 task 为目标推送(MSG_TYPE.RECVFROM);ikcp_update 由 event 线程 tick 自动驱动。
-- socket 用 srey.udp(PACK_TYPE.UDP_KCP) 创建,数据接收复用 srey.on_recvedfrom。
-- 使用方:local kcp = require("lib.kcp"); local k = kcp.new(fd, skid, conv); k:start(ip, port)
--        第 4 参数是 sync(boolean),传 true 则 start/send 同步等响应;缺省只走异步。
--        注意 Lua 中 0 为真值,旧写法 kcp.new(fd, skid, conv, 0) 会被当成 sync=true
local srey = require("lib.srey")
local ckcp = require("kcp")

local ctx = class("kcp_ctx")

---@class kcp_config
---@field nodelay integer? 0 普通 / 1 nodelay;缺省不改
---@field interval integer? flush 间隔 ms;缺省不改
---@field resend integer? 快速重传阈值;缺省不改
---@field nc integer? 0 开流控 / 1 关流控;缺省不改
---@field sndwnd integer? 发送窗口;缺省不改
---@field rcvwnd integer? 接收窗口;缺省不改
---@field mtu integer? MTU;缺省不改

---绑定底层 UDP socket 与会话号(不建立会话,需再调 start)
---@param fd integer UDP socket fd
---@param skid integer 连接 skid
---@param conv integer 会话号(同一 socket 内唯一,两端约定一致)
---@param sync boolean? true=start/send 同步等待响应,缺省=只走异步
function ctx:ctor(fd, skid, conv, sync)
    self.sync = sync and true or false
    self.sess = 0
    self.kcp = ckcp.new(fd, skid, conv)
end

---建立会话:数据到达以当前 task 为推送目标;ctor 未传 sync 时异步发起(不等结果),
---sync 时同步等待 event 线程实际建会话完成(或因 conv 冲突失败)后返回。
---每次调用都生成新 sess,故 stop 后重启不会被上一会话在途的 CLOSE 击穿。
---sync 模式下会话存活期间重复调用直接返回 false,须先 stop
---@param ip string 对端 IP
---@param port integer 对端端口
---@param config kcp_config? KCP 可调参数,缺省用库默认
---@return boolean ok
function ctx:start(ip, port, config)
    if not self.sync then
        return self.kcp:start(0, ip, port, config)
    end
    -- sync 模式自己记着 sess,故存活期间重复 start 一律在 Lua 侧挡掉:C 侧虽会隐式 stop 旧会话再起
    -- (不产生 stop 不掉的孤儿),但那会静默丢掉本对象正在等的会话,不如让调用方显式 stop
    if 0 ~= self.sess then
        return false
    end
    local sess = srey.id()
    if not self.kcp:start(sess, ip, port, config) then
        return false
    end
    self.sess = sess
    local msg = srey._coro_wait(sess, srey.MSG_TYPE.HANDSHAKED, srey.get_netread_timeout())
    if srey.MSG_TYPE.TIMEOUT == msg.mtype then
        self:stop()
        return false
    end
    if srey.MSG_TYPE.CLOSE == msg.mtype
        or ERR_OK ~= msg.erro then
        -- 占位条目由 _kcp_start 失败时补发的合成 CLOSE 清(erro != ERR_OK,不触发 on_close 观察者)
        self.sess = 0
        return false
    end
    return true
end

---停止并释放会话(从会话表移除;之后需重新 start 才能再用)
function ctx:stop()
    self.kcp:stop()
    self.sess = 0-- 占位条目由 kcp_stop 触发的 CLOSE 清
end

---变更数据推送目标 task
---@param handle integer 目标 task handle(srey.task_handle 取)
---@return boolean ok
function ctx:handle(handle)
    return self.kcp:handle(handle)
end

---发送数据;ctor 未传 sync 时异步发送(不等响应),sync 时同步发送并等待响应
---(响应由对端回包按 start 生成的 sess 唤醒本协程,同一会话上的多次 send 按 FIFO 排队唤醒,而非互相覆盖)
---@param data string|lightuserdata 数据
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer 1=复制;0=转移所有权
---@return boolean|lightuserdata|nil ok_or_rdata 异步:发送是否成功;同步:响应数据指针(仅本协程下次 yield 前有效),超时/失败/会话未建立为 nil
---@return integer? rsize 同步模式下的响应长度
function ctx:send(data, size, copy)
    if not self.sync then
        return self.kcp:send(data, size, copy)
    end
    if 0 == self.sess then
        srey._ud_free_copy(data, copy)
        return nil
    end
    if not self.kcp:send(data, size, copy) then
        return nil
    end
    local msg = srey._coro_wait(self.sess, srey.MSG_TYPE.RECVFROM, srey.get_netread_timeout())
    if srey.MSG_TYPE.TIMEOUT == msg.mtype then
        self:stop()
        return nil
    end
    if srey.MSG_TYPE.CLOSE == msg.mtype then
        -- 会话已在 C 层拆除,coro_sess 条目也已由 _net_close_dispatch 清掉(waiters 摘空后 del_empty);
        -- 但 self.sess 仍是旧值,不清则下次 send 会通过守卫、投到已消失的会话被 _kcp_resolve 静默丢弃,
        -- 而 kcp_send 返 ERR_OK 让 Lua 以为发送成功,继而空等满一个 netread 超时
        self.sess = 0
        return nil
    end
    return msg.udata, msg.size
end

return ctx
