-- MongoDB 客户端（mongo_ctx 类）。
-- 封装 C 层 mongo / mongo.session 模块，提供：连接管理、SCRAM 认证、
-- CRUD 操作（find/insert/update/delete/aggregate/count 等）及会话事务支持。
-- 连接生命周期完全由 Lua 端管理：mongo:try_connect 发起、srey.wait_connect 等待、
-- srey.close 关闭；C 层 mongo 模块只做命令组包/解包。

local srey = require("lib.srey")
local mongo = require("mongo")
local mongo_session = require("mongo.session")
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

-- fd/skid 必须在锁内才读:排队期间前一个协程可能已经 ping 重连、换掉了这一对,
-- 锁外读的是退休的那个 skid。往退休 skid 上发,MORETOCOME 路径会被静默丢弃却报成功,
-- 同步路径则空等满一个 netread 超时。组包不受此影响——集合名/库名在组包时已写进包体,
-- 而"设集合名→组包"之间没有让出点。
-- MORETOCOME 问包不问 mgo:check_flag，理由见 C 层 mongo_pack_check_flag
local function _wdo(mgoctx, pack, size)
    local mgo = mgoctx.mongo
    local fd, skid = mgo:sock_id()
    if mongo.pack_check_flag(pack, ctx.FLAGS.MORETOCOME) then
        return srey.send(fd, skid, pack, size, 0), nil
    end
    local mgopack, _ = srey.syn_send(fd, skid, pack, size, 0)
    return nil ~= mgopack, mgopack
end
-- 统一"发送 + 按 MORETOCOME 决定是否等待响应"。pack 为 nil(C 层组包被拒)在此一并吸收:
-- 调用方不必各自守卫,新增的 pack_* 绑定也自动受保护。镜像 C 层 coro_utils.c 的 _mongo_send。
-- 串行化落在两个漏斗上（与 C 侧 _mongo_send / _mongo_sendwait 同处），一次覆盖全部命令站点：
-- 各命令函数在此之后只做纯解析、不再有 I/O，锁到漏斗为止与锁整个命令函数等效。
-- MORETOCOME 的只发不等也进锁——不等响应也不能乱序，后面那条 find 得看得见前面这批 insert。
-- connect / ping 另有外层锁，它们走多次往返或绕开漏斗，靠 ref 计数嵌套
---@param mgoctx any 所属 mongo_ctx（提供 serial 执行器与 C 层 mongo 句柄）
local function _wsend(mgoctx, pack, size)
    if not pack then
        return false, nil
    end
    local ok, mgopack = srey.serial_ret(nil, mgoctx.serial(_wdo, mgoctx, pack, size))
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
local function _wsend_n(mgoctx, pack, size)
    local ok, mgopack = _wsend(mgoctx, pack, size)
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
local function _wsend_ok(mgoctx, pack, size)
    local ok, mgopack = _wsend(mgoctx, pack, size)
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
local function _rdo(mgoctx, pack, size)
    local fd, skid = mgoctx.mongo:sock_id()-- 锁内读,理由同 _wdo
    local mgopack, _ = srey.syn_send(fd, skid, pack, size, 0)
    return mgopack
end
---@param mgoctx any 所属 mongo_ctx（提供 serial 执行器与 C 层 mongo 句柄）
local function _rsend(mgoctx, pack, size)
    if not pack then
        return nil
    end
    return srey.serial_ret(nil, mgoctx.serial(_rdo, mgoctx, pack, size))
end

-- 组包期间把连接级 flags 清零、组完再恢复：要等响应的命令不能带 MORETOCOME。
-- 套 pcall 是因为 Lua 没有 RAII 而 pack_* 会抛((指针,长度) 入口的长度校验)，抛点正在清零与恢复
-- 之间，不兜住就把 MORETOCOME 永久摘掉、clear_flag() 也查不出来。C 侧没有异常，无此问题
---@param mgo any 连接级 flags 的持有者（C 层 mongo 句柄）；清零与恢复都作用于它
---@param obj any 组包方法所在的对象：多数命令就是 mgo 自己，事务收尾是 session
---@param name string 组包方法名
---@return lightuserdata|nil pack 命令数据指针；组包被拒时为 nil
---@return integer? size 数据长度
local function _pack_noflag(mgo, obj, name, ...)
    local flags = mgo:clear_flag()
    local ok, pack, size = pcall(obj[name], obj, ...)
    mgo:set_flag(flags)
    if not ok then
        error(pack, 0)
    end
    return pack, size
