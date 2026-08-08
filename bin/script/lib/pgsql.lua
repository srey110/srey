-- PostgreSQL 客户端（pgsql_ctx 类）。
-- 封装 C 层 pgsql 模块，提供：连接管理、ping 保活、
-- 简单查询（query）、预处理语句（prepare）及 COPY IN/OUT 数据流操作。
-- 查询结果通过 pgsql.reader 惰性迭代，避免大结果集一次性复制到 Lua。
-- 异步通知（LISTEN/NOTIFY）由 srey.on_recved 回调接收，不在此层处理。

local srey   = require("lib.srey")
local stmt   = require("lib.pgsql_stmt")
local pgsql  = require("pgsql")
local reader = require("pgsql.reader")
local ppub   = require("lib.pgsql_pub")-- 失败原因与 err 契约，见该模块头部

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
    -- 命令串行化执行器：多协程共用一条连接时按 FIFO 排队。一条 pgsql 命令要读到
    -- ReadyForQuery 才算完，copy_in 更是整段会话，交错会让整条连接错位。
    -- 建在 ctor 而非 connect：connect 会被 ping / selectdb 的重连路径重入，
    -- 建在那儿会在重连时换掉执行器，把排队者连同锁一起丢掉
    self.serial = srey.serial()
end

---建立 TCP 连接并完成 PostgreSQL 握手；成功后 skid 设为会话键
---@return boolean ok 握手成功 true，失败 false。多协程并发调用时按 FIFO 串行，
---排在后面那个若发现连接已被前一个重建好（代次已变）直接返 true，不再白拆一次
function ctx:connect()
    -- 排队前记下代次：等锁期间别人可能已经把连接重建好了（每次 _connect 成功都会递增）
    local gen = self.generation
    return srey.serial_ret(false, self.serial(self._doconnect, self, gen))
end
function ctx:_doconnect(gen)
    if gen ~= self.generation then
        return true
    end
    return self:_connect()
end
function ctx:_connect()
    if not self.pg:try_connect() then
        return false
    end
    local fd, skid = self.pg:sock_id()
    if not srey.wait_connect(fd, skid) then
        return false
    end
    local ok, _, _ = srey.wait_handshaked(fd, skid)
    if ok then
        self.generation = self.generation + 1
    end
    return ok
end

---内部 ping：发送 "SELECT 1" 简单查询探活，不自动重连
---@return boolean ok 服务端响应 OK 时 true（仅供 ping() 内部调用，不要直接调用；调用方须已持锁）
function ctx:_ping()
    local pack, size = pgsql.pack_query("SELECT 1")
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    return PGPACK_TYPE.OK == pgsql.pack_type(pgpack)
end

---连接保活：ping 失败时自动重连，建议在执行查询前调用
---@return boolean ok 连接可用 true；ping 失败时在锁内重连，重连也失败返 false
function ctx:ping()
    return srey.serial_ret(false, self.serial(self._pingreconn, self))
end
-- 重连整段也在锁内：连接正在重建时别人不该往上发命令，而 fd/skid 换掉之后
-- 排队者醒来拿到的自然是新连接
function ctx:_pingreconn()
    if not self:_ping() then
        local fd, skid = self.pg:sock_id()
        srey.sync_close(fd, skid, 1)
        return self:connect()
    end
    return true
end

-- 写 err 并返回 false 的合并写法，让"置原因"与"报失败"成为一步，不会只做一半
---@param err string 失败原因
---@return boolean always false
function ctx:_fail(err)
    self.err = err
    return false
end

---执行简单查询（Query 协议）
---@param sql string SQL 语句
---@param format PG_FORMAT? 已废弃：简单查询协议服务端恒以文本格式应答，传 BINARY 无效，结果固定按 TEXT 解析
---@return boolean|_pgsql_reader_ctx result reader=结果集；true=无结果集 OK；false=失败
function ctx:query(sql, format)
    return srey.serial_ret(false, self.serial(self._query, self, sql, format))
end
function ctx:_query(sql, format)
    self.err = ""-- 复位:erro() 只反映最近一次操作
    if format and PG_FORMAT.TEXT ~= format then
        WARN("pgsql simple query protocol always replies in text format; format=%s ignored, use prepare()/execute() for binary results.", tostring(format))
    end
    local pack, size = pgsql.pack_query(sql)
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return self:_fail(ppub.SEND)
    end
    local e = ppub.check_type(pgpack, PGPACK_TYPE.OK)
    if e then
        return self:_fail(e)
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
---@return any|false stmt pgsql_stmt_ctx 实例；失败返回 false
function ctx:prepare(name, sql, nparam, oids, format)
    return srey.serial_ret(false, self.serial(self._prepare, self, name, sql, nparam, oids, format))
