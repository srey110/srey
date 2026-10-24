-- MongoDB 客户端（mongo_ctx 类）。
-- 封装 C 层 mongo / mongo.session 模块，提供：连接管理、SCRAM 认证、
-- CRUD 操作（find/insert/update/delete/aggregate/count 等）及会话事务支持。
-- 连接生命周期完全由 Lua 端管理：mongo:try_connect 发起、srey.wait_connect 等待、
-- srey.sync_close 关闭；C 层 mongo 模块只做命令组包/解包。
-- 服务端最低 4.4：SCRAM 认证依赖 saslStart 的 skipEmptyExchange，更早的版本认证必败（同 C 侧 mongo_init）。

local srey = require("lib.srey")
local mongo = require("srey.mongo")
local mongo_session = require("srey.mongo.session")
local utils = require("srey.utils")
local pub = require("lib.conn_pub")-- connect / ping / quit 的共用骨架
-- mongo_ctx：MongoDB 连接上下文，每实例对应一条持久连接。
-- 建链、保活、断开三段继承自 conn_pub，本文件只实现 _connect / _ping / _doquit 三个钩子。
local ctx = class("mongo_ctx", pub)

-- MongoDB OP_MSG 消息标志位（与 C 层 mongo_flags 枚举对应）。
-- 仅 MORETOCOME 已被 C 层 mongo_set_flag 实现；CHECKSUM/EXHAUSTALLOWED 保留常量供协议完整性参考，
-- set_flag() 传入这两者会被忽略并 WARN，不代表已支持 wire checksum / exhaust 游标
ctx.FLAGS = {
    CHECKSUM       = 0x01,
    MORETOCOME     = 0x02,
    EXHAUSTALLOWED = 1 << 16,
}

-- 拿到锁后、发送前复查(同 C 侧 _mongo_lock)：组包之后连接换过，包里带的是已作废事务的上下文，
-- 发到新连接上会被收进那个事务。命中即放掉包、按组包失败处理。mark 为 nil 是包不带事务上下文，不查
---@param mgo any C 层 mongo 句柄
---@param pack lightuserdata 已组好的包，命中时由本函数释放
---@param mark integer? 组包前的 mgo:txn_mark()
---@return boolean moved 连接已换过、包已释放时 true
local function _txn_moved(mgo, pack, mark)
    if nil == mark
        or not mgo:txn_moved(mark) then
        return false
    end
    utils.ud_free(pack)
    WARN("mongo transaction invalidated by reconnect, rollback or begin again.")
    return true
end
-- 连接标识 sk 必须在锁内才读（等应答那支由 _send_wait 现取）:排队期间前一个协程可能已经 ping 重连、换掉了它,
-- 锁外读的是已退休的连接。往退休连接上发,MORETOCOME 路径会被静默丢弃却报成功,
-- 同步路径则空等满一个 netread 超时。组包不受此影响——集合名/库名在组包时已写进包体,
-- 而集合名就在组包那次 C 调用里设，中间没有让出点。
-- more 是写类 pack_* 读包头带回的 MORETOCOME，问包不问 mgo:check_flag，理由见 C 层 mongo_pack_check_flag
local function _wdo(mgoctx, pack, size, more, mark)
    local mgo = mgoctx.mongo
    if _txn_moved(mgo, pack, mark) then
        return false, nil
    end
    if more then
        return srey.send(mgo:sock_id(), pack, size, 0), nil
    end
    local mgopack = mgoctx:_send_wait(pack, size)
    if mgopack then
        -- 写命令同样带 lsid，续期口径与 _rdo 一致；MORETOCOME 那支没有应答，不续
        mgo:session_touch()
    end
    return nil ~= mgopack, mgopack