end

-- mongo_session_ctx：会话事务上下文，由 mongo_ctx:startsession() 创建。
local sess_ctx = class("mongo_session_ctx")

---构造函数（内部使用）
---@param mgoctx any 所属 mongo_ctx 对象
---@param session_ud any C 层 mongo.session userdata
function sess_ctx:ctor(mgoctx, session_ud)
    self.mgoctx = mgoctx
    self.session = session_ud
    -- 记录创建时的连接代次；mongo ping 失败重连后代次 +1，旧 lsid 已被服务端清理
    self.gen = mgoctx.generation
end

---开始事务：递增 txnNumber，构建 lsid + txnNumber 事务选项 BSON，挂载到 mongo->session
---一条连接同时只允许一个活跃事务，该连接上已有别的 session 在事务中时返回 false
---@return boolean ok 成功 true（session 已因重连失效、或该连接上已有别的 session 处于事务中时返回 false）
function sess_ctx:begin()
    if self.gen ~= self.mgoctx.generation then
        WARN("mongo session invalidated by reconnect, please restart session.")
        return false
    end
    return self.session:begin()
end

-- 代次判定、组包、发送整段在锁内，理由同 C 侧 mongo_commit：判定与 _rsend 里那次上锁之间隔着
-- 一次不定长排队，锁外判就是过期票——排队期间别人可能已 ping 重连换掉连接。
-- 内层 _rsend 再上一次锁，同协程按 ref 嵌套
local function _txn_do(self, opts, optslens, packname, what)
    if self.gen ~= self.mgoctx.generation then
        WARN("mongo session invalidated by reconnect, please restart session.")
        return false
    end
    local mgo = self.mgoctx.mongo
    local pack, size = _pack_noflag(mgo, self.session, packname, opts, optslens)
    if not pack then
        WARN("mongo %s packing rejected, transaction state kept.", what)
        return false
    end
    local mgopack = _rsend(self.mgoctx, pack, size)
    if not mgopack then
        return false
    end
    self.session:done()
    return mgo:check_error(mgopack) >= 0
end
---commit / rollback 的共同流程，两者只差组包用哪个 C 接口和日志里的动作名。
---事务状态一路保留到服务端真的回了包为止：组包被拒（options 畸形或超单包上限、或连接已不再
---绑定该 session）和网络失败都可能只是这一次不成，状态还在就能重试或改走另一条收尾路径。
---这里若提前 done() 会解绑并 FREE options，之后另一条撞上绑定守卫也只能放弃，
---服务端那个事务就一直持锁到超时。
---流程主体见 _txn_do，本函数只负责套锁
---@param self any mongo_session_ctx 实例
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@param packname string session 上的组包方法名（"pack_commit" / "pack_abort"）；由 _pack_noflag 取用
---@param what string 动作名，仅用于组包被拒时的日志
---@return boolean ok 服务端确认且未报错 true
local function _txn_finish(self, opts, optslens, packname, what)
    return srey.serial_ret(false, self.mgoctx.serial(_txn_do, self, opts, optslens, packname, what))
end

---提交事务；网络失败或组包被拒时保留事务状态供重试，仅服务端响应确认时清理（见 _txn_finish）
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

-- 判定与发送同在锁内，理由同 _txn_do
local function _refresh_do(self)
    if self.gen ~= self.mgoctx.generation then
        WARN("mongo session invalidated by reconnect, please restart session.")
        return false
    end
    local mgo = self.mgoctx.mongo
    local pack, size = _pack_noflag(mgo, self.session, "pack_refresh")
    local mgopack = _rsend(self.mgoctx, pack, size)
    if not mgopack then
        return false
    end
    return mgo:check_error(mgopack) >= 0
end
---刷新会话超时（refreshSessions），延续会话存活时间
---@return boolean ok 刷新成功 true（session 已因重连失效时返回 false）
function sess_ctx:refresh()
    return srey.serial_ret(false, self.mgoctx.serial(_refresh_do, self))
