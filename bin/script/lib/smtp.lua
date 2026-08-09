-- SMTP 客户端（smtp_ctx 类）。
-- 封装 C 层 srey.smtp，实现完整的 SMTP 对话流程：
--   connect → [AUTH] → MAIL FROM → RCPT TO × N → DATA → 邮件正文 → QUIT
-- 支持 TLS（SMTPS 或 STARTTLS），keepalive 通过 ping/reset 维护长连接。

local srey = require("lib.srey")
local smtp = require("srey.smtp")
local pub  = require("lib.conn_pub")-- connect / ping / quit 的共用骨架

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
    local ok, ssl = srey.ssl_qury(sslname)
    if not ok then
        error(string.format("ssl_qury not find ssl name %s", sslname), 2)
    end
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
    local fd, skid = self.smtp:sock_id()
    if not srey.wait_connect(fd, skid, SSL_NAME.NONE ~= self.sslname or nil) then
        return false
    end
    local ok, err, elens = srey.wait_handshaked(fd, skid)
    if not ok and err then
        WARN("%s", srey.ud_str(err, elens))
    end
    return ok
end

---发送 RSET 命令重置服务端会话状态（不关闭连接），用于复用连接发送下一封邮件
---@return boolean ok 服务端返回 2xx 时 true
function ctx:reset()
    return srey.serial_ret(false, self.serial(self._reset, self))
end
-- RSET 清的是服务端会话状态，插进别人半途的信封里会把它的收件人清掉，
-- 之后那封信的 DATA 会被回 503 而不是 354，静默发不出去
function ctx:_reset()
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_reset()
    local pack =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    return self.smtp:check_ok(pack)
end

-- conn_pub 的探活钩子：NOOP，服务端返回 2xx 即存活
function ctx:_ping()
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_ping()
    local pack =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    return self.smtp:check_ok(pack)
end

---内部邮件发送流程（不含 reset）：MAIL FROM → RCPT TO × N → DATA(354) → MIME 正文；任一步失败即返回
---@param mail any mail_ctx 邮件对象
---@return boolean ok 整个流程 2xx 通过时 true
function ctx:_send(mail)
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_from(mail:from_get())
    if not cmd then
        return false
    end
    local pack =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack or not self.smtp:check_ok(pack)  then
        return false
    end
    for _, addr in ipairs(mail:addrs_get()) do
        cmd, csize = self.smtp:pack_rcpt(addr)
        if not cmd then
            return false
        end
        pack =  srey.syn_send(fd, skid, cmd, csize, 0)
        if nil == pack or not self.smtp:check_ok(pack) then
            return false
        end
    end
    cmd, csize = self.smtp:pack_data()
    pack =  srey.syn_send(fd, skid, cmd, csize, 0)
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
    pack =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack or not self.smtp:check_ok(pack) then
        return false
    end
    return true
end

---发送邮件：_send 后无论成败都执行 reset，保证服务端状态干净以便复用连接；reset 失败说明连接已断，立即关闭
---@param mail any mail_ctx 邮件对象
---@return boolean ok 发送成功 true
function ctx:send(mail)
    return srey.serial_ret(false, self.serial(self._sendmail, self, mail))
end
-- 锁覆盖 _send + reset 整段：RSET 清的是本次投递在服务端留下的会话状态，与发送是同一笔事。
-- 分开各包一次的话，别人的 MAIL FROM 会挤在中间被我们的 RSET 清掉
function ctx:_sendmail(mail)
    local rtn = self:_send(mail)
    if not self:_reset() then
        local fd, skid = self.smtp:sock_id()
        if INVALID_SOCK ~= fd then
            srey.sync_close(fd, skid, 1)
        end
    end
    return rtn
end

---内部 QUIT 命令（不关闭 socket，供 quit 调用）
---@param fd integer socket fd
---@param skid integer 连接 skid
function ctx:_quit(fd, skid)
    local cmd, csize = self.smtp:pack_quit()
    srey.syn_send(fd, skid, cmd, csize, 0)
end

-- conn_pub 的断开钩子：QUIT 要等服务端 221，等完再关 TCP
function ctx:_doquit()
    local fd, skid = self.smtp:sock_id()
    if INVALID_SOCK == fd then
        return
    end
    self:_quit(fd, skid)
    fd, skid = self.smtp:sock_id()
    if INVALID_SOCK == fd then
        return
    end
    srey.sync_close(fd, skid)
end

return ctx