end
-- 统一"发送 + 按 MORETOCOME 决定是否等待响应"。pack 为 nil(C 层组包被拒)在此一并吸收:
-- 调用方不必各自守卫,新增的 pack_* 绑定也自动受保护。镜像 C 层 coro_utils.c 的 _mongo_send。
-- 串行化落在两个漏斗上（与 C 侧 _mongo_send / _mongo_sendwait 同处），一次覆盖全部命令站点：
-- 各命令函数在此之后只做纯解析、不再有 I/O，锁到漏斗为止与锁整个命令函数等效。
-- MORETOCOME 的只发不等也进锁——不等响应也不能乱序，后面那条 find 得看得见前面这批 insert。
-- connect / ping 另有外层锁，它们走多次往返或绕开漏斗，靠 ref 计数嵌套
---@param mgoctx any 所属 mongo_ctx（提供 serial 执行器与 C 层 mongo 句柄）
---@param more boolean? 包里写着 MORETOCOME（写类 pack_* 的第 3 个返回值），真则只发不等
---@param mark integer? 组包前的 mgo:txn_mark()；包不带事务上下文时不传，理由见 _txn_moved
local function _wsend(mgoctx, pack, size, more, mark)
    if not pack then
        return false, nil
    end
    -- 包在锁外组好，执行器挂不起时会在入锁前抛、没人放包：先用同一判据查，挂不起就放掉包再抛
    srey._check_wait("srey.serial executor", pack)
    local ok, mgopack = srey.serial_ret(nil, mgoctx.serial(_wdo, mgoctx, pack, size, more, mark))
    if nil == ok then
        return false, nil
    end
    return ok, mgopack
end
-- 写命令的两种统一尾块。mgopack 为 nil 是 MORETOCOME 只发不等，那是成功而非失败。
-- 两者不可互换：check_error 返的是受影响文档数而不是 ERR_OK，用 _wsend_n 顶替 _wsend_ok
-- 会把那几个命令的公开返回值从 boolean 变成计数。bulkwrite 的尾块第三种形状，不走这里
---@return boolean ok
---@return integer? n 受影响文档数；MORETOCOME 只发不等时不返
local function _wsend_n(mgoctx, pack, size, more, mark)
    local ok, mgopack = _wsend(mgoctx, pack, size, more, mark)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    local n = mgoctx.mongo:check_error(mgopack)
    if n < 0 then
        return false
    end
    return true, n
end
---@return boolean ok
local function _wsend_ok(mgoctx, pack, size, more, mark)
    local ok, mgopack = _wsend(mgoctx, pack, size, more, mark)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    return mgoctx.mongo:check_error(mgopack) >= 0
end
-- 统一"发送 + 同步等待响应"(不受 MORETOCOME 影响)。组包被拒与网络失败都返回 nil,调用方判一次即可;
-- 错误码校验留给调用方——有的直接把 mgopack 交给上层解析。镜像 C 层 _mongo_call 去掉 check_error 的部分
local function _rdo(mgoctx, pack, size, mark)
    if _txn_moved(mgoctx.mongo, pack, mark) then
        return nil
    end
    local mgopack = mgoctx:_send_wait(pack, size)
    if mgopack then
        -- 应答回来了就说明服务端处理过这条命令；事务中每条命令都带着 lsid,
        -- 服务端那边的会话寿命已被延长,本地跟着记一次 expires_in 才准
        mgoctx.mongo:session_touch()
    end
    return mgopack
end
---@param mgoctx any 所属 mongo_ctx（提供 serial 执行器与 C 层 mongo 句柄）
---@param mark integer? 同 _wsend
local function _rsend(mgoctx, pack, size, mark)
    if not pack then
        return nil
    end
    srey._check_wait("srey.serial executor", pack)-- 理由同 _wsend
    return srey.serial_ret(nil, mgoctx.serial(_rdo, mgoctx, pack, size, mark))
end

-- mongo_session_ctx：会话事务上下文，由 mongo_ctx:startsession() 创建。
local sess_ctx = class("mongo_session_ctx")

---构造函数（内部使用）
---@param mgoctx any 所属 mongo_ctx 对象
---@param session_ud any C 层 mongo.session userdata
function sess_ctx:ctor(mgoctx, session_ud)
    self.mgoctx = mgoctx
    self.session = session_ud
end

---开始事务：递增 txnNumber，构建 lsid + txnNumber 事务选项 BSON，挂载到 mongo->session
---一条连接同时只允许一个活跃事务，该连接上已有别的 session 在事务中时返回 false。
---事务进行中连接重连过即作废：之后带事务上下文的命令一律失败，commit/rollback 都发 abortTransaction
---让服务端当场中止、放锁（commit 返回 false；提交已交出过的 commit 除外，照常重发，见 commit）；要继续须重新 begin（同 C 侧 mongo_begin）
---@return boolean ok 成功 true（该连接上已有别的 session 处于事务中时返回 false）
function sess_ctx:begin()
    return self.session:begin()
