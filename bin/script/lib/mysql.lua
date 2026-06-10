-- MySQL 客户端（mysql_ctx 类）。
-- 封装 C 层 mysql 模块，提供：连接管理、ping 保活、
-- 普通查询（query）、预处理语句（prepare）及会话控制。
-- 查询结果通过 mysql.reader 惰性迭代，避免大结果集一次性复制到 Lua。

local srey  = require("lib.srey")
local stmt  = require("lib.mysql_stmt")
local core  = require("srey.core")
local mysql = require("mysql")
local reader = require("mysql.reader")
local MYSQL_PACK_TYPE = MYSQL_PACK_TYPE

-- mysql_ctx：MySQL 连接上下文，每实例对应一条持久连接。
local ctx = class("mysql_ctx")

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param user string 用户名
---@param password string 密码
---@param database string 初始数据库
---@param charset string 字符集（如 "utf8mb4"）
---@param maxpk integer? 单包最大字节数，0 使用默认
function ctx:ctor(ip, port, sslname, user, password, database, charset, maxpk)
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
    end
    self.mysql = mysql.new(ip, port, ssl, user, password, database, charset, maxpk)
    if not self.mysql then
        error(string.format("mysql.new failed: %s:%d db=%s", ip, port, tostring(database)), 2)
    end
    -- 连接代次：每次 connect 成功后 +1，prepare 出来的 stmt 持有创建时的代次，
    -- execute 前比对，重连后旧 statement_id 已被服务端清理时返 false 明确提示重新 prepare
    self.generation = 0
end

---建立 TCP 连接并完成 MySQL 握手（Handshake/AuthResponse）；成功后 skid 设为会话键
---@return boolean ok 握手成功 true，失败 false
function ctx:connect()
    if not self.mysql:try_connect() then
        return false
    end
    local fd, skid = self.mysql:sock_id()
    local ok,_,_ = srey.wait_handshaked(fd, skid)
    if ok then
        srey.sock_session(fd, skid, skid)
        self.generation = self.generation + 1
    end
    return ok
end

---切换当前数据库（COM_INIT_DB）
---@param database string 目标数据库名
---@return boolean ok 切换成功 true
function ctx:selectdb(database)
    local pack, size = self.mysql:pack_selectdb(database)
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if nil == mpack then
        return false
    end
    return MYSQL_PACK_TYPE.MPACK_OK == mysql.pack_type(mpack)
end

---内部 ping（COM_PING），不自动重连
---@return boolean ok 服务端响应即 true
function ctx:_ping()
    local pack, size = self.mysql:pack_ping()
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return false
    end
    return true
end

---连接保活：ping 失败时自动重连，建议在执行查询前调用
---@return boolean ok 连接可用 true
function ctx:ping()
    if not self:_ping() then
        local fd, skid = self.mysql:sock_id()
        srey.sync_close(fd, skid, 1)
        return self:connect()
    end
    return true
end

---执行 SQL 查询（COM_QUERY）
---@param sql string SQL 语句
---@param mbind any? mysql_bind_ctx 参数绑定上下文
---@return boolean|_mysql_reader_ctx result true=OK 包；false=ERR 包或网络失败；reader=结果集（SELECT）
function ctx:query(sql, mbind)
    local pack, size = self.mysql:pack_query(sql, mbind)
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

---准备预处理语句（COM_STMT_PREPARE）
---@param sql string 含 ? 占位符的 SQL 语句
---@return any|false stmt mysql_stmt_ctx 实例；失败返回 false
function ctx:prepare(sql)
    local pack, size = self.mysql:pack_stmt_prepare(sql)
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return false
    end
    if MYSQL_PACK_TYPE.MPACK_ERR == mysql.pack_type(mpack) then
        return false
    end
    return stmt.new(self, mpack)
end

---发送 COM_QUIT 并关闭连接
function ctx:quit()
    local fd, skid = self.mysql:sock_id()
    if INVALID_SOCK == fd then
        return
    end
    local pack, size = self.mysql:pack_quit()
    srey.send(fd, skid, pack, size, 0)
    srey.sync_close(fd, skid)
end

---返回服务端版本字符串（握手阶段获取）
---@return string version 服务端版本
function ctx:version()
    return self.mysql:version()
end

---返回最近一次错误信息并清除错误状态
---@return string err 错误描述
function ctx:erro()
    return self.mysql:erro()
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
