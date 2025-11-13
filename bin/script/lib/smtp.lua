local srey = require("lib.srey")
local smtp = require("srey.smtp")
local core = require("srey.core")

local ctx = class("smtp_ctx")
function ctx:ctor(ip, port, sslname, user, password)
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
    end
    self.smtp = smtp.new(ip, port, ssl, user, password)
end
function ctx:connect()
    if not self.smtp:try_connect() then
        return false
    end
    local fd, skid = self.smtp:sock_id()
    local ok,_,_ = srey.wait_handshaked(fd, skid)
    return ok
end
function ctx:reset()
    if not self.smtp:check_auth() then
        return false
    end
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_reset()
    if not cmd then
        return false
    end
    local pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    return self.smtp:check_ok(pack)
end
function ctx:_ping()
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_ping()
    if not cmd then
        return false
    end
    local pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    return self.smtp:check_ok(pack)
end
function ctx:ping()
    if not self:_ping() then
        return self:connect()
    end
    return true
end
function ctx:_send(from, rcpt, subject, data)
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_from(from)
    if not cmd then
        return false
    end
    local pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    if not self.smtp:check_ok(pack) then
        return false
    end
    cmd, csize = self.smtp:pack_rcpt(rcpt)
    if not cmd then
        return false
    end
    pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    if not self.smtp:check_ok(pack) then
        return false
    end
    cmd, csize = self.smtp:pack_data()
    if not cmd then
        return false
    end
    pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    if not self.smtp:check_code(pack, "354") then
        return false
    end
    cmd, csize = self.smtp:pack_mail(subject, data)
    if not cmd then
        return
    end
    pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack then
        return false
    end
    return self.smtp:check_ok(pack)
end
function ctx:send(from, rcpt, subject, data)
    if not self.smtp:check_auth() then
        return false
    end
    local rtn = self:_send(from, rcpt, subject, data)
    self:reset()
    return rtn
end
function ctx:quit()
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_quit()
    if not cmd then
        return
    end
    srey.syn_send(fd, skid, cmd, csize, 0)
end

return ctx