end

-- 组包与发送整段在锁内，理由同 C 侧 mongo_commit：组包要读连接当前绑定的 session，
-- 与 _rsend 里那次上锁之间隔着一次不定长排队，锁外组包拿到的是过期状态。
-- 内层 _rsend 再上一次锁，同协程按 ref 嵌套
local function _txn_do(self, opts, optslens, packname, what)
    local mgo = self.mgoctx.mongo
    local sess = self.session
    -- 空事务没发过命令：不发包，本地收尾按成功返回，作废与否都一样(同 C 侧 _mongo_txn_end)
    if sess:empty() then
        sess:done()
        return true
    end
    -- 事务已随重连作废就不能提交：改发 abortTransaction 让服务端当场中止、放锁，本次按失败返回(同 C 侧 _mongo_commit)。
    -- 提交已交出过的不算作废(判据在 C 层 mongo_session_stale)，照常重发提交
    if "pack_commit" == packname
        and sess:stale() then
        _txn_do(self, nil, nil, "pack_abort", "abort(stale)")
        return false
    end
    local pack, size = sess[packname](sess, opts, optslens)-- 读类组包，不带 MORETOCOME
    if not pack then
        WARN("mongo %s packing rejected.", what)
        return false
    end
    local txn = sess:txn()
    local mgopack = _rsend(self.mgoctx, pack, size)
    if not mgopack then
        return false
    end
    -- 等回包期间同一 session 又 begin 过就不 done：绑定与 options 已归新事务(同 C 侧 _mongo_txn_end)
    if txn == sess:txn() then
        sess:done()
    end
    return mgo:check_error(mgopack) >= 0
end
---commit / rollback 的共同流程，两者只差组包用哪个 C 接口和日志里的动作名；主体见 _txn_do，本函数只负责套锁。
---事务状态一路保留到服务端真的回了包为止：组包被拒与网络失败都可能只是这一次不成，留着状态才能重试
---或改走另一条收尾路径，同 C 侧 mongo_commit。
---begin 后还没发过命令的空事务不发包、本地收尾返回 true；
---事务已随重连作废时 commit 改发 abortTransaction 了结并返回 false，rollback 照常发
---@param self any mongo_session_ctx 实例
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@param packname string session 上的组包方法名（"pack_commit" / "pack_abort"）
---@param what string 动作名，仅用于组包被拒时的日志
---@return boolean ok 服务端确认且未报错 true
local function _txn_finish(self, opts, optslens, packname, what)
    return srey.serial_ret(false, self.mgoctx.serial(_txn_do, self, opts, optslens, packname, what))
end

---提交事务；网络失败或组包被拒时保留事务状态供重试，仅服务端响应确认时清理（见 _txn_finish）。
---提交已交给连着的连接、本事务又没发过只发不等的写时，网络失败后先 ping 重连再 commit，会在新连接上按同一事务
---重发提交，由服务端裁决；不满足的重连后按作废处理（同 C 侧 mongo_commit）
---@param opts string|lightuserdata|nil 附加 writeConcern 等 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 提交成功 true
function sess_ctx:commit(opts, optslens)
    return _txn_finish(self, opts, optslens, "pack_commit", "commit")
end

---回滚事务；状态保留与清理时机同 commit（见 _txn_finish）
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 回滚成功 true
function sess_ctx:rollback(opts, optslens)
    return _txn_finish(self, opts, optslens, "pack_abort", "rollback")
end

-- 组包与发送同在锁内，理由同 _txn_do
local function _refresh_do(self)
    local mgo = self.mgoctx.mongo
    local pack, size = self.session:pack_refresh()
    local mgopack = _rsend(self.mgoctx, pack, size)
    if not mgopack then
        return false
    end
    if mgo:check_error(mgopack) < 0 then
        return false
    end
    -- 只能自己续：_rdo 那趟 session_touch 续的是连接当前绑定的会话，
    -- 而 refresh 允许在事务外调，那时连接并没绑这个 session（同 C 侧 mongo_refreshsession）
    self.session:renew()
    return true