end
function ctx:_prepare(name, sql, nparam, oids, format)
    self.err = ""-- 复位:erro() 只反映最近一次操作
    local pack, size = pgsql.pack_stmt_prepare(name, sql, nparam or 0, oids)
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return self:_fail(ppub.SEND)
    end
    local e = ppub.check_type(pgpack, PGPACK_TYPE.OK)
    if e then
        return self:_fail(e)
    end
    return stmt.new(self, name, format)
end

-- COPY IN --

---执行 COPY FROM STDIN 全过程：发起 → 逐块写 → 收尾。整段在锁内独占连接。
---服务端一进 COPY IN 模式就只认 CopyData/CopyFail，别的协程的普通查询挤进来会让整条
---连接报错，所以三步不再拆成三个公开方法（那样锁得跨用户调用，用户忘了收尾连接就永久卡死）。
---producer 抛错或某块发送失败时，本函数会替你发 CopyFail 把服务端拉出 COPY IN 模式，
---连接仍可继续使用。
---producer 内不要再碰这条连接：锁对同协程是可重入的，那条命令会真的发出去，
---而服务端此刻只认 CopyData/CopyFail。
---@param sql string COPY ... FROM STDIN 语句
---@param producer fun(format:integer, ncol:integer):(string|lightuserdata|boolean|nil), (integer|string|nil)
---       数据块生成器，反复调用，每次都带上服务端 CopyInResponse 里的格式与列数（不关心可忽略）：
---       返回 (data) 或 (data, size) 继续写（size 仅 data 为 lightuserdata 时必填）；
---       返回 nil 表示写完，走 CopyDone；返回 (false, reason) 表示主动中止，走 CopyFail
---@return boolean ok 服务端已按请求收尾（正常完成或按请求中止）true；失败 false，原因走 erro()
---@return string? aborted 走了中止路径时为服务端 ErrorResponse 文本；正常完成时为 nil
function ctx:copy_in(sql, producer)
    return srey.serial_ret(false, self.serial(self._copy_in, self, sql, producer))
end
-- 发 CopyFail 并等服务端确认。中止本身成功时返回 (true, 服务端文本)：
-- CopyFail 的正常应答就是 ErrorResponse，所以"中止成功"也走 ERR 包。
-- 成功路径不写 err —— 写了的话 erro() 会把一次正常中止报成失败，
-- 成为本文件"err 非空 == 上一次操作失败"这条读法的唯一例外
function ctx:_copy_fail(msg)
    local pack, size = pgsql.pack_copy_fail(msg or "")
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return self:_fail(ppub.SEND)
    end
    -- 必须正向判 ERR：带 LISTEN 时一条抢先到达的 NOTIFICATION 会被当成"服务端已确认中止"，
    -- 真正的 ErrorResponse + ReadyForQuery 留在流里被下一个请求的等待者接走，连接从此错位一格
    local e = ppub.check_type(pgpack, PGPACK_TYPE.ERR)
    if e then
        return self:_fail(e)
    end
    return true, pgsql.erro(pgpack)
end
-- 逐块写完 producer 给的数据。返回 true=写完可以收尾，(false, reason)=producer 要求中止；
-- 任何异常都靠 error 抛给调用方那个 pcall，由它统一发 CopyFail
---@return boolean more
---@return string? reason
function ctx:_copy_stream(fd, skid, producer, format, ncol)
    local pack, psize
    local data, extra
    while true do
        data, extra = producer(format, ncol)
        if nil == data then
            return true
        end
        if false == data then
            return false, extra
        end
        pack, psize = pgsql.pack_copy_data(data, extra)
        if not srey.send(fd, skid, pack, psize, 0) then
            error(ppub.SEND, 0)
        end
    end