end

-- 判定与发送同在锁内，理由同 _txn_do。
-- 重连后服务端已自动清理旧 lsid，跳过 endSessions 网络包
local function _close_do(self)
    if self.gen ~= self.mgoctx.generation then
        return
    end
    local pack, size = self.session:pack_endsession()
    _wsend(self.mgoctx, pack, size)
end
---结束会话（endSessions，fire-and-forget）并释放 C 层会话内存。
---session:free() 留在锁外：它不碰连接，且无论有没有发出 endSessions 都要释放
function sess_ctx:close()
    -- 锁外先判一次:代次只增不减,读到不等就必然真不等(服务端早清了旧 lsid),没东西可发,
    -- 不必排队等锁;读到相等可能是过期票,进去后 _close_do 在锁内还会重判一次
    if self.gen == self.mgoctx.generation then
        self.mgoctx.serial(_close_do, self)
    end
    self.session:free()
end

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param db string 初始数据库名
---@param user string? 认证用户名；nil 表示不认证
---@param password string 认证密码；user 非 nil 时必填（C 层 user_pwd 对它是 luaL_checkstring）
---@param authdb string? 认证数据库；nil 时使用 db
---@param authmod string? SCRAM 算法，默认 "SCRAM-SHA-256"
function ctx:ctor(ip, port, sslname, db, user, password, authdb, authmod)
    local ok, ssl = srey.ssl_qury(sslname)
    if not ok then
        error(string.format("ssl_qury not find ssl name %s", sslname), 2)
    end
    self.sslname = sslname
    self.mongo = mongo.new(ip, port, ssl, db)
    if not self.mongo then
        error(string.format("mongo.new failed: %s:%d db=%s", ip, port, tostring(db)), 2)
    end
    self.user = user
    self.authmod = authmod or "SCRAM-SHA-256"
    if user then
        -- 先自查再下发：user_pwd 的第 3 参在 C 层是 luaL_checkstring，password 为 nil 会先抛
        -- "bad argument #3 ... (string expected, got nil)",压根走不到下面那句带解释的 error
        if "string" ~= type(password) then
            error("mongo ctor: password is required when user is given", 2)
        end
        if not self.mongo:user_pwd(user, password) then
            error("mongo user_pwd failed: user or password too long", 2)
        end
        if not self.mongo:authdb(authdb or db) then
            error(string.format("mongo authdb failed: %s too long", tostring(authdb or db)), 2)
        end
    end
    -- 串行化执行器由 conn_pub 建，锁点在 _wsend / _rsend 两个漏斗上。
    -- 它只保证单条命令原子，不保证事务原子——事务上下文挂在连接上，别人的命令挤在
    -- begin 与 commit 之间时组包侧照样给它附上本事务的 lsid/txnNumber，与 C 侧同一结论：
    -- 要事务隔离请给事务用独占连接
    pub.init(self, self.mongo)
end

