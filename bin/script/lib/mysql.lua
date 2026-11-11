-- MySQL 客户端（mysql_ctx 类）。
-- 封装 C 层 mysql 模块，提供：连接管理、ping 保活、
-- 普通查询（query）、预处理语句（prepare）及会话控制。
-- 查询结果通过 mysql.reader 惰性迭代，避免大结果集一次性复制到 Lua。

local srey  = require("lib.srey")
local stmt  = require("lib.mysql_stmt")
local mysql = require("srey.mysql")
local reader = require("srey.mysql.reader")
local pub   = require("lib.conn_pub")-- connect / ping / quit 的共用骨架
local mpub  = require("lib.mysql_pub")-- 与 mysql_stmt.lua 共用的包类型与判型，见该模块头部
local MYSQL_PACK_TYPE = mpub.PACK_TYPE

-- mysql_ctx：MySQL 连接上下文，每实例对应一条持久连接。
-- 建链、保活、断开三段继承自 conn_pub，本文件只实现 _connect / _ping / _doquit 三个钩子。
local ctx = class("mysql_ctx", pub)

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param user string 用户名，最长 128 字节
---@param password string 密码，最长 256 字节。caching_sha2_password 账号在明文连接上遇到服务端缓存未命中（如服务端重启后首连）
---认证失败：明文下不做 RSA 公钥交换。出路：配 SSL、先经 SSL 登录一次让服务端缓存口令，或换 mysql_native_password 账号(MySQL 8.4 默认关、9.0 已移除该插件)
---@param database string 初始数据库，最长 256 字节
---@param charset string 字符集（如 "utf8mb4"）
---@param maxpk integer? 单包最大字节数，0 使用默认
function ctx:ctor(ip, port, sslname, user, password, database, charset, maxpk)
    local ssl = pub.ssl(sslname)
    self.mysql = mysql.new(ip, port, ssl, user, password, database, charset, maxpk)
    if not self.mysql then
        error(string.format("mysql.new failed: %s:%d db=%s", ip, port, tostring(database)), 3)
    end
    -- MySQL 半双工，一条命令的响应没收完就发下一条会串包，而 mysql_ctx 的解析状态
    -- 又是每连接一份，交错即互相覆盖——串行化执行器由 conn_pub 建
    pub.init(self, self.mysql)
end

-- conn_pub 的建链钩子：TCP 连接 + MySQL 握手（Handshake/AuthResponse），成功后 skid 设为会话键
function ctx:_connect()
    if not self.mysql:try_connect() then
        return false
    end
    local sk = self.mysql:sock_id()
    -- SSL 由协议层在握手中途发起并接管，不投 SSLEXCHANGED，这里不带 ssl(同 pgsql)；greeting 位置的 ERR 也由下面收到
    if not srey.wait_connect(sk) then
        return false
    end
    -- 握手失败的 ERR 包体记到连接上(之后 erro() 取得到错误号)，同时打出来；同 C 侧 _mysql_do_connect
    local ok, err, elens = srey.wait_handshaked(sk)
    if not ok and err then
        WARN("%s", self.mysql:record_handshake(err, elens))
    end
    return ok
end

---切换当前数据库（COM_INIT_DB）。池里借出的连接换了库要在归还前换回去，reset_session 不管库名
---@param database string 目标数据库名
---@return boolean ok 切换成功 true（库名超 256 字节时不发包直接返 false）
---设置服务端 RSA 公钥（PEM），明文连接上完整认证时用它加密口令，说明同 C 侧 mysql_set_pubkey。
---与 connect 同在 self.serial 里排队：重连握手要读它
---@param pem string? 公钥 PEM；nil 或空串清掉已设的公钥
---@return boolean ok 成功 true；超长、不是 RSA 公钥或编译时没开 SSL 时 false
function ctx:set_pubkey(pem)
    return srey.serial_ret(false, self.serial(self.mysql.set_pubkey, self.mysql, pem))
end

---允许明文连接上向服务端要 RSA 公钥（默认不允许），说明同 C 侧 mysql_set_pubkey_request。排队理由同 set_pubkey
---@param on boolean true 允许
---@return boolean ok 成功 true；编译时没开 SSL 时 false
function ctx:set_pubkey_request(on)
    return srey.serial_ret(false, self.serial(self.mysql.set_pubkey_request, self.mysql, on))
end

function ctx:selectdb(database)
    return srey.serial_ret(false, self.serial(self._selectdb, self, database))
end
function ctx:_selectdb(database)
    local pack, size = self.mysql:pack_selectdb(database)
    if nil == pack then
        return false
    end
    return mpub.request_ok(self, pack, size)
end

-- conn_pub 的探活钩子：COM_PING，不自动重连。为什么必须正向判型见 mysql_pub.request_ok
function ctx:_ping()
    local pack, size = self.mysql:pack_ping()
    return mpub.request_ok(self, pack, size)
end