end
---会话距超时还剩多少秒。服务端超过 logicalSessionTimeoutMinutes 未见该会话就会回收它，
---据此决定何时调 refresh()。事务中的每条命令都会自动续期，长事务通常不必手动刷
---@return integer secs 剩余秒数；已过期为 0 或负数；超时未知（建连前开的会话、或 hello 没给
---logicalSessionTimeoutMinutes）时恒为 0，refresh 也不改变它
function sess_ctx:expires_in()
    return self.session:expires_in()
end

---刷新会话超时（refreshSessions），延续会话存活时间
---@return boolean ok 刷新成功 true
function sess_ctx:refresh()
    return srey.serial_ret(false, self.mgoctx.serial(_refresh_do, self))
end

-- 组包与发送同在锁内，理由同 _txn_do。pack_endsession 的包恒带 MORETOCOME，_wsend 只发不等
local function _close_do(self)
    local pack, size, more = self.session:pack_endsession()
    _wsend(self.mgoctx, pack, size, more)
end
---结束会话（endSessions，fire-and-forget）并释放 C 层会话内存。
---session:free() 留在锁外：它不碰连接，且无论有没有发出 endSessions 都要释放
function sess_ctx:close()
    -- 服务端的会话记录不随连接消失,重连过也照发 endSessions,漏发就要挂到会话超时才回收
    self.mgoctx.serial(_close_do, self)
    self.session:free()
end

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param db string 初始数据库名
---@param user string? 认证用户名；nil 表示不认证
---@param password string 认证密码；user 非 nil 时必填（C 层 user_pwd 对它是 lpub_check_cstr，nil 或含 NUL 报错）。
---按原样参与认证、不做 SASLprep：含需 NFKC 规范化字符的非 ASCII 密码须由调用方先规范化，否则认证失败
---@param authdb string? 认证数据库；nil 时使用 db
---@param authmod string? SCRAM 算法，默认 "SCRAM-SHA-256"
function ctx:ctor(ip, port, sslname, db, user, password, authdb, authmod)
    local ssl = pub.ssl(sslname)
    self.sslname = sslname
    self.mongo = mongo.new(ip, port, ssl, db)
    if not self.mongo then
        error(string.format("mongo.new failed: %s:%d db=%s", ip, port, tostring(db)), 3)
    end
    self.user = user
    -- 同 password 那道自查：authmod 不在这里挡的话，非字符串要到第一次 _connect 的
    -- pack_auth_first 才抛，那一抛连接没人关，还把 conn_pub 的 established 留成陈旧的 true
    if nil ~= authmod
        and "string" ~= type(authmod) then
        error("mongo ctor: authmod must be a string", 3)
    end
    self.authmod = authmod or "SCRAM-SHA-256"
    if user then
        -- 先自查再下发：user_pwd 的第 3 参在 C 层是 lpub_check_cstr，password 为 nil 会先抛
        -- "bad argument #3 ... (string expected, got nil)",压根走不到下面那句带解释的 error
        if "string" ~= type(password) then
            error("mongo ctor: password is required when user is given", 3)
        end
        if not self.mongo:user_pwd(user, password) then
            error("mongo user_pwd failed: user or password too long", 3)
        end
        if not self.mongo:authdb(authdb or db) then
            error(string.format("mongo authdb failed: %s too long", tostring(authdb or db)), 3)
        end
    end
    -- 串行化执行器由 conn_pub 建，锁点在 _wsend / _rsend 两个漏斗上。它只保证单条命令
    -- 原子，不保证事务原子——要事务隔离请给事务用独占连接（与 C 侧同一结论）
    pub.init(self, self.mongo)
end

