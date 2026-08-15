-- MySQL 客户端（mysql_ctx 类）。
-- 封装 C 层 mysql 模块，提供：连接管理、ping 保活、
-- 普通查询（query）、预处理语句（prepare）及会话控制。
-- 查询结果通过 mysql.reader 惰性迭代，避免大结果集一次性复制到 Lua。

local srey  = require("lib.srey")
local stmt  = require("lib.mysql_stmt")
local mysql = require("mysql")
local reader = require("mysql.reader")
local pub   = require("lib.conn_pub")-- connect / ping / quit 的共用骨架
local MYSQL_PACK_TYPE = MYSQL_PACK_TYPE

-- mysql_ctx：MySQL 连接上下文，每实例对应一条持久连接。
-- 建链、保活、断开三段继承自 conn_pub，本文件只实现 _connect / _ping / _doquit 三个钩子。
local ctx = class("mysql_ctx", pub)

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
    local ok, ssl = srey.ssl_qury(sslname)
    if not ok then
        error(string.format("ssl_qury not find ssl name %s", sslname), 2)
    end
    self.mysql = mysql.new(ip, port, ssl, user, password, database, charset, maxpk)
    if not self.mysql then
        error(string.format("mysql.new failed: %s:%d db=%s", ip, port, tostring(database)), 2)
    end
    self.sslname = sslname
    -- MySQL 半双工，一条命令的响应没收完就发下一条会串包，而 mysql_ctx 的解析状态
    -- 又是每连接一份，交错即互相覆盖——串行化执行器由 conn_pub 建
    pub.init(self, self.mysql)
end

-- conn_pub 的建链钩子：TCP 连接 + MySQL 握手（Handshake/AuthResponse），成功后 skid 设为会话键
function ctx:_connect()
    if not self.mysql:try_connect() then
        return false
    end
    local fd, skid = self.mysql:sock_id()
    if not srey.wait_connect(fd, skid, SSL_NAME.NONE ~= self.sslname or nil) then
        return false
    end
    local ok,_,_ = srey.wait_handshaked(fd, skid)
    return ok
end

---切换当前数据库（COM_INIT_DB）
---@param database string 目标数据库名
---@return boolean ok 切换成功 true（库名超 63 字节时不发包直接返 false）
function ctx:selectdb(database)
    return srey.serial_ret(false, self.serial(self._selectdb, self, database))
end
function ctx:_selectdb(database)
    local pack, size = self.mysql:pack_selectdb(database)
    if nil == pack then
        return false
    end
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if nil == mpack then
        return false
    end
    return MYSQL_PACK_TYPE.MPACK_OK == mysql.pack_type(mpack)
end

-- conn_pub 的探活钩子：COM_PING，不自动重连。
-- 必须正向判 MPACK_OK，不能只判"收到了包"：conn_pub 的 _pingreconn 拿本函数的返回值当唯一
-- 重连判据，只判非 nil 的话，服务端以 ERR 应答 COM_PING（shutdown 期的 1053、连接被 KILL 之类）
-- 会被报成健康，于是永不重连，此后每一轮 ping + query 都重复失败。
-- 连接因前一次多结果集没收干净而错位时同理：读到的可能是上一条命令残留的包，判型能发现，
-- 只判非 nil 则会把它当自己的 pong 吃掉，错位从此无法自愈
function ctx:_ping()
    local pack, size = self.mysql:pack_ping()
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return false
    end
    return MYSQL_PACK_TYPE.MPACK_OK == mysql.pack_type(mpack)
end

