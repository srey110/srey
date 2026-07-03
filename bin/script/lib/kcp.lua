-- KCP 会话封装:class 包住 C 层 userdata 句柄(lkcp.c),new 时 kcp_init 一次,后续方法复用同一句柄。
-- 数据到达以创建时所在 task 为目标推送(MSG_TYPE.RECVFROM);ikcp_update 由 event 线程 tick 自动驱动。
-- socket 用 srey.udp(PACK_TYPE.UDP_KCP) 创建,数据接收复用 srey.on_recvedfrom。
-- 使用方:local kcp = require("lib.kcp"); local k = kcp.new(fd, skid, conv, sess); k:start(ip, port)
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
---@param sess integer? 唤醒 sess,由调用方生成(如 srey.id());0 或缺省=send 只走异步,非 0=send 同步等待响应
function ctx:ctor(fd, skid, conv, sess)
    self.sess = sess or 0
    self.kcp = ckcp.new(fd, skid, conv, self.sess)
end

---建立会话:数据到达以当前 task 为推送目标;sess(ctor 传入)为 0 时异步发起(不等结果),
---非 0 时同步等待 event 线程实际建会话完成(或因 conv 冲突失败)后返回
---@param ip string 对端 IP
---@param port integer 对端端口
---@param config kcp_config? KCP 可调参数,缺省用库默认
---@return boolean ok
function ctx:start(ip, port, config)
    if 0 == self.sess then
        return self.kcp:start(ip, port, config)
    end
    if not self.kcp:start(ip, port, config) then
        return false
    end
    local msg = srey._coro_wait(false, self.sess, srey.MSG_TYPE.HANDSHAKED, srey.get_netread_timeout())
    if srey.MSG_TYPE.TIMEOUT == msg.mtype then
        self.kcp:stop()
        return false
    end
    if srey.MSG_TYPE.CLOSE == msg.mtype then
        return false
    end
    return ERR_OK == msg.erro
end

---停止并释放会话(从会话表移除;之后需重新 start 才能再用)
function ctx:stop()
    self.kcp:stop()
end

---变更数据推送目标 task
---@param handle integer 目标 task handle(srey.task_handle 取)
---@return boolean ok
function ctx:handle(handle)
    return self.kcp:handle(handle)
end

---发送数据;sess(ctor 传入)为 0 时异步发送(不等响应),非 0 时同步发送并等待响应
---(响应由对端回包按 sess 唤醒本协程,同一会话上的多次 send 按 FIFO 排队唤醒,而非互相覆盖)
---@param data string|lightuserdata 数据
---@param size integer? data 为 lightuserdata 时必填
---@param copy integer 1=复制;0=转移所有权
---@return boolean|lightuserdata|nil ok_or_rdata sess==0:发送是否成功;sess~=0:响应数据指针(仅本协程下次 yield 前有效),超时/失败为 nil
---@return integer? rsize sess~=0 时的响应长度
function ctx:send(data, size, copy)
    if 0 == self.sess then
        return self.kcp:send(data, size, copy)
    end
    if not self.kcp:send(data, size, copy) then
        return nil
    end
    local msg = srey._coro_wait(false, self.sess, srey.MSG_TYPE.RECVFROM, srey.get_netread_timeout())
    if srey.MSG_TYPE.TIMEOUT == msg.mtype then
        self.kcp:stop()
        return nil
    end
    if srey.MSG_TYPE.CLOSE == msg.mtype then
        return nil
    end
    return msg.udata, msg.size
end

return ctx
