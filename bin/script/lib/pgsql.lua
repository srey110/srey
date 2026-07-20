-- PostgreSQL 客户端（pgsql_ctx 类）。
-- 封装 C 层 pgsql 模块，提供：连接管理、ping 保活、
-- 简单查询（query）、预处理语句（prepare）及 COPY IN/OUT 数据流操作。
-- 查询结果通过 pgsql.reader 惰性迭代，避免大结果集一次性复制到 Lua。
-- 异步通知（LISTEN/NOTIFY）由 srey.on_recved 回调接收，不在此层处理。

local srey   = require("lib.srey")
local stmt   = require("lib.pgsql_stmt")
local pgsql  = require("pgsql")
local reader = require("pgsql.reader")

-- pgsql_ctx：PostgreSQL 连接上下文，每实例对应一条持久连接。
local ctx = class("pgsql_ctx")

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param user string 用户名
---@param password string 密码
---@param database string 数据库名
function ctx:ctor(ip, port, sslname, user, password, database)
    local ok, ssl = srey.ssl_qury(sslname)
    if not ok then
        error(string.format("ssl_qury not find ssl name %s", sslname), 2)
    end
    self.pg = pgsql.new(ip, port, ssl, user, password, database)
    if not self.pg then
        error(string.format("pgsql.new failed: %s:%d db=%s", ip, port, tostring(database)), 2)
    end
    self.affected = 0
    self.err = ""
    self.ip = ip
    self.port = port
    self.sslname = sslname
    -- 连接代次：每次 connect 成功后 +1，prepare 出来的 stmt 持有创建时的代次，
    -- execute 前比对，重连后旧 statement name 已被服务端清理时返 false 明确提示重新 prepare
    self.generation = 0
    -- connect() 进行中标志：握手完成前 fd/skid 尚不可用，其余方法须 fail-fast 拒绝，避免并发协程读到未就绪的连接
    self.connecting = false
end

---建立 TCP 连接并完成 PostgreSQL 握手；成功后 skid 设为会话键
---@return boolean ok 握手成功 true，失败 false（含并发期间已有 connect() 在进行中）
function ctx:connect()
    if self.connecting then
        return false
    end
    self.connecting = true
    if not self.pg:try_connect() then
        self.connecting = false
        return false
    end
    local fd, skid = self.pg:sock_id()
    if not srey.wait_connect(fd, skid) then
        self.connecting = false
        return false
    end
    local ok, _, _ = srey.wait_handshaked(fd, skid)
    if ok then
        self.generation = self.generation + 1
    end
    self.connecting = false
    return ok
end

---内部 ping：发送 "SELECT 1" 简单查询探活，不自动重连
---@return boolean ok 服务端响应 OK 时 true（connect() 进行中时 fail-fast 返回 false；仅供 ping() 内部调用，不要直接调用）
function ctx:_ping()
    if self.connecting then
        return false
    end
    local pack, size = pgsql.pack_query("SELECT 1")
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    return PGPACK_TYPE.OK == pgsql.pack_type(pgpack)
end

---连接保活：ping 失败时自动重连，建议在执行查询前调用
---@return boolean ok 连接可用 true（connect() 进行中时 fail-fast 返回 false，避免与外层 connect() 抢同一 fd/skid）
function ctx:ping()
    if self.connecting then
        return false
    end
    if not self:_ping() then
        local fd, skid = self.pg:sock_id()
        srey.sync_close(fd, skid, 1)
        return self:connect()
    end
    return true
end

---执行简单查询（Query 协议）
---@param sql string SQL 语句
---@param format PG_FORMAT? 已废弃：简单查询协议服务端恒以文本格式应答，传 BINARY 无效，结果固定按 TEXT 解析
---@return boolean|_pgsql_reader_ctx result reader=结果集；true=无结果集 OK；false=失败或 connect() 进行中
function ctx:query(sql, format)
    if self.connecting then
        return false
    end
    if format and PG_FORMAT.TEXT ~= format then
        WARN("pgsql simple query protocol always replies in text format; format=%s ignored, use prepare()/execute() for binary results.", tostring(format))
    end
    local pack, size = pgsql.pack_query(sql)
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    local pktype = pgsql.pack_type(pgpack)
    if PGPACK_TYPE.ERR == pktype then
        self.err = pgsql.erro(pgpack) or ""
        return false
    end
    if PGPACK_TYPE.OK ~= pktype then
        self.err = string.format("unexpected pgsql response type: %s", tostring(pktype))
        return false
    end
    local rd = reader.new(pgpack, PG_FORMAT.TEXT)
    if rd then
        return rd
    end
    self.affected = pgsql.affected_rows(pgpack)
    return true
end

---准备预处理语句（Parse + Sync）
---@param name string 服务端语句名（"" 表示匿名）
---@param sql string SQL 文本
---@param nparam integer? 参数数量，默认 0
---@param oids integer[]? 各参数类型 OID 数组
---@param format PG_FORMAT? execute 时结果列格式，默认 BINARY
---@return any|false stmt pgsql_stmt_ctx 实例；失败或 connect() 进行中返回 false
function ctx:prepare(name, sql, nparam, oids, format)
    if self.connecting then
        return false
    end
    local pack, size = pgsql.pack_stmt_prepare(name, sql, nparam or 0, oids)
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    if PGPACK_TYPE.ERR == pgsql.pack_type(pgpack) then
        self.err = pgsql.erro(pgpack) or ""
        return false
    end
    return stmt.new(self, name, format)
end

-- COPY IN 三步操作 --

