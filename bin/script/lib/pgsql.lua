-- PostgreSQL 客户端（pgsql_ctx 类）。
-- 封装 C 层 pgsql 模块，提供：连接管理、ping 保活、
-- 简单查询（query）、预处理语句（prepare）及 COPY IN/OUT 数据流操作。
-- 查询结果通过 pgsql.reader 惰性迭代，避免大结果集一次性复制到 Lua。
-- 异步通知（LISTEN/NOTIFY）由 srey.on_recved 回调接收，不在此层处理。

local srey   = require("lib.srey")
local stmt   = require("lib.pgsql_stmt")
local pgsql  = require("srey.pgsql")
local reader = require("srey.pgsql.reader")
local ppub   = require("lib.pgsql_pub")-- 失败原因与 err 契约，见该模块头部
local pub    = require("lib.conn_pub")-- connect / ping / quit 的共用骨架
local PGPACK_TYPE = ppub.PACK_TYPE
local PG_FORMAT = ppub.FORMAT

-- pgsql_ctx：PostgreSQL 连接上下文，每实例对应一条持久连接。
-- 建链、保活、断开三段继承自 conn_pub，本文件只实现 _connect / _ping / _doquit 三个钩子。
local ctx = class("pgsql_ctx", pub)

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param user string 用户名
---@param password string 密码
---@param database string 数据库名
function ctx:ctor(ip, port, sslname, user, password, database)
    local ssl = pub.ssl(sslname)
    self.pg = pgsql.new(ip, port, ssl, user, password, database)
    if not self.pg then
        error(string.format("pgsql.new failed: %s:%d db=%s", ip, port, tostring(database)), 2)
    end
    self.affected = 0
    self.err = ""
    self.ip = ip
    self.port = port
    self.sslname = sslname
    -- 一条 pgsql 命令要读到 ReadyForQuery 才算完，copy_in 更是整段会话，交错会让整条
    -- 连接错位——串行化执行器由 conn_pub 建
    pub.init(self, self.pg)
end

-- conn_pub 的建链钩子：TCP 连接 + PostgreSQL 握手，成功后 skid 设为会话键
function ctx:_connect()
    if not self.pg:try_connect() then
        return false
    end
    local fd, skid = self.pg:sock_id()
    if not srey.wait_connect(fd, skid) then
        return false
    end
    local ok, _, _ = srey.wait_handshaked(fd, skid)
    return ok
end

-- conn_pub 的探活钩子：发 "SELECT 1" 简单查询，不自动重连
function ctx:_ping()
    local pack, size = pgsql.pack_query("SELECT 1")
    return ppub.request_ok(self, pack, size, PGPACK_TYPE.OK)
end

-- _fail / _reset 两个类逐字相同，实现落在 ppub 一处（说明见那边）
ctx._fail = ppub.fail
ctx._reset = ppub.reset

---执行简单查询（Query 协议）。
---一条 SQL 里用 `;` 分隔多条语句时，服务端按语句逐条应答，返回数组每条语句一个元素。
---多语句是隐式单事务：任一条出错则整体回滚，本函数返回 false，前面成功的结果一并作废。
---结果恒按文本格式解析：简单查询协议服务端只以文本应答，要二进制结果走 prepare() / execute()
---@param sql string SQL 语句
---@return (_pgsql_reader_ctx|integer)[]|false results 每条语句一个元素：有结果集给 reader，
---无结果集（INSERT/UPDATE 等）给该条的影响行数；失败返回 false，原因走 erro()。
---空 SQL / 纯注释这类服务端不回 CommandComplete 的语句拿到空表，取元素前先判 #results。
---所有结果攒齐才返回，峰值内存是各结果集之和，别拿它跑几百条语句拼成的脚本。
---别把 COPY TO STDOUT 和别的语句拼在一条 query 里：包类型会被换掉，前面已提交的结果
---连同行一起丢。COPY FROM STDIN 走独立包，不影响下标。
---注意与 pgsql_stmt_ctx:execute 形状不同：那边一次只有一条语句，直接给 reader 不套数组
function ctx:query(sql)
    return srey.serial_ret(false, self.serial(self._query, self, sql))