-- conn_pub 的建链钩子：TCP 连接（含可选 SSL 握手）→ 发 hello → 可选 SCRAM 身份验证。
-- 认证走 srey.send + wait_handshaked 绕开了漏斗，而那期间连接处于 AUTH 态，
-- 别人的普通命令挤进来会被当成认证响应解析——靠 conn_pub 把整段包在锁内
function ctx:_connect()
    local sk = self.mongo:try_connect()
    if not sk.valid then
        return false
    end
    -- 以下各处失败只管返 false，关连接由 conn_pub 在锁内做
    if not srey.wait_connect(sk, SSL_NAME.NONE ~= self.sslname or nil) then
        return false
    end
    -- 不解绑上一条连接上开的事务（解绑后的写会脱离事务自动提交）：try_connect 换了 skid，
    -- C 层组包据此判它已作废，同 C 侧 _mongo_do_connect。hello 恒不带事务上下文（见 C 层 mongo_pack_hello）
    local pack, size = self.mongo:pack_hello()-- 包头恒不带 MORETOCOME
    local mgopack = _rsend(self, pack, size)
    if not mgopack then
        return false
    end
    if self.mongo:check_error(mgopack) < 0 then
        return false
    end
    self.mongo:parse_hello(mgopack)-- 记下会话超时分钟数，startsession 本地开会话时用
    if self.user then
        -- pack_auth_first 先把 scram 交给网络线程装上(连同切 AUTH 态)再交出包，故包必须在它之后发
        local authpack, authsize = self.mongo:pack_auth_first(sk, self.authmod)
        local ok = false
        if authpack and srey.send(sk, authpack, authsize, 0) then
            ok = srey.wait_handshaked(sk)
        end
        if not ok then
            return false
        end
    end
    return true
end

---内部 ping（isMaster / ping 命令），不自动重连
---@return boolean ok 服务端响应成功 true（仅供 ping() 内部调用，不要直接调用；调用方须已持锁）
function ctx:_ping()
    local pack, size = self.mongo:pack_ping()
    local mgopack = _rsend(self, pack, size)
    if not mgopack then
        return false
    end
    return self.mongo:check_error(mgopack) >= 0
end

---切换当前数据库。成功时 C 层顺带清空当前集合名；本模块的命令都显式带集合参数，不受影响
---@param name string 数据库名
---@return boolean ok 成功 true；库名超 63 字节返 false 且当前库不变
function ctx:db(name)
    return self.mongo:db(name)
end

---切换当前集合
---@param name string 集合名
---@return boolean ok 成功 true；集合名超 63 字节返 false 且当前集合不变
function ctx:collection(name)
    return self.mongo:collection(name)
end

---置上消息标志位（C 层当前仅实现 MORETOCOME；CHECKSUM/EXHAUSTALLOWED 未实现，设置无效果）。
---置上就一直有效，直到自己调 clear_flag——不是只管下一条命令：此后每条写命令都只发不等，
---服务端的失败（重复键、校验不过）因为没有响应可解析而一律报成功；要等回包的读命令组包时包头恒不带。
---标志挂在连接上而不是命令上，多协程共用一条连接时别人的写也会跟着变成 fire-and-forget，
---批量写完请及时清掉
---@param flag integer ctx.FLAGS 枚举值的按位或，按位置上、不清已有的位；清除只能用 clear_flag。
---0 合法但什么都不置（还原惯用法 set_flag(clear_flag()) 传回的旧值可能就是 0）
function ctx:set_flag(flag)
    local known = ctx.FLAGS.CHECKSUM | ctx.FLAGS.MORETOCOME | ctx.FLAGS.EXHAUSTALLOWED
    if 0 ~= (flag & ~known) then
        WARN("mongo set_flag: flag %s has bits outside mongo_flags, ignored.", tostring(flag))
        return
    end
    if 0 ~= (flag & ~ctx.FLAGS.MORETOCOME) then
        WARN("mongo set_flag: only MORETOCOME is implemented by mongo_set_flag, other bits are dropped.")
    end
    self.mongo:set_flag(flag)
end

---清除所有消息标志位
---@return integer old 清除前的旧标志位
function ctx:clear_flag()
    return self.mongo:clear_flag()
end

-- ---- 写操作（返回 true, n / false）----

