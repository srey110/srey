local srey = require("lib.srey")
local stmt = require("lib.mysql_stmt")
local core = require("srey.core")
local mysql = require("mysql")
local reader = require("mysql.reader")
local MYSQL_PACK_TYPE = MYSQL_PACK_TYPE

local ctx = class("mysql_ctx")
function ctx:ctor(ip, port, sslname, user, password, database, charset, maxpk, relink)
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
    end
    self.relink = relink
    self.mysql = mysql.new(ip, port, ssl, user, password, database, charset, maxpk)
end
function ctx:connect()
    if not self.mysql:try_connect() then
        return false
    end
    local fd, skid = self.mysql:sock_id()
    local ok,_,_ = srey.wait_handshaked(fd, skid)
    return ok
end
function ctx:selectdb(database)
    local pack, size = self.mysql:pack_selectdb(database)
    if not pack then
        return false
    end
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if nil == mpack then
        return false
    end
    return MYSQL_PACK_TYPE.MPACK_OK == mysql.pack_type(mpack)
end
function ctx:ping()
    if self.relink and self.mysql:link_closed() then
        if not self:connect() then
            return false
        end
    end
    local pack, size = self.mysql:pack_ping()
    if not pack then
        return false
    end
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return false
    end
    return true
end
function ctx:query(sql, mbind)
    local pack, size = self.mysql:pack_query(sql, mbind)
    if not pack then
        return false
    end
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return false
    end
    local pktype = mysql.pack_type(mpack)
    if MYSQL_PACK_TYPE.MPACK_OK == pktype then
        return true
    end
    if MYSQL_PACK_TYPE.MPACK_ERR == pktype then
        return false
    end
    return reader.new(mpack)
end
function ctx:prepare(sql)
    local pack, size = self.mysql:pack_stmt_prepare(sql)
    if not pack then
        return false
    end
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return false
    end
    if MYSQL_PACK_TYPE.MPACK_ERR == mysql.pack_type(mpack) then
        return false
    end
    return stmt.new(mpack)
end
function ctx:version()
    return self.mysql:version()
end
function ctx:erro()
    return self.mysql:erro()
end
function ctx:last_id()
    return self.mysql:last_id()
end
function ctx:affectd_rows()
    return self.mysql:affectd_rows()
end

return ctx