end
function ctx:_query(sql)
    self:_reset()
    local pack, size = pgsql.pack_query(sql)
    local pgpack = ppub.request(self, pack, size, PGPACK_TYPE.OK)
    if not pgpack then
        return false
    end
    self.affected = pgsql.affected_rows(pgpack)-- 连接级"最近一次"，多语句时是最后一条
    local rs = {}
    -- affected 为 0 在 Lua 里仍是真值，不会被 or 吞掉
    for i = 1, pgsql.result_count(pgpack) do
        rs[i] = reader.at(pgpack, i, PG_FORMAT.TEXT) or pgsql.affected_at(pgpack, i)
    end
    return rs
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
    self:_reset()
    local pack, size = pgsql.pack_stmt_prepare(name, sql, nparam or 0, oids)
    if not ppub.request(self, pack, size, PGPACK_TYPE.OK) then
        return false
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
-- producer 抛出或返回的值由业务代码决定，可能是个带 __tostring 的对象，而那个元方法自己
-- 也会抛（返回非字符串同样抛）。_copy_fail 与它的调用方是把服务端拉出 COPY IN 模式的唯一
-- 路径，转字符串在那里失手就等于谁也拉不出来了，故一律走这里兜住
local function _safe_str(v)
    if nil == v then
        return ""
    end
    local ok, s = pcall(tostring, v)
    return ok and s or "copy aborted"
end
-- 发 CopyFail 并等服务端确认。中止本身成功时返回 (true, 服务端文本)——CopyFail 的正常
-- 应答就是 ErrorResponse，所以"中止成功"也走 ERR 包，且成功路径不写 err（写了会让
-- erro() 把正常中止报成失败）。msg 一律转字符串再交出去：本函数是把服务端拉出 COPY IN
-- 模式的唯一手段，在这里抛就等于谁也拉不出来了
function ctx:_copy_fail(msg)
    local pack, size = pgsql.pack_copy_fail(_safe_str(msg))
    -- 这里期望的类型就是 ERR：CopyFail 的正常应答即 ErrorResponse，收到 OK 说明服务端不在 COPY IN
    -- 模式，放过去会让真正的 ErrorResponse + ReadyForQuery 留在流里错位一格。判定口径见 ppub.check_type
    local pgpack = ppub.request(self, pack, size, PGPACK_TYPE.ERR)
    if not pgpack then
        return false
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
    self:_reset()
    if "function" ~= type(producer) then
        return self:_fail("copy_in: producer must be a function")
    end
    local pack, size = pgsql.pack_query(sql)
    local pgpack = ppub.request(self, pack, size, PGPACK_TYPE.COPY_IN)
    if not pgpack then
        return false
    end
    -- fd/skid 下面串流还要用，单独取一次；ppub.request 内部那次只服务它自己
    local fd, skid = self.pg:sock_id()
    -- 服务端期望的格式与列数透给 producer：折叠成一个方法之后，调用方再没有别的途径拿到它
    local cfmt, cncol = pgsql.copy_in_info(pgpack)
    -- 自此服务端已进 COPY IN 模式，任何一条失败路径都必须先把它拉出来再返回。
    -- 整段串流收进一个 pcall 而不是只包 producer：会抛的不止它，漏了就把服务端
    -- 留在 copy-in 模式上攥着开放事务和表锁，而 err 还是空的
    local ok, more, reason = pcall(self._copy_stream, self, fd, skid, producer, cfmt, cncol)
    if not ok then
        self:_copy_fail(more)
        return self:_fail("copy_in: " .. _safe_str(more))
    end
    if not more then
        return self:_copy_fail(reason)-- producer 主动中止
    end
    pack, size = pgsql.pack_copy_done()
    pgpack = ppub.request(self, pack, size, PGPACK_TYPE.OK)
    if not pgpack then
        return false
    end
    self.affected = pgsql.affected_rows(pgpack)
    return true
end


-- COPY OUT --

