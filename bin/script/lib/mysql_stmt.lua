-- MySQL 预处理语句执行器（mysql_stmt_ctx 类）。
-- 由 mysql_ctx:prepare() 返回，持有 C 层 stmt 句柄。
-- 同一 stmt 可多次调用 execute（每次绑定不同参数），
-- 执行完毕后调用 reset 让服务端恢复到 prepare 后的就绪状态。

local srey   = require("lib.srey")
local mysql  = require("mysql")
local stmt   = require("mysql.stmt")
local reader = require("mysql.reader")
local MYSQL_PACK_TYPE = MYSQL_PACK_TYPE

-- mysql_stmt_ctx：预处理语句执行上下文。
-- self.stmt      ：C 层 stmt 对象，持有服务端 statement_id；动态调用 sock_id() 感知重连。
-- self.owner     ：mysql_ctx Lua 包装实例，守卫读其实时 generation。
-- self.mysql     ：C 层 mysql 对象引用（= owner.mysql），用于 last_id / affectd_rows 查询。
local ctx = class("mysql_stmt_ctx")

---构造函数
---@param owner any mysql_ctx Lua 包装实例（持有实时 generation 与 C mysql 对象）
---@param mpack lightuserdata COM_STMT_PREPARE 响应包，C 层据此初始化列元数据和参数个数
function ctx:ctor(owner, mpack)
    self.stmt = stmt.new(mpack)
    if not self.stmt then
        error("mysql stmt.new failed", 2)
    end
    self.owner = owner
    self.mysql = owner.mysql
    -- 记录创建时的连接代次；mysql ping 失败重连后 owner.generation +1，旧 statement_id 已失效
    self.gen = owner.generation
end

---执行预处理语句（COM_STMT_EXECUTE）
---@param mbind any? mysql_bind_ctx 参数绑定上下文
---@return boolean|_mysql_reader_ctx result true=OK；false=ERR；reader=结果集
function ctx:execute(mbind)
    if self.gen ~= self.owner.generation then
        WARN("mysql stmt invalidated by reconnect, please re-prepare.")
        return false
    end
    local fd, skid = self.stmt:sock_id()
    local pack, size = self.stmt:pack_stmt_execute(mbind)
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

---发送 COM_STMT_RESET：清除服务端语句执行状态，保留 prepare 结果，下次 execute 可绑定新参数
---@return boolean ok 重置成功 true
function ctx:reset()
    if self.gen ~= self.owner.generation then
        WARN("mysql stmt invalidated by reconnect, please re-prepare.")
        return false
    end
    local fd, skid = self.stmt:sock_id()
    local pack, size = self.stmt:pack_stmt_reset()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return false
    end
    return MYSQL_PACK_TYPE.MPACK_OK == mysql.pack_type(mpack)
end

---返回最近一次 INSERT 操作产生的自增 ID
---@return integer id last insert id
function ctx:last_id()
    return self.mysql:last_id()
end

---返回最近一次 UPDATE/DELETE/INSERT 影响的行数
---@return integer rows affected rows
function ctx:affectd_rows()
    return self.mysql:affectd_rows()
end

return ctx