---收齐一次请求的全部响应包（多语句 / CALL 会产生多个结果集），query 与 stmt:execute 共用
---@param sk userdata 发出请求那条连接的标识（_send_wait 带出），续读须用同一条
---@param mpack lightuserdata 首个响应包
---@return (_mysql_reader_ctx|boolean)[]|nil results 结果集数组（元素 reader=SELECT 结果集 / true=OK 包 / false=ERR 包）；中途断连或结果集异常返回 nil
function ctx:_read_results(sk, mpack)
    local results = {}
    local failed = false
    while true do
        self.mysql:record(mpack)-- 连接级 erro / last_id / affected_rows 只在锁内由收应答的一方落
        local pktype, more = mysql.pack_type(mpack)-- more 须在 reader.new 前读
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
        mpack = self:_recv_wait(sk)
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
---否则 ping 失败自动重连会按旧库名握手，之后未限定库名的语句全部打到原库上。
---接收侧不拼 16MB 续传包：单个包载荷达 16MB-1（如超大的行）即按协议错误断连，返回 nil
---@param sql string SQL 语句
---@param mbind any? mysql_bind_ctx 查询属性绑定上下文；服务端不支持查询属性（MySQL 8.0.23 之前、MariaDB）时被忽略、不报错
---@return (_mysql_reader_ctx|boolean)[]|nil results 结果集数组（元素 reader=SELECT 结果集 / true=OK 包 / false=ERR 包）；网络失败或多结果集中途断连返回 nil
function ctx:query(sql, mbind)
    return srey.serial_ret(nil, self.serial(self._query, self, sql, mbind))
end
-- 锁覆盖到 _read_results 的续读循环为止：多结果集是"一次请求多个响应"，
-- 中途放别人进来，它的响应会被我们的 syn_recv 收走
function ctx:_query(sql, mbind)
    local pack, size = self.mysql:pack_query(sql, mbind)
    if not pack then
        return nil-- 只有载荷超 16MB 这一种，C 侧已带字节数打过 WARN
    end
    local mpack, sk = self:_send_wait(pack, size)
    if not mpack then
        return nil
    end
    return self:_read_results(sk, mpack)
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
        return false-- 同 _query
    end
    local mpack = self:_send_wait(pack, size)
    if not mpack then
        return false
    end
    self.mysql:record(mpack)-- prepare 失败时的 ERR 包
    -- 必须正向判 MPACK_STMT_PREPARE 而不是"非 ERR 即成功"：连接错位时收到的可能是残留的 OK 包，
    -- 拿它建 stmt 会在 ctor 里抛错，被 serial 记一条 ERROR 后才按失败返回
    if MYSQL_PACK_TYPE.MPACK_STMT_PREPARE ~= mysql.pack_type(mpack) then
        return false
    end
    return stmt.new(self, mpack)
end

-- conn_pub 的断开钩子：COM_QUIT 不等响应，发完直接关
function ctx:_doquit()
    local sk = self.mysql:sock_id()
    if not sk.valid then
        return
    end
    local pack, size = self.mysql:pack_quit()
    srey.send(sk, pack, size, 0)
    srey.sync_close(sk)
end

---连接池归还时调：服务端还处在事务里（见 in_trans）就回滚；autocommit 与连上时不同就还原，
---不然下一个人不 COMMIT 的写会在它归还时被回滚掉。别的会话状态（selectdb 换的库、SET 的会话变量、
---LOCK TABLES、临时表）这里不管，借用者要在归还前自己还原
---@return boolean ok 本来就干净或都还原成功；false 时连接池会断开这条连接
function ctx:reset_session()
    if self.mysql:in_trans() then
        local rs = self:query("ROLLBACK")
        if nil == rs or true ~= rs[1] or self.mysql:in_trans() then
            return false
        end
    end
    local cur, init = self.mysql:autocommit()
    if cur == init then
        return true
    end
    local rs = self:query(init and "SET autocommit=1" or "SET autocommit=0")
    cur = self.mysql:autocommit()
    return nil ~= rs and true == rs[1] and cur == init
end

---服务端是否处在事务里：最近一个 OK / EOF 包带 SERVER_STATUS_IN_TRANS（含 autocommit=0 下已执行过语句），
---或之后的命令回了 ERR（说不准，按在事务里算）；断开后为 false。读取时机同 erro
---@return boolean
function ctx:in_trans()
    return self.mysql:in_trans()
end

---返回服务端版本字符串（握手阶段获取）
---@return string version 服务端版本
function ctx:version()
    return self.mysql:version()
end

---返回本连接的命令最近收到的 ERR 包带的错误信息并清除错误状态，成功的命令不清它，握手失败的 ERR 也记；
---读取时机见 conn_pub 文件头（读取即清除，更要紧挨着命令读——别的协程先读一次就把它清空了）
---@return string err 错误描述（服务端文案，随版本与 locale 变）
---@return integer code MySQL 错误号；无错误时为 0。要区分可重试（1213 死锁 / 1205 锁等待超时）
---与不可重试（1062 重复键）只能靠它，别去匹配文案
function ctx:erro()
    return self.mysql:erro()
end

---返回最近一个命令 OK 包带的自增 ID（非 INSERT 语句为 0，ERR 包与结果集不改它）；读取时机见 conn_pub 文件头。
---多语句时是最后一个 OK 包的
---@return integer|false id last insert id；超出 int64 时为 false 并打 WARN（同 reader:integer）
function ctx:last_id()
    return self.mysql:last_id()
end

---返回最近一个命令 OK 包带的影响行数（ERR 包与结果集不改它）；读取时机见 conn_pub 文件头。
---多语句时是最后一个 OK 包的
---@return integer|false rows affected rows；超出 int64 时为 false 并打 WARN
function ctx:affected_rows()
    return self.mysql:affected_rows()
end

return ctx
