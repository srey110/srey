-- SMTP 客户端（smtp_ctx 类）。
-- 封装 C 层 srey.smtp，实现完整的 SMTP 对话流程：
--   connect → [AUTH] → MAIL FROM → RCPT TO × N → DATA → 邮件正文 → QUIT
-- 支持隐式 TLS（SMTPS，连接即握手；协议层无 STARTTLS 升级），keepalive 通过 ping/reset 维护长连接。

local srey = require("lib.srey")
local smtp = require("srey.smtp")
local pub  = require("lib.conn_pub")-- connect / ping / quit 的共用骨架

-- RCPT TO 的合法应答：250 已接受、251 已接受但将转发（RFC 5321 §4.3.2）。
-- 判 251 为失败会让 DATA 从不发出、整封信报错，而收件人其实已被服务端接受。
-- 其余命令都是单码（MAIL FROM / 正文 / RSET / NOOP 判 250，DATA 判 354，QUIT 判 221），
-- 直接走 check_ok / check_code。与 C 侧 coro_utils.c 的四张 SMTP_CODE_* 表同源
local RCPT_CODES = { "250", "251" }

-- smtp_ctx：SMTP 连接上下文。
-- 每个实例对应一条到 SMTP 服务器的持久连接。
-- 建链、保活、断开三段继承自 conn_pub，本文件只实现 _connect / _ping / _doquit 三个钩子。
local ctx = class("smtp_ctx", pub)

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param user string AUTH 用户名（空时跳过认证）
---@param password string AUTH 密码
function ctx:ctor(ip, port, sslname, user, password)
    local ssl = pub.ssl(sslname)
    self.smtp = smtp.new(ip, port, ssl, user, password)
    -- ip / user / password 任一超出 C 侧字段容量时 smtp.new 返回 nil（不静默截断——
    -- 截断后的密码拿去认证只换回服务端一句 535，调用方看不出是自己传长了）
    if not self.smtp then
        error("smtp.new failed: ip / user / password too long", 2)
    end
    self.sslname = sslname
    -- 一封邮件是 MAIL FROM → N×RCPT TO → DATA → 正文 → RSET 一长串往返，两个协程
    -- 同时发信会把收件人混到一起——串行化执行器由 conn_pub 建
    pub.init(self, self.smtp)
end

-- conn_pub 的建链钩子：TCP 连接 + SMTP 握手（等待 220 欢迎行及 AUTH 协商）
function ctx:_connect()
    if not self.smtp:try_connect() then
        return false
    end
    local sk = self.smtp:sock_id()
    if not srey.wait_connect(sk, SSL_NAME.NONE ~= self.sslname or nil) then
        return false
    end
    local ok, err, elens = srey.wait_handshaked(sk)
    if not ok and err then
        WARN("%s", srey.ud_str(err, elens))
    end
    return ok
end

---发送 RSET 命令重置服务端会话状态（不关闭连接），用于复用连接发送下一封邮件
---@return boolean ok 服务端返回 250 时 true（check_ok 只认这一个码，不是判整个 2xx 段）
function ctx:reset()
    return srey.serial_ret(false, self.serial(self._reset, self))
end
-- "一条命令、一个往返、应答判 250" 的固定形状，_reset 与 _ping 共用。
-- RCPT 是文件头说的那个例外（多收件人各判一次），不走这里
---@param packer fun(smtp:userdata):lightuserdata,integer 组包方法，如 smtp.pack_reset
---@return boolean ok 应答为 250 时 true
function ctx:_cmd_ok(packer)
    local sk = self.smtp:sock_id()
    local cmd, csize = packer(self.smtp)
    local pack = srey.syn_send(sk, cmd, csize, 0)
    if nil == pack then
        return false
    end
    return self.smtp:check_ok(pack)
end

-- RSET 清的是服务端会话状态，插进别人半途的信封里会把它的收件人清掉，
-- 之后那封信的 DATA 会被回 503 而不是 354，静默发不出去
function ctx:_reset()
    return self:_cmd_ok(self.smtp.pack_reset)
end

-- conn_pub 的探活钩子：NOOP，服务端返回 250 即存活（check_ok 只认这一个码）
function ctx:_ping()
    return self:_cmd_ok(self.smtp.pack_ping)
end

---内部邮件发送流程（不含 reset）：MAIL FROM → RCPT TO × N → DATA(354) → MIME 正文；任一步失败即返回
---@param mail any mail_ctx 邮件对象
---@return boolean ok 每步应答码都符合预期时 true（MAIL/RCPT/正文判 250，DATA 判 354）
function ctx:_send(mail)
    local sk = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_from(mail:from_get())
    if not cmd then
        return false
    end
    local pack = srey.syn_send(sk, cmd, csize, 0)
    if nil == pack or not self.smtp:check_ok(pack)  then
        return false
    end
    for _, addr in ipairs(mail:addrs_get()) do
        cmd, csize = self.smtp:pack_rcpt(addr)
        if not cmd then
            return false
        end
        pack = srey.syn_send(sk, cmd, csize, 0)
        if nil == pack or not self.smtp:check_codes(pack, RCPT_CODES) then
            return false
        end
    end
    cmd, csize = self.smtp:pack_data()
    pack = srey.syn_send(sk, cmd, csize, 0)
    if nil == pack or not self.smtp:check_code(pack, "354") then
        return false
    end
    cmd, csize = mail:pack()
    -- 取不到熵生成 MIME boundary 时 pack 返 nil。这里已经收过 354、连接处于 DATA 态，
    -- 后续 _reset 的 RSET 会被当成正文行、等不到响应而超时断连——与 C 侧 _smtp_send 同样处理，
    -- 系统随机源坏掉时丢一条连接是可接受的降级
    if nil == cmd then
        return false
    end
    pack = srey.syn_send(sk, cmd, csize, 0)
    if nil == pack or not self.smtp:check_ok(pack) then
        return false
    end
    return true
end

---发送邮件：_send 后无论成败都执行 reset，保证服务端状态干净以便复用连接；
---reset 失败即就地拆掉连接（代次前进、established 清零），下一封信要么先 ping() 重连要么直接失败
---@param mail any mail_ctx 邮件对象
---@return boolean ok 邮件投递成功 true。只反映这封邮件的成败，不反映连接状态——
---投递成功而收尾 reset 失败时连接已被拆掉，本次仍返 true
function ctx:send(mail)
    return srey.serial_ret(false, self.serial(self._sendmail, self, mail))
end
-- 锁覆盖 _send + reset 整段：RSET 清的是本次投递在服务端留下的会话状态，与发送是同一笔事。
-- 分开各包一次的话，别人的 MAIL FROM 会挤在中间被我们的 RSET 清掉
function ctx:_sendmail(mail)
    local rtn = self:_send(mail)
    -- 走 _closereset 而不是自己 sync_close：代次与 established 由 conn_pub 统一维护，
    -- 漏掉后者会让排队中的 connect 对着这条已关的连接报成功
    if not self:_reset() then
        self:_closereset()
    end
    return rtn
end

---内部 QUIT 命令（不关闭 socket，供 quit 调用）
---@param sk userdata 连接标识
function ctx:_quit(sk)
    local cmd, csize = self.smtp:pack_quit()
    srey.syn_send(sk, cmd, csize, 0)
end

-- conn_pub 的断开钩子：QUIT 要等服务端 221，等完再关 TCP
function ctx:_doquit()
    local sk = self.smtp:sock_id()
    if not sk.valid then
        return
    end
    self:_quit(sk)
    sk = self.smtp:sock_id()
    if not sk.valid then
        return
    end
    srey.sync_close(sk)
end

return ctx