-- conn_pub 的建链钩子：TCP 连接（含可选 SSL 握手）→ 发 hello → 可选 SCRAM 身份验证。
-- 认证走 srey.send + wait_handshaked 绕开了漏斗，而那期间连接处于 AUTH 态，
-- 别人的普通命令挤进来会被当成认证响应解析——靠 conn_pub 把整段包在锁内
function ctx:_connect()
    local fd, skid = self.mongo:try_connect()
    if INVALID_SOCK == fd then
        return false
    end
    if not srey.wait_connect(fd, skid, SSL_NAME.NONE ~= self.sslname or nil) then
        return false   -- wait_connect 内已 close
    end
    -- 从此处往后失败需 close fd；用 sync_close 等复位完成再返回，避免旧连接异步 teardown 追上后清掉下一次 connect() 的新 fd
    local function _fail()
        local cfd, cskid = self.mongo:sock_id()-- 现取:对端已断时 sk.fd 已被 teardown 复位为 INVALID,sync_close 内 guard 直接返回不空等
        srey.sync_close(cfd, cskid)
        return false
    end
    -- 解绑上一代事务会话，否则 pack_hello 及后续命令会带上旧的 lsid/txnNumber。
    -- Lua 侧走 try_connect 不经 C 的 mongo_connect，那边同一句在 coro_utils.c 的 mongo_connect 里
    self.mongo:clear_session()
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_hello")
    local mgopack = _rsend(self, pack, size)
    if not mgopack then return _fail() end
    if self.mongo:check_error(mgopack) < 0 then return _fail() end
    if self.user then
        -- ev_ud_status 只在 fd 为 INVALID_SOCK 时失败,而 fd 上面已判过,这支实际不可达;
        -- 留着是为了它哪天新增失败原因时不漏 fd,所以走 _fail() 而不是裸 return
        if not self.mongo:set_auth_status(fd, skid) then
            return _fail()
        end
        -- 不走 _pack_noflag：清零要盖住"组包+发送+等握手"整段(SCRAM 多次往返)，不是只盖组包。
        -- 这段也抛不出来——pack_auth_first 收的 authmod 由 ctor 兜成 "SCRAM-SHA-256"
        local aflags = self.mongo:clear_flag()
        local authpack, authsize = self.mongo:pack_auth_first(self.authmod)
        local ok = false
        if authpack and srey.send(fd, skid, authpack, authsize, 0) then
            ok = srey.wait_handshaked(fd, skid)
        end
        self.mongo:set_flag(aflags)
        if not ok then return _fail() end
    end
    return true
end

---内部 ping（isMaster / ping 命令），不自动重连
---@return boolean ok 服务端响应成功 true（仅供 ping() 内部调用，不要直接调用；调用方须已持锁）
function ctx:_ping()
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_ping")
    local mgopack = _rsend(self, pack, size)
    if not mgopack then
        return false
    end
    return self.mongo:check_error(mgopack) >= 0
end

---切换当前数据库
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
---服务端的失败（重复键、校验不过）因为没有响应可解析而一律报成功；读命令内部会临时清掉再恢复。
---标志挂在连接上而不是命令上，多协程共用一条连接时别人的写也会跟着变成 fire-and-forget，
---批量写完请及时清掉
---@param flag integer ctx.FLAGS 枚举值
function ctx:set_flag(flag)
    if ctx.FLAGS.MORETOCOME ~= flag then
        WARN("mongo set_flag: flag %s not implemented by current mongo_set_flag (only MORETOCOME supported), ignored.", tostring(flag))
        return
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
    if not self.mongo:collection(col) then
        return false
    end
    local pack, size = self.mongo:pack_insert(docs, dlens, opts, optslens)
    return _wsend_n(self, pack, size)
end

---更新文档
---@param col string 集合名
---@param updates lightuserdata BSON 数组格式的更新列表指针
---@param ulens integer updates 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
---@return integer? n 成功时为 nModified
function ctx:update(col, updates, ulens, opts, optslens)
    if not self.mongo:collection(col) then
        return false
    end
    local pack, size = self.mongo:pack_update(updates, ulens, opts, optslens)
    return _wsend_n(self, pack, size)
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
    if not self.mongo:collection(col) then
        return false
    end
    local pack, size = self.mongo:pack_delete(deletes, dlens, opts, optslens)
    return _wsend_n(self, pack, size)
end

---删除当前集合（drop）
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
function ctx:drop(opts, optslens)
    local pack, size = self.mongo:pack_drop(opts, optslens)
    return _wsend_ok(self, pack, size)
end

---批量写操作（bulkWrite，MongoDB 8.0+）
---@param ops lightuserdata BSON 数组格式操作列表指针
---@param opsz integer ops 字节数
---@param nsinfo lightuserdata BSON 数组格式命名空间信息指针
---@param nsz integer nsinfo 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|true|nil mgopack 普通模式返回响应包指针供解析；MORETOCOME fire-and-forget 成功返回 true；发送失败返回 nil
function ctx:bulkwrite(ops, opsz, nsinfo, nsz, opts, optslens)
    local pack, size = self.mongo:pack_bulkwrite(ops, opsz, nsinfo, nsz, opts, optslens)
    local ok, mgopack = _wsend(self, pack, size)
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
    if not self.mongo:collection(col) then
        return false
    end
    local pack, size = self.mongo:pack_createindexes(indexes, ilens, opts, optslens)
    return _wsend_ok(self, pack, size)
