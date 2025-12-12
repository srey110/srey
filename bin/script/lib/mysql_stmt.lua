local srey = require("lib.srey")
local mysql = require("mysql")
local stmt = require("mysql.stmt")
local reader = require("mysql.reader")
local MYSQL_PACK_TYPE = MYSQL_PACK_TYPE

local ctx = class("mysql_stmt_ctx")
function ctx:ctor(mpack)
    self.stmt = stmt.new(mpack)
    self.fd, self.skid = self.stmt:sock_id()
end
function ctx:execute(mbind)
    local pack, size = self.stmt:pack_stmt_execute(mbind)
    local mpack, _ =  srey.syn_send(self.fd, self.skid, pack, size, 0)
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
function ctx:reset()
    local pack, size = self.stmt:pack_stmt_reset()
    local mpack, _ =  srey.syn_send(self.fd, self.skid, pack, size, 0)
    if not mpack then
        return false
    end
    return MYSQL_PACK_TYPE.MPACK_OK == mysql.pack_type(mpack)
end

return ctx