end
function ctx:_copy_in(sql, producer)
    self.err = ""-- 复位:erro() 只反映最近一次操作
    if "function" ~= type(producer) then
        return self:_fail("copy_in: producer must be a function")
    end
    local pack, size = pgsql.pack_query(sql)
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return self:_fail(ppub.SEND)
    end
    local e = ppub.check_type(pgpack, PGPACK_TYPE.COPY_IN)
    if e then
        return self:_fail(e)
    end
    -- 服务端期望的格式与列数透给 producer：折叠成一个方法之后，调用方再没有别的途径拿到它
    local cfmt, cncol = pgsql.copy_in_info(pgpack)
    -- 自此服务端已进 COPY IN 模式，下面任何一条失败路径都必须先把它拉出来再返回。
    -- 整段串流收进一个 pcall：会抛的不止 producer——producer 返回的块类型不对时
    -- pack_copy_data 返 nil，紧接着的 srey.send 就 luaL_argerror。只 pcall producer 的话
    -- 那种抛出会掠过下面两处 _copy_fail，被 serial 执行器的 xpcall 吞掉，
    -- 留下服务端停在 copy-in 模式攥着开放事务和表锁，而 err 还是空的
    local ok, more, reason = pcall(self._copy_stream, self, fd, skid, producer, cfmt, cncol)
    if not ok then
        self:_copy_fail(tostring(more))
        return self:_fail("copy_in: " .. tostring(more))
    end
    if not more then
        return self:_copy_fail(reason)-- producer 主动中止
    end
    pack, size = pgsql.pack_copy_done()
    pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return self:_fail(ppub.SEND)
    end
    e = ppub.check_type(pgpack, PGPACK_TYPE.OK)
    if e then
        return self:_fail(e)
    end
    self.affected = pgsql.affected_rows(pgpack)
    return true
end


-- COPY OUT --

---执行 COPY TO STDOUT 查询，一次性返回全部数据
---@param sql string COPY ... TO STDOUT 语句
---@return lightuserdata|false data 数据指针；失败返回 false
---@return integer? size 成功时为字节数
function ctx:copy_out(sql)
    return srey.serial_ret(false, self.serial(self._copy_out, self, sql))
end
function ctx:_copy_out(sql)
    self.err = ""-- 复位:erro() 只反映最近一次操作
    local pack, size = pgsql.pack_query(sql)
    local fd, skid = self.pg:sock_id()
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return self:_fail(ppub.SEND)
    end
    local e = ppub.check_type(pgpack, PGPACK_TYPE.COPY_OUT)
    if e then
        return self:_fail(e)
    end
    return pgsql.copy_out_data(pgpack)
end

---发送 Terminate 消息并关闭连接
function ctx:quit()
    self.serial(self._doquit, self)
end
-- 走锁:Terminate 虽不等响应,但插进别人正在进行的交换会串包,随后的 sync_close 更会
-- 把对方半途的等待直接打断
function ctx:_doquit()
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
---@return boolean ok 切换并重连成功 true（库名超 63 字节时不断连直接返 false，
---原连接与原库名均保持不变）
function ctx:selectdb(database)
    return srey.serial_ret(false, self.serial(self._selectdb, self, database))
end
-- 换库整段在锁内（含 set_db）：它改的库名是连接级状态，搁在锁外的话拿不到锁那次
-- 会留下"库名已换、连接还在旧库上"，之后随便哪次 ping 重连就悄悄换了库
function ctx:_selectdb(database)
    self.err = ""-- 复位:erro() 只反映最近一次操作
    -- 先校验再断连：库名超长时 set_db 保留旧名，若照旧先 quit 就白断一条可用连接，
    -- 还会用原库名重连成功、把切库失败报成功
    if not self.pg:set_db(database) then
        self.err = "pgsql: database name exceeds 63 bytes"
        return false
    end
    self:quit()
    return self:connect()
end

---取消当前正在执行的查询：在独立连接上发送 CancelRequest，服务端处理后主动断开、无响应
---@return boolean ok 发送成功 true；未连接返回 false
function ctx:cancel()
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
---@return boolean ok 成功 true；用户名或密码超 63 字节时 false，两者均保持原值
function ctx:set_userpwd(user, password)
    return self.pg:set_userpwd(user, password)
end

---更新目标数据库名（下次重连时生效）
---@param database string 新数据库名
---@return boolean ok 成功 true；库名超 63 字节时 false，库名保持原值
function ctx:set_db(database)
    return self.pg:set_db(database)
end

---获取当前配置的数据库名
---@return string database 数据库名
function ctx:get_db()
    return self.pg:get_db()
end

---返回服务端就绪状态字符（ASCII 整数）
---@return integer status 73('I') 空闲 / 84('T') 事务中 / 69('E') 失败事务中 / 0 尚未收到 ReadyForQuery（新建或刚重连）
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
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---@return string err 错误描述
function ctx:erro()
    return self.err
end

---返回最近一次受影响行数
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---@return integer rows affected rows
function ctx:affected_rows()
    return self.affected
end

ctx.PACK_TYPE = PGPACK_TYPE
ctx.FORMAT    = PG_FORMAT

return ctx
