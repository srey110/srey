-- SMTP 客户端（smtp_ctx 类）。
-- 封装 C 层 srey.smtp，实现完整的 SMTP 对话流程：
--   connect → [AUTH] → MAIL FROM → RCPT TO × N → DATA → 邮件正文 → QUIT
-- 支持 TLS（SMTPS 或 STARTTLS），keepalive 通过 ping/reset 维护长连接。

local srey = require("lib.srey")
local smtp = require("srey.smtp")
local core = require("srey.core")

-- smtp_ctx：SMTP 连接上下文。
-- 每个实例对应一条到 SMTP 服务器的持久连接。
local ctx = class("smtp_ctx")

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param user string AUTH 用户名（空时跳过认证）
---@param password string AUTH 密码
function ctx:ctor(ip, port, sslname, user, password)
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
    end
    self.smtp = smtp.new(ip, port, ssl, user, password)
end

---建立 TCP 连接并完成 SMTP 握手（等待 220 欢迎行及 AUTH 协商）；成功后 skid 设为 socket 会话键
---@return boolean ok 握手成功 true，失败 false
function ctx:connect()
    if not self.smtp:try_connect() then
        return false
    end
    local fd, skid = self.smtp:sock_id()
    local ok, err, elens = srey.wait_handshaked(fd, skid)
    if ok then
        srey.sock_session(fd, skid, skid)
    elseif err then
        WARN("%s", srey.ud_str(err, elens))
    end
    return ok
end

---发送 RSET 命令重置服务端会话状态（不关闭连接），用于复用连接发送下一封邮件
---@return boolean ok 服务端返回 2xx 时 true
function ctx:reset()
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_reset()
    local pack =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    return self.smtp:check_ok(pack)
end

---发送 NOOP 探测连接是否存活（内部使用）
---@return boolean ok 服务端返回 2xx 时 true
function ctx:_ping()
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_ping()
    local pack =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    return self.smtp:check_ok(pack)
end

---连接保活检测：NOOP 失败时自动重连，建议在每次发送邮件前调用
---@return boolean ok 连接可用 true，否则 false
function ctx:ping()
    if not self:_ping() then
        local fd, skid = self.smtp:sock_id()
        srey.sync_close(fd, skid, 1)
        return self:connect()
    end
    return true
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
    pack =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack or not self.smtp:check_ok(pack) then
        return false
    end
    return true
end

---发送邮件：_send 后无论成败都执行 reset，保证服务端状态干净以便复用连接
---@param mail any mail_ctx 邮件对象
---@return boolean ok 发送成功 true
function ctx:send(mail)
    local rtn = self:_send(mail)
    self:reset()
    return rtn
end

---内部 QUIT 命令（不关闭 socket，供 quit 调用）
---@param fd integer socket fd
---@param skid integer 连接 skid
function ctx:_quit(fd, skid)
    local cmd, csize = self.smtp:pack_quit()
    srey.syn_send(fd, skid, cmd, csize, 0)
end

---优雅关闭：先发 QUIT 等待服务端确认，再关闭 TCP 连接
function ctx:quit()
    local fd, skid = self.smtp:sock_id()
    if INVALID_SOCK == fd then
        return
    end
    self:_quit(fd, skid)
    srey.sync_close(fd, skid)
end

return ctx