end

---删除索引
---@param col string 集合名
---@param indexes lightuserdata BSON 数组格式索引名列表指针
---@param ilens integer indexes 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return boolean ok 成功 true
function ctx:dropindexes(col, indexes, ilens, opts, optslens)
    if not self.mongo:collection(col) then
        return false
    end
    local pack, size = self.mongo:pack_dropindexes(indexes, ilens, opts, optslens)
    return _wsend_ok(self, pack, size)
end

-- ---- 读操作（返回 mgopack lightuserdata 或 nil）----

---查询文档（find，支持游标分页）；通过 mongo.doc + bson.iter 遍历结果，cursorid 判断是否有后续游标
---@param col string 集合名
---@param filter lightuserdata? BSON 过滤条件；nil 表示全部
---@param flens integer? filter 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项（limit/skip/sort 等）
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:find(col, filter, flens, opts, optslens)
    if not self.mongo:collection(col) then
        return nil
    end
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_find", filter, flens, opts, optslens)
    local mgopack = _rsend(self, pack, size)
    return mgopack
end

---聚合查询（aggregate）
---@param col string 集合名
---@param pipeline lightuserdata BSON 数组格式聚合管道指针
---@param pllens integer pipeline 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:aggregate(col, pipeline, pllens, opts, optslens)
    if not self.mongo:collection(col) then
        return nil
    end
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_aggregate", pipeline, pllens, opts, optslens)
    local mgopack = _rsend(self, pack, size)
    return mgopack
end

---获取游标后续批次（getMore）
---@param cursorid integer 上次 find / aggregate 返回的游标 ID
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:getmore(cursorid, opts, optslens)
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_getmore", cursorid, opts, optslens)
    local mgopack = _rsend(self, pack, size)
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
    if not self.mongo:collection(col) then
        return false
    end
    local pack, size = self.mongo:pack_killcursors(cursorids, cslens, opts, optslens)
    return _wsend_ok(self, pack, size)
end

---去重查询（distinct）
---@param col string 集合名
---@param key string 去重字段名
---@param query lightuserdata? BSON 过滤条件；nil 表示全部
---@param qlens integer? query 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:distinct(col, key, query, qlens, opts, optslens)
    if not self.mongo:collection(col) then
        return nil
    end
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_distinct", key, query, qlens, opts, optslens)
    local mgopack = _rsend(self, pack, size)
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
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:findandmodify(col, query, qlens, remove, pipeline, update, ulens, opts, optslens)
    if not self.mongo:collection(col) then
        return nil
    end
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_findandmodify", query, qlens, remove, pipeline, update, ulens, opts, optslens)
    local mgopack = _rsend(self, pack, size)
    return mgopack
end

---文档计数（count）
---@param col string 集合名
---@param query lightuserdata? BSON 过滤条件；nil 表示全部计数
---@param qlens integer? query 字节数
---@param opts string|lightuserdata|nil 附加 BSON 选项
---@param optslens integer? opts 为 lightuserdata 时必填，缓冲字节数
---@return integer|false n 计数整数；失败返回 false
function ctx:count(col, query, qlens, opts, optslens)
    if not self.mongo:collection(col) then
        return false
    end
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_count", query, qlens, opts, optslens)
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

---启动服务端逻辑会话（startSession）
---@return any|nil session mongo_session_ctx 实例；失败返回 nil
function ctx:startsession()
    local pack, size = _pack_noflag(self.mongo, self.mongo, "pack_startsession")
    local mgopack = _rsend(self, pack, size)
    if not mgopack then
        return nil
    end
    local uuid, timeout = self.mongo:parse_startsession(mgopack)
    if not uuid then
        return nil
    end
    local session_ud = mongo_session.new(self.mongo, uuid, timeout)
    if not session_ud then
        return nil
    end
    return sess_ctx.new(self, session_ud)
end

-- conn_pub 的断开钩子：MongoDB 无专用断开命令，直接 close socket
function ctx:_doquit()
    local fd, skid = self.mongo:sock_id()
    srey.sync_close(fd, skid)
end

return ctx