---执行 COPY TO STDOUT 查询，一次性返回全部数据。
---与本文件各 pack_* 的返回值形状相同但语义相反：data 是响应包内部的**借用**指针，
---调用方既不拥有它、也不能对它调 utils.ud_free（会二次释放）；且仅在本协程下次挂起前有效，
---下次 resume 时框架连同响应包一起释放，需要保留请先 srey.ud_str 拷出来
---@param sql string COPY ... TO STDOUT 语句
---@return lightuserdata|false data 数据指针（借用，勿释放）；失败返回 false
---@return integer? size 成功时为字节数
function ctx:copy_out(sql)
    return srey.serial_ret(false, self.serial(self._copy_out, self, sql))
end
function ctx:_copy_out(sql)
    self:_reset()
    local pack, size = pgsql.pack_query(sql)
    local pgpack = ppub.request(self, pack, size, PGPACK_TYPE.COPY_OUT)
    if not pgpack then
        return false
    end
    self.affected = pgsql.affected_rows(pgpack)-- COPY OUT 的 "COPY N"
    return pgsql.copy_out_data(pgpack)
end

-- conn_pub 的断开钩子：Terminate 不等响应，发完直接关
function ctx:_doquit()
    local fd, skid = self.pg:sock_id()
    if INVALID_SOCK == fd then
        return
    end
    local pack, size = pgsql.pack_terminate()
    srey.send(fd, skid, pack, size, 0)
    srey.sync_close(fd, skid)
end

---切换数据库：关闭当前连接 → 更新库名 → 重连（重连后旧 prepare 语句失效；
---走的是 quit + connect，两边各让代次 +1，故一次切库代次共 +2）
---@param database string 目标数据库名
---@return boolean ok 切换并重连成功 true（库名超 63 字节时不断连直接返 false，
---原连接与原库名均保持不变）
function ctx:selectdb(database)
    return srey.serial_ret(false, self.serial(self._selectdb, self, database))
end
-- 换库整段在锁内（含 set_db）：它改的库名是连接级状态，搁在锁外的话拿不到锁那次
-- 会留下"库名已换、连接还在旧库上"，之后随便哪次 ping 重连就悄悄换了库
function ctx:_selectdb(database)
    self:_reset()
    -- 先校验再断连：库名超长时 set_db 保留旧名，若照旧先 quit 就白断一条可用连接，
    -- 还会用原库名重连成功、把切库失败报成功
    if not self.pg:set_db(database) then
        self.err = "pgsql: database name exceeds 63 bytes"
        return false
    end
    self:quit()
    -- connect 自己不写 err,失败时这里补上:否则返 false 而 erro() 是空串,
    -- 调用方既不知道原因,也看不出自己现在处于哪个库
    if not self:connect() then
        return self:_fail("pgsql: reconnect failed after switching database")
    end
    return true
end

---取消当前正在执行的查询：在独立连接上发送 CancelRequest，服务端处理后主动断开、无响应
---@return boolean ok 发送成功 true；未连接、或握手还没拿到 BackendKeyData（pid 未就绪）返回 false
function ctx:cancel()
    local fd = self.pg:sock_id()
    if INVALID_SOCK == fd then
        return false
    end
    if SSL_NAME.NONE ~= self.sslname then
        WARN("pgsql cancel: CancelRequest sent in plaintext (BackendKeyData pid+key exposed); SSL cancel not supported")
    end
    local cfd, cskid = srey.connect(PACK_TYPE.NONE, SSL_NAME.NONE, self.ip, self.port)
    if INVALID_SOCK == cfd then
        return false
    end
    -- 组包必须排在 connect 之后：connect 会挂起，先组包发出去的可能是已被重连换掉的
    -- 旧 pid/key。组包与 send 之间没有挂起点，快照到发出是原子的；
    -- 撞上"重连中、握手未完成"那档由 pid 为 0 被绑定层拒掉
    local pack = self.pg:pack_cancel()
    if not pack then
        srey.close(cfd, cskid)
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

---返回最近一次受影响行数。SELECT 也会刷新它（值为该 SELECT 返回的行数），
---要按语句取写操作的行数请读 query 返回数组里的整数元素
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---@return integer rows affected rows
function ctx:affected_rows()
    return self.affected
end

return ctx
