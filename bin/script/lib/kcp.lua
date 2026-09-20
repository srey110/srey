-- KCP 会话封装:class 包住 C 层 userdata 句柄(lkcp.c),new 时 kcp_init 一次,后续方法复用同一句柄。
-- 数据到达以创建时所在 task 为目标推送(MSG_TYPE.RECVFROM);ikcp_update 由 event 线程 tick 自动驱动。
-- socket 用 srey.udp(PACK_TYPE.UDP_KCP) 创建,数据接收复用 srey.on_recvedfrom。
-- 使用方:local kcp = require("lib.kcp"); local k = kcp.new(sk, conv); k:start(ip, port)
--        第 4 参数是 sync(boolean),传 true 则 start/send 同步等响应;缺省只走异步。
--        注意 Lua 中 0 为真值,旧写法 kcp.new(sk, conv, 0) 会被当成 sync=true
local srey = require("lib.srey")
local ckcp = require("srey.kcp")
local ctx = class("kcp_ctx")

-- 下面各字段的取值域由绑定层校验,越界即报错并点出字段名。落在域内的值仍可能被库调整,见各字段
---@class kcp_config
---@field nodelay integer? 0 普通 / 1 nodelay;缺省不改。取 [-1, 1]
---@field interval integer? flush 间隔 ms(库钳到 10~5000);缺省不改。取 [-1, 5000]
---@field resend integer? 快速重传阈值(典型 2);缺省不改。取 [-1, INT32_MAX]
---@field nc integer? 0 开流控 / 1 关流控;缺省不改。取 [-1, 1]
---@field sndwnd integer? 发送窗口(默认 32);缺省不改。取 [0, 65535]
---@field rcvwnd integer? 接收窗口(默认 128,填 1~127 会被库抬到 128);缺省不改。取 [0, 65535]
---@field mtu integer? MTU(默认 1400,库只认 50~65535);缺省不改。取 [0, 65535]

---绑定底层 UDP socket 与会话号(不建立会话,需再调 start)
---@param sk userdata 连接标识
---@param conv integer 会话号(同一 socket 内唯一,两端约定一致)
---@param sync boolean? true=start/send 同步等待响应,缺省=只走异步
function ctx:ctor(sk, conv, sync)
    self.sync = sync and true or false
    self.sess = 0
    self.kcp = ckcp.new(sk, conv)
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
    -- 失败分支 stop 之前先认一次 sess:挂起期间别的协程可能 stop + 重启换上新会话(它那次 stop
    -- 把 self.sess 清成 0,正好放开上面那道守卫),认不上就说明现在这条是别人的,停它就是误伤。
    -- 自己那条旧会话此时已无法再定位,只能等 socket 关闭时回收——总好过抹掉别人刚建好的
    if srey.MSG_TYPE.TIMEOUT == msg.mtype then
        if sess == self.sess then
            self:stop()
        end
        return false
    end
    if srey.MSG_TYPE.CLOSE == msg.mtype
        or ERR_OK ~= msg.erro then
        -- 走 stop 而不是只清 sess:C 侧 kcp_start 已把 stopped 置 0,留在 0 则 handle/send 绕过守卫
        -- 投到不存在的会话,被静默丢弃却返成功。占位条目由 _kcp_start 补发的那条合成 CLOSE 清
        -- (NEVERCONN,不触发 on_closed 观察者),kcp_stop 解析不到会话不会再补一条
        if sess == self.sess then
            self:stop()
        end
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
---@param handle integer|string 目标 task：字符串按名字查，整数按句柄直取(srey.task_handle 取)
---@return boolean ok 目标不存在(名字未注册 / 数字句柄对应 task 已退出)或会话已 stop 时 false。
--- 仅保证调用时目标存在:目标若在此之后退出,该会话的消息会被静默丢弃。探测口径同 srey.sock_bind_task
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
    -- 捏住 sess 而不是等待与回写各读一次 self.sess:挂起期间它可能被别的协程换掉，
    -- 按新值等待就等错了会话，按新值回写更会抹掉别人刚建好的那条。与 C 侧 kcp_synsend 同形
    local sess = self.sess
    if 0 == sess then
        srey._ud_free_copy(data, copy)
        return nil
    end
    if not self.kcp:send(data, size, copy) then
        return nil
    end
    local msg = srey._coro_wait(sess, srey.MSG_TYPE.RECVFROM, srey.get_netread_timeout())
    if srey.MSG_TYPE.TIMEOUT == msg.mtype then
        if sess == self.sess then
            self:stop()
        end
        return nil
    end
    if srey.MSG_TYPE.CLOSE == msg.mtype then
        -- 会话已在 C 层拆除,coro_sess 条目也已由 _net_close_dispatch 清掉(waiters 摘空后 del_empty);
        -- 但 self.sess 仍是旧值,不清则下次 send 会通过守卫、投到已消失的会话被 _kcp_resolve 静默丢弃,
        -- 而 kcp_send 返 ERR_OK 让 Lua 以为发送成功,继而空等满一个 netread 超时
        if sess == self.sess then
            self.sess = 0
        end
        return nil
    end
    return msg.udata, msg.size
end

return ctx
