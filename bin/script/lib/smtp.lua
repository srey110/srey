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
    local ok, err, elens = srey.wait_handshaked(fd, skid)
    if not ok and err then
        WARN("%s", srey.ud_str(err, elens))
    end
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
function ctx:_send(mail)
    local fd, skid = self.smtp:sock_id()
    local cmd, csize = self.smtp:pack_from(mail:from_get())
    if not cmd then
        return false
    end
    local pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack or not self.smtp:check_ok(pack)  then
        return false
    end
    for _, addr in pairs(mail:addrs_get()) do
        cmd, csize = self.smtp:pack_rcpt(addr)
        if not cmd then
            return false
        end
        pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
        if nil == pack or not self.smtp:check_ok(pack) then
            return false
        end
    end
    cmd, csize = self.smtp:pack_data()
    if not cmd then
        return false
    end
    pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack or not self.smtp:check_code(pack, "354") then
        return false
    end
    cmd, csize = mail:pack()
    pack, _ =  srey.syn_send(fd, skid, cmd, csize, 0)
    if nil == pack or not self.smtp:check_ok(pack) then
        return false
    end
    return true
end
function ctx:send(mail)
    if not self.smtp:check_auth() then
        return false
    end
    local rtn = self:_send(mail)
    self:reset()
    return rtn
end
function ctx:_quit(fd, skid)
    local cmd, csize = self.smtp:pack_quit()
    if not cmd then
        return
    end
    srey.syn_send(fd, skid, cmd, csize, 0)
end
function ctx:quit()
    local fd, skid = self.smtp:sock_id()
    self:_quit(fd, skid)
    srey.close(fd, skid)
end

return ctx