---插入文档
---@param col string 集合名
---@param docs lightuserdata BSON 数组格式的文档列表指针
---@param dlens integer docs 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
---@return integer? n 成功时为 nInserted
function ctx:insert(col, docs, dlens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size, more = self.mongo:pack_insert(col, docs, dlens, opts, optslens)
    return _wsend_n(self, pack, size, more, mark)
end

---更新文档
---@param col string 集合名
---@param updates lightuserdata BSON 数组格式的更新列表指针
---@param ulens integer updates 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
---@return integer? n 成功时为应答里的 n，即匹配到的文档数（matched，不是 nModified——值未变化的 $set 同样计入）
function ctx:update(col, updates, ulens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size, more = self.mongo:pack_update(col, updates, ulens, opts, optslens)
    return _wsend_n(self, pack, size, more, mark)
end

---删除文档
---@param col string 集合名
---@param deletes lightuserdata BSON 数组格式的删除列表指针
---@param dlens integer deletes 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
---@return integer? n 成功时为 nDeleted
function ctx:delete(col, deletes, dlens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size, more = self.mongo:pack_delete(col, deletes, dlens, opts, optslens)
    return _wsend_n(self, pack, size, more, mark)
end

---删除集合（drop）
---@param col string 集合名；组包时写进包体
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
function ctx:drop(col, opts, optslens)
    local pack, size, more = self.mongo:pack_drop(col, opts, optslens)
    return _wsend_ok(self, pack, size, more)
end

---批量写操作（bulkWrite，MongoDB 8.0+）。全库唯一不收 col 的集合类命令：
---bulkWrite 是集群级命令，目标集合逐条写在 nsinfo 里，不走连接级集合名
---@param ops lightuserdata BSON 数组格式操作列表指针
---@param opsz integer ops 字节数
---@param nsinfo lightuserdata BSON 数组格式命名空间信息指针
---@param nsz integer nsinfo 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|true|nil mgopack 普通模式返回响应包指针，仅在本协程下次 yield 前有效，
---需保留请自行拷贝；MORETOCOME fire-and-forget 成功返回 true；组包被拒或网络失败返回 nil，服务端报错同样返回应答包
function ctx:bulkwrite(ops, opsz, nsinfo, nsz, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size, more = self.mongo:pack_bulkwrite(ops, opsz, nsinfo, nsz, opts, optslens)
    local ok, mgopack = _wsend(self, pack, size, more, mark)
    if not ok then
        return nil
    end
    if not mgopack then
        return true
    end
    return mgopack
end

---创建索引
---@param col string 集合名
---@param indexes lightuserdata BSON 数组格式索引定义列表指针
---@param ilens integer indexes 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
function ctx:createindexes(col, indexes, ilens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size, more = self.mongo:pack_createindexes(col, indexes, ilens, opts, optslens)
    return _wsend_ok(self, pack, size, more, mark)
end

---删除索引
---@param col string 集合名
---@param indexes lightuserdata BSON 数组格式索引名列表指针
---@param ilens integer indexes 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
function ctx:dropindexes(col, indexes, ilens, opts, optslens)
    local pack, size, more = self.mongo:pack_dropindexes(col, indexes, ilens, opts, optslens)
    return _wsend_ok(self, pack, size, more)
end

-- ---- 读操作（返回 mgopack lightuserdata 或 nil）----

---查询文档（find，支持游标分页）；通过 mongo.doc + bson.iter 遍历结果，cursorid 判断是否有后续游标
---@param col string 集合名
---@param filter lightuserdata? BSON 过滤条件；nil 表示全部
---@param flens integer? filter 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项（limit/skip/sort 等）
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针，仅在本协程下次 yield 前有效，
---需保留请自行拷贝；组包被拒或网络失败返回 nil，服务端报错（ok:0）照样返回应答包，须自行判 ok
function ctx:find(col, filter, flens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size = self.mongo:pack_find(col, filter, flens, opts, optslens)
    local mgopack = _rsend(self, pack, size, mark)
    return mgopack
end

---聚合查询（aggregate）
---@param col string 集合名
---@param pipeline lightuserdata BSON 数组格式聚合管道指针
---@param pllens integer pipeline 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针，仅在本协程下次 yield 前有效，
---需保留请自行拷贝；组包被拒或网络失败返回 nil，服务端报错（ok:0）照样返回应答包，须自行判 ok
function ctx:aggregate(col, pipeline, pllens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size = self.mongo:pack_aggregate(col, pipeline, pllens, opts, optslens)
    local mgopack = _rsend(self, pack, size, mark)
    return mgopack
end

---获取游标后续批次（getMore）
---@param col string 集合名；必须与开游标那条命令用的一致，组包时写进包体
---@param cursorid integer 上次 find / aggregate 返回的游标 ID
---@param opts string|lightuserdata|nil 附加 BSON 选项(batchSize maxTimeMS comment)；集合取 col，别再带 collection
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针，仅在本协程下次 yield 前有效，
---需保留请自行拷贝；组包被拒或网络失败返回 nil，服务端报错（ok:0）照样返回应答包，须自行判 ok
function ctx:getmore(col, cursorid, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size = self.mongo:pack_getmore(col, cursorid, opts, optslens)
    local mgopack = _rsend(self, pack, size, mark)
    return mgopack
end

---关闭游标（killCursors）
---@param col string 集合名
---@param cursorids lightuserdata BSON 数组格式游标 ID 列表指针
---@param cslens integer cursorids 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
function ctx:killcursors(col, cursorids, cslens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size, more = self.mongo:pack_killcursors(col, cursorids, cslens, opts, optslens)
    return _wsend_ok(self, pack, size, more, mark)
end

---去重查询（distinct）
---@param col string 集合名
---@param key string 去重字段名
---@param query lightuserdata? BSON 过滤条件；nil 表示全部
---@param qlens integer? query 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针，仅在本协程下次 yield 前有效，
---需保留请自行拷贝；组包被拒或网络失败返回 nil，服务端报错（ok:0）照样返回应答包，须自行判 ok
function ctx:distinct(col, key, query, qlens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size = self.mongo:pack_distinct(col, key, query, qlens, opts, optslens)
    local mgopack = _rsend(self, pack, size, mark)
    return mgopack
end

---原子查找并修改/删除（findAndModify）
---@param col string 集合名
---@param query lightuserdata? BSON 过滤条件；nil 表示不过滤
---@param qlens integer? query 字节数
---@param remove integer 非零时执行删除，零时执行 update
---@param pipeline integer 非零时 update 为聚合数组
---@param update lightuserdata? BSON 更新文档或聚合管道；删除时可 nil
---@param ulens integer? update 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针，仅在本协程下次 yield 前有效，
---需保留请自行拷贝；组包被拒或网络失败返回 nil，服务端报错（ok:0）照样返回应答包，须自行判 ok
function ctx:findandmodify(col, query, qlens, remove, pipeline, update, ulens, opts, optslens)
    local mark = self.mongo:txn_mark()
    local pack, size = self.mongo:pack_findandmodify(col, query, qlens, remove, pipeline, update, ulens, opts, optslens)
    local mgopack = _rsend(self, pack, size, mark)
    return mgopack
end

---文档计数（count）。不参与事务：连接绑着事务时也按事务外执行、看不到事务里未提交的写，
---事务内计数用 aggregate 的 $count 阶段（同 C 侧 mongo_count）
---@param col string 集合名
---@param query lightuserdata? BSON 过滤条件；nil 表示全部计数
---@param qlens integer? query 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return integer|false n 计数整数；失败返回 false
function ctx:count(col, query, qlens, opts, optslens)
    local pack, size = self.mongo:pack_count(col, query, qlens, opts, optslens)
    local mgopack = _rsend(self, pack, size)
    if not mgopack then
        return false
    end
    local n = self.mongo:check_error(mgopack)
    if n < 0 then
        return false
    end
    return n
end

-- ---- 会话 ----

---启动逻辑会话：lsid 在本地生成，不发 startSession、不挂起，服务端第一次见到该 lsid 时自动建会话，
---故连接有问题要到第一次使用时才暴露。超时分钟数取最近一次建连时 hello 回的值，建连前调用为 0（超时未知），
---要读超时须先建连再开会话
---@return any|nil session mongo_session_ctx 实例；本地生成 UUID 失败返回 nil
function ctx:startsession()
    local session_ud = mongo_session.new(self.mongo)
    if not session_ud then
        return nil
    end
    return sess_ctx.new(self, session_ud)
end

-- conn_pub 的断开钩子：MongoDB 无专用断开命令，直接 close socket
function ctx:_doquit()
    local sk = self.mongo:sock_id()
    srey.sync_close(sk)
end

return ctx