---收齐一次请求的全部响应包（多语句 / CALL 会产生多个结果集），query 与 stmt:execute 共用
---@param fd integer socket fd
---@param skid integer 会话键
---@param mpack lightuserdata 首个响应包
---@return (_mysql_reader_ctx|boolean)[]|nil results 结果集数组（元素 reader=SELECT 结果集 / true=OK 包 / false=ERR 包）；中途断连或结果集异常返回 nil
function ctx:_read_results(fd, skid, mpack)
    local results = {}
    local failed = false
    while true do
        local more = mysql.has_more(mpack)-- has_more 须在 reader.new 前读
        local pktype = mysql.pack_type(mpack)
        if MYSQL_PACK_TYPE.MPACK_OK == pktype then
            results[#results + 1] = true
        elseif MYSQL_PACK_TYPE.MPACK_ERR == pktype then
            results[#results + 1] = false
        else
            local rd = reader.new(mpack)
            if rd then
                results[#results + 1] = rd
            else
                failed = true -- 异常结果集：标记失败但继续排空剩余包，避免留在连接缓冲致下次查询 desync
            end
        end
        if not more then
            break
        end
        mpack = srey.syn_recv(fd, skid)
        if not mpack then
            return nil
        end
    end
    if failed then
        return nil
    end
    return results
end

---执行 SQL 查询（COM_QUERY）。
---服务端支持 CLIENT_SESSION_TRACK（MySQL 5.7+）时 query("USE xxx") 会被 OK 包的
---session-state-change 跟踪，重连握手用切换后的库；老服务端不带该信息，那时切库须用 selectdb，
---否则 ping 失败自动重连会按旧库名握手，之后未限定库名的语句全部打到原库上
---@param sql string SQL 语句
---@param mbind any? mysql_bind_ctx 参数绑定上下文
---@return (_mysql_reader_ctx|boolean)[]|nil results 结果集数组（元素 reader=SELECT 结果集 / true=OK 包 / false=ERR 包）；网络失败或多结果集中途断连返回 nil
function ctx:query(sql, mbind)
    return srey.serial_ret(nil, self.serial(self._query, self, sql, mbind))
end
-- 锁覆盖到 _read_results 的续读循环为止：多结果集是"一次请求多个响应"，
-- 中途放别人进来，它的响应会被我们的 syn_recv 收走
function ctx:_query(sql, mbind)
    local pack, size = self.mysql:pack_query(sql, mbind)
    if not pack then
        WARN("mysql query payload exceeds 16MB.")
        return nil
    end
    local fd, skid = self.mysql:sock_id()
    local mpack = srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return nil
    end
    return self:_read_results(fd, skid, mpack)
end

---准备预处理语句（COM_STMT_PREPARE）
---用完须调 stmt:close() 释放服务端句柄：GC 只做本地释放，不发 COM_STMT_CLOSE，
---长连接上每请求 prepare 一次会逐步撞满 max_prepared_stmt_count（默认 16382）
---@param sql string 含 ? 占位符的 SQL 语句
---@return any|false stmt mysql_stmt_ctx 实例；失败返回 false
function ctx:prepare(sql)
    return srey.serial_ret(false, self.serial(self._prepare, self, sql))
end
function ctx:_prepare(sql)
    local pack, size = self.mysql:pack_stmt_prepare(sql)
    if not pack then
        WARN("mysql stmt_prepare payload exceeds 16MB.")
        return false
    end
    local fd, skid = self.mysql:sock_id()
    local mpack, _ =  srey.syn_send(fd, skid, pack, size, 0)
    if not mpack then
        return false
    end
    -- 必须正向判 MPACK_STMT_PREPARE 而不是"非 ERR 即成功"：连接因前一次多结果集没收干净而
    -- 错位时,这里收到的可能是残留的 OK 包,mysql_stmt_init 对它返 NULL → stmt.new 返 nil →
    -- mysql_stmt 的 ctor 直接 error 抛出、穿透本函数,调用方按注解写的 `if not stmt` 完全失效
    if MYSQL_PACK_TYPE.MPACK_STMT_PREPARE ~= mysql.pack_type(mpack) then
        return false
    end
    return stmt.new(self, mpack)
end

-- conn_pub 的断开钩子：COM_QUIT 不等响应，发完直接关
function ctx:_doquit()
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
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---（读取即清除，更要紧挨着命令读——别的协程先读一次就把它清空了）
---@return string err 错误描述
function ctx:erro()
    return self.mysql:erro()
end

---返回最近一次 INSERT 操作产生的自增 ID
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---@return integer id last insert id
function ctx:last_id()
    return self.mysql:last_id()
end

---返回最近一次 UPDATE/DELETE/INSERT 影响的行数
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---@return integer rows affected rows
function ctx:affectd_rows()
    return self.mysql:affectd_rows()
end

return ctx