---发起 COPY FROM STDIN 查询，等待服务端 CopyInResponse
---@param sql string COPY ... FROM STDIN 语句
---@return integer|false fmt 成功时为 format（0=TEXT / 1=BINARY）；失败或 connect() 进行中返回 false
---@return integer? ncol 列数（仅成功时返回）
function ctx:copy_in_begin(sql)
    if self.connecting then
        return false
    end
    local pack, size = pgsql.pack_query(sql)
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    local pktype = pgsql.pack_type(pgpack)
    if PGPACK_TYPE.COPY_IN ~= pktype then
        if PGPACK_TYPE.ERR == pktype then
            self.err = pgsql.erro(pgpack) or ""
            return false
        end
        return false
    end
    return pgsql.copy_in_info(pgpack)
end

---发送一块 CopyData（不等待响应，可多次调用）
---@param data string|lightuserdata 数据；字符串时长度自动取得
---@param size integer? data 为 lightuserdata 时必填
function ctx:copy_in_data(data, size)
    if not data or self.connecting then
        return
    end
    local pack, psize = pgsql.pack_copy_data(data, size)
    local fd, skid = self.pg:sock_id()
    srey.send(fd, skid, pack, psize, 0)
end

---发送 CopyDone，等待服务端 CommandComplete + ReadyForQuery
---@return boolean ok 成功 true；失败或 connect() 进行中 false
function ctx:copy_in_done()
    if self.connecting then
        return false
    end
    local pack, size = pgsql.pack_copy_done()
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    local pktype = pgsql.pack_type(pgpack)
    if PGPACK_TYPE.OK == pktype then
        self.affected = pgsql.affected_rows(pgpack)
        return true
    end
    self.err = pgsql.erro(pgpack) or ""
    return false
end

---发送 CopyFail 中止 COPY IN 流程，等待服务端 ErrorResponse + ReadyForQuery
---@param msg string? 错误原因描述
---@return boolean ok 服务端已确认中止返回 true；通信失败或 connect() 进行中返回 false
function ctx:copy_in_abort(msg)
    if self.connecting then
        return false
    end
    local pack, size = pgsql.pack_copy_fail(msg or "")
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    self.err = pgsql.erro(pgpack) or ""
    return true
end

-- COPY OUT --

---执行 COPY TO STDOUT 查询，一次性返回全部数据
---@param sql string COPY ... TO STDOUT 语句
---@return lightuserdata|false data 数据指针；失败或 connect() 进行中返回 false
---@return integer? size 成功时为字节数
function ctx:copy_out(sql)
    if self.connecting then
        return false
    end
    local pack, size = pgsql.pack_query(sql)
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    local pktype = pgsql.pack_type(pgpack)
    if PGPACK_TYPE.COPY_OUT ~= pktype then
        if PGPACK_TYPE.ERR == pktype then
            self.err = pgsql.erro(pgpack) or ""
            return false
        end
        return false
    end
    return pgsql.copy_out_data(pgpack)
end

---发送 Terminate 消息并关闭连接
function ctx:quit()
    local fd, skid = self.pg:sock_id()
    if INVALID_SOCK == fd then
        return
    end
    local pack, size = pgsql.pack_terminate()
    srey.send(fd, skid, pack, size, 0)
    srey.sync_close(fd, skid)
end

---切换数据库：关闭当前连接 → 更新库名 → 重连（重连后旧 prepare 语句失效，代次 +1）
---@param database string 目标数据库名
---@return boolean ok 切换并重连成功 true（connect() 进行中时 fail-fast 返回 false，避免 quit() 误挂外层正在建立的连接）
function ctx:selectdb(database)
    if self.connecting then
        return false
    end
    self:quit()
    self.pg:set_db(database)
    return self:connect()
end

---取消当前正在执行的查询：在独立连接上发送 CancelRequest，服务端处理后主动断开、无响应
---@return boolean ok 发送成功 true；未连接或 connect() 进行中返回 false
function ctx:cancel()
    if self.connecting then
        return false
    end
    local fd = self.pg:sock_id()
    if INVALID_SOCK == fd then
        return false
    end
    local pack = self.pg:pack_cancel()
    if SSL_NAME.NONE ~= self.sslname then
        WARN("pgsql cancel: CancelRequest sent in plaintext (BackendKeyData pid+key exposed); SSL cancel not supported")
    end
    local cfd, cskid = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, self.ip, self.port)
    if INVALID_SOCK == cfd then
        return false
    end
    srey.send(cfd, cskid, pack, #pack, 1)
    srey.close(cfd, cskid)
    return true
end

---更新连接认证信息（下次重连时生效）
---@param user string 新用户名
---@param password string 新密码
function ctx:set_userpwd(user, password)
    self.pg:set_userpwd(user, password)
end

---更新目标数据库名（下次重连时生效）
---@param database string 新数据库名
function ctx:set_db(database)
    self.pg:set_db(database)
end

---获取当前配置的数据库名
---@return string database 数据库名
function ctx:get_db()
    return self.pg:get_db()
end

---返回服务端就绪状态字符（ASCII 整数）
---@return integer status 73('I') 空闲 / 84('T') 事务中 / 69('E') 失败事务中
function ctx:readyforquery()
    return self.pg:readyforquery()
end

---返回当前连接的 fd 和 skid
---@return integer fd socket fd
---@return integer skid 连接 skid
function ctx:sock_id()
    return self.pg:sock_id()
end

---返回最近一次错误信息
---@return string err 错误描述
function ctx:erro()
    return self.err
end

---返回最近一次受影响行数
---@return integer rows affected rows
function ctx:affected_rows()
    return self.affected
end

ctx.PACK_TYPE = PGPACK_TYPE
ctx.FORMAT    = PG_FORMAT

return ctx
