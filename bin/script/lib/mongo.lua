-- MongoDB 客户端（mongo_ctx 类）。
-- 封装 C 层 mongo / mongo.session 模块，提供：连接管理、SCRAM 认证、
-- CRUD 操作（find/insert/update/delete/aggregate/count 等）及会话事务支持。
-- 连接生命周期完全由 Lua 端管理：mongo:try_connect 发起、srey.wait_connect 等待、
-- srey.close 关闭；C 层 mongo 模块只做命令组包/解包。

local srey = require("lib.srey")
local mongo = require("mongo")
local mongo_session = require("mongo.session")
-- mongo_ctx：MongoDB 连接上下文，每实例对应一条持久连接。
local ctx = class("mongo_ctx")

-- MongoDB OP_MSG 消息标志位（与 C 层 mongo_flags 枚举对应）
ctx.FLAGS = {
    CHECKSUM       = 0x01,
    MORETOCOME     = 0x02,
    EXHAUSTALLOWED = 1 << 16,
}

local function _wsend(mgo, fd, skid, pack, size)
    if mgo:check_flag(ctx.FLAGS.MORETOCOME) then
        return srey.send(fd, skid, pack, size, 0), nil
    end
    local mgopack, _ = srey.syn_send(fd, skid, pack, size, 0)
    return nil ~= mgopack, mgopack
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

---开始事务：递增 txnNumber，构建 lsid + txnNumber 事务选项 BSON，挂载到 mongo-&gt;session
function sess_ctx:begin()
    self.session:begin()
end

---提交事务；网络失败时保留事务状态供重试，仅服务端响应确认时清理
---@param opts lightuserdata? 附加 writeConcern 等 BSON 选项
---@return boolean ok 提交成功 true
function sess_ctx:commit(opts)
    if self.gen ~= self.mgoctx.generation then
        WARN("mongo session invalidated by reconnect, please restart session.")
        return false
    end
    local flags = self.mgoctx.mongo:clear_flag()
    local pack, size = self.session:pack_commit(opts)
    self.mgoctx.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.mgoctx.fd, self.mgoctx.skid, pack, size, 0)
    if not mgopack then
        -- 网络失败：保留事务状态，不调用 done()，便于重试
        return false
    end
    -- 服务端有响应：清理事务状态（无论成功或失败）
    self.session:done()
    return self.mgoctx.mongo:check_error(mgopack) >= 0
end

---回滚事务；网络失败时保留事务状态供重试，仅服务端响应确认时清理
---@param opts lightuserdata? 附加 BSON 选项
---@return boolean ok 回滚成功 true
function sess_ctx:rollback(opts)
    if self.gen ~= self.mgoctx.generation then
        WARN("mongo session invalidated by reconnect, please restart session.")
        return false
    end
    local flags = self.mgoctx.mongo:clear_flag()
    local pack, size = self.session:pack_abort(opts)
    self.mgoctx.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.mgoctx.fd, self.mgoctx.skid, pack, size, 0)
    if not mgopack then
        -- 网络失败：保留事务状态，不调用 done()，便于重试
        return false
    end
    -- 服务端有响应：清理事务状态（无论成功或失败）
    self.session:done()
    return self.mgoctx.mongo:check_error(mgopack) >= 0
end

---刷新会话超时（refreshSessions），延续会话存活时间
---@return boolean ok 刷新成功 true
function sess_ctx:refresh()
    if self.gen ~= self.mgoctx.generation then
        WARN("mongo session invalidated by reconnect, please restart session.")
        return false
    end
    local flags = self.mgoctx.mongo:clear_flag()
    local pack, size = self.session:pack_refresh()
    self.mgoctx.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.mgoctx.fd, self.mgoctx.skid, pack, size, 0)
    if not mgopack then
        return false
    end
    return self.mgoctx.mongo:check_error(mgopack) >= 0
end

---结束会话（endSessions，fire-and-forget）并释放 C 层会话内存
function sess_ctx:close()
    -- 重连后服务端已自动清理旧 lsid，跳过 endSessions 网络包；本地 C 资源始终释放
    if self.gen == self.mgoctx.generation then
        local pack, size = self.session:pack_endsession()
        _wsend(self.mgoctx.mongo, self.mgoctx.fd, self.mgoctx.skid, pack, size)
    end
    self.session:free()
end

---构造函数
---@param ip string 服务器 IP
---@param port integer 服务器端口
---@param sslname SSL_NAME SSL 上下文名；SSL_NAME.NONE 表示明文
---@param db string 初始数据库名
---@param user string? 认证用户名；nil 表示不认证
---@param password string? 认证密码
---@param authdb string? 认证数据库；nil 时使用 db
---@param authmod string? SCRAM 算法，默认 "SCRAM-SHA-256"
function ctx:ctor(ip, port, sslname, db, user, password, authdb, authmod)
    local core = require("srey.core")
    local ssl
    if SSL_NAME.NONE ~= sslname then
        ssl = core.ssl_qury(sslname)
    end
    self.sslname = sslname
    self.mongo = mongo.new(ip, port, ssl, db)
    if not self.mongo then
        error(string.format("mongo.new failed: %s:%d db=%s", ip, port, tostring(db)), 2)
    end
    self.user = user
    self.authmod = authmod or "SCRAM-SHA-256"
    if user then
        self.mongo:user_pwd(user, password)
        self.mongo:authdb(authdb or db)
    end
    self.fd = INVALID_SOCK
    self.skid = 0
    -- 连接代次：每次 connect 成功 +1，session 持有创建时的代次以感知重连
    self.generation = 0
end

---建立 TCP 连接（含可选 SSL 握手）→ 发 hello → 可选 SCRAM 身份验证；成功后 self.fd/skid 缓存连接句柄
---@return boolean ok 连接和认证均成功时 true
function ctx:connect()
    local fd, skid = self.mongo:try_connect()
    if INVALID_SOCK == fd then
        return false
    end
    if not srey.wait_connect(fd, skid, SSL_NAME.NONE ~= self.sslname or nil) then
        return false   -- wait_connect 内已 close
    end
    srey.sock_session(fd, skid, skid)
    self.fd, self.skid = fd, skid
    -- 从此处往后失败需 close fd 并重置 self 缓存
    local function _fail()
        srey.close(fd, skid)
        self.fd, self.skid = INVALID_SOCK, 0
        return false
    end
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_hello()
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not mgopack then return _fail() end
    if self.mongo:check_error(mgopack) < 0 then return _fail() end
    if self.user then
        self.mongo:set_auth_status(fd, skid)
        local authpack, authsize = self.mongo:pack_auth_first(self.authmod)
        if not authpack then return _fail() end
        if not srey.send(fd, skid, authpack, authsize, 0) then return _fail() end
        local ok, _, _ = srey.wait_handshaked(fd, skid)
        if not ok then return _fail() end
    end
    self.generation = self.generation + 1
    return true
end

---内部 ping（isMaster / ping 命令），不自动重连
---@return boolean ok 服务端响应成功 true
function ctx:_ping()
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_ping()
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.fd, self.skid, pack, size, 0)
    if not mgopack then
        return false
    end
    return self.mongo:check_error(mgopack) >= 0
end

---连接保活：ping 失败时自动重连，建议在执行操作前调用
---@return boolean ok 连接可用 true
function ctx:ping()
    if not self:_ping() then
        srey.sync_close(self.fd, self.skid, 1)
        self.fd, self.skid = INVALID_SOCK, 0
        return self:connect()
    end
    return true
end

---切换当前数据库
---@param name string 数据库名
function ctx:db(name)
    self.mongo:db(name)
 end

---切换当前集合
---@param name string 集合名
function ctx:collection(name)
    self.mongo:collection(name)
end

---设置下一条命令的消息标志位
---@param flag integer ctx.FLAGS 枚举值
function ctx:set_flag(flag)
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
---@param opts lightuserdata? 附加 BSON 选项
---@return boolean ok 成功 true
---@return integer? n 成功时为 nInserted
function ctx:insert(col, docs, dlens, opts)
    self.mongo:collection(col)
    local pack, size = self.mongo:pack_insert(docs, dlens, opts)
    local ok, mgopack = _wsend(self.mongo, self.fd, self.skid, pack, size)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    local n = self.mongo:check_error(mgopack)
    if n < 0 then
        return false
    end
    return true, n
end

---更新文档
---@param col string 集合名
---@param updates lightuserdata BSON 数组格式的更新列表指针
---@param ulens integer updates 字节数
---@param opts lightuserdata? 附加 BSON 选项
---@return boolean ok 成功 true
---@return integer? n 成功时为 nModified
function ctx:update(col, updates, ulens, opts)
    self.mongo:collection(col)
    local pack, size = self.mongo:pack_update(updates, ulens, opts)
    local ok, mgopack = _wsend(self.mongo, self.fd, self.skid, pack, size)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    local n = self.mongo:check_error(mgopack)
    if n < 0 then
        return false
    end
    return true, n
end

---删除文档
---@param col string 集合名
---@param deletes lightuserdata BSON 数组格式的删除列表指针
---@param dlens integer deletes 字节数
---@param opts lightuserdata? 附加 BSON 选项
---@return boolean ok 成功 true
---@return integer? n 成功时为 nDeleted
function ctx:delete(col, deletes, dlens, opts)
    self.mongo:collection(col)
    local pack, size = self.mongo:pack_delete(deletes, dlens, opts)
    local ok, mgopack = _wsend(self.mongo, self.fd, self.skid, pack, size)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    local n = self.mongo:check_error(mgopack)
    if n < 0 then
        return false
    end
    return true, n
end

---删除当前集合（drop）
---@param opts lightuserdata? 附加 BSON 选项
---@return boolean ok 成功 true
function ctx:drop(opts)
    local pack, size = self.mongo:pack_drop(opts)
    local ok, mgopack = _wsend(self.mongo, self.fd, self.skid, pack, size)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    return self.mongo:check_error(mgopack) >= 0
end

---批量写操作（bulkWrite，MongoDB 8.0+）
---@param ops lightuserdata BSON 数组格式操作列表指针
---@param opsz integer ops 字节数
---@param nsinfo lightuserdata BSON 数组格式命名空间信息指针
---@param nsz integer nsinfo 字节数
---@param opts lightuserdata? 附加 BSON 选项
---@return lightuserdata|true|nil mgopack 普通模式返回响应包指针供解析；MORETOCOME fire-and-forget 成功返回 true；发送失败返回 nil
function ctx:bulkwrite(ops, opsz, nsinfo, nsz, opts)
    local pack, size = self.mongo:pack_bulkwrite(ops, opsz, nsinfo, nsz, opts)
    local ok, mgopack = _wsend(self.mongo, self.fd, self.skid, pack, size)
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
---@param opts lightuserdata? 附加 BSON 选项
---@return boolean ok 成功 true
function ctx:createindexes(col, indexes, ilens, opts)
    self.mongo:collection(col)
    local pack, size = self.mongo:pack_createindexes(indexes, ilens, opts)
    local ok, mgopack = _wsend(self.mongo, self.fd, self.skid, pack, size)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    return self.mongo:check_error(mgopack) >= 0
end

---删除索引
---@param col string 集合名
---@param indexes lightuserdata BSON 数组格式索引名列表指针
---@param ilens integer indexes 字节数
---@param opts lightuserdata? 附加 BSON 选项
---@return boolean ok 成功 true
function ctx:dropindexes(col, indexes, ilens, opts)
    self.mongo:collection(col)
    local pack, size = self.mongo:pack_dropindexes(indexes, ilens, opts)
    local ok, mgopack = _wsend(self.mongo, self.fd, self.skid, pack, size)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    return self.mongo:check_error(mgopack) >= 0
end

-- ---- 读操作（返回 mgopack lightuserdata 或 nil）----

---查询文档（find，支持游标分页）；通过 mongo.doc + bson.iter 遍历结果，cursorid 判断是否有后续游标
---@param col string 集合名
---@param filter lightuserdata? BSON 过滤条件；nil 表示全部
---@param flens integer? filter 字节数
---@param opts lightuserdata? 附加 BSON 选项（limit/skip/sort 等）
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:find(col, filter, flens, opts)
    self.mongo:collection(col)
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_find(filter, flens, opts)
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.fd, self.skid, pack, size, 0)
    return mgopack
end

---聚合查询（aggregate）
---@param col string 集合名
---@param pipeline lightuserdata BSON 数组格式聚合管道指针
---@param pllens integer pipeline 字节数
---@param opts lightuserdata? 附加 BSON 选项
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:aggregate(col, pipeline, pllens, opts)
    self.mongo:collection(col)
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_aggregate(pipeline, pllens, opts)
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.fd, self.skid, pack, size, 0)
    return mgopack
end

---获取游标后续批次（getMore）
---@param cursorid integer 上次 find / aggregate 返回的游标 ID
---@param opts lightuserdata? 附加 BSON 选项
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:getmore(cursorid, opts)
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_getmore(cursorid, opts)
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.fd, self.skid, pack, size, 0)
    return mgopack
end

---关闭游标（killCursors）
---@param col string 集合名
---@param cursorids lightuserdata BSON 数组格式游标 ID 列表指针
---@param cslens integer cursorids 字节数
---@param opts lightuserdata? 附加 BSON 选项
---@return boolean ok 成功 true
function ctx:killcursors(col, cursorids, cslens, opts)
    self.mongo:collection(col)
    local pack, size = self.mongo:pack_killcursors(cursorids, cslens, opts)
    local ok, mgopack = _wsend(self.mongo, self.fd, self.skid, pack, size)
    if not ok then
        return false
    end
    if not mgopack then
        return true
    end
    return self.mongo:check_error(mgopack) >= 0
end

---去重查询（distinct）
---@param col string 集合名
---@param key string 去重字段名
---@param query lightuserdata? BSON 过滤条件；nil 表示全部
---@param qlens integer? query 字节数
---@param opts lightuserdata? 附加 BSON 选项
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:distinct(col, key, query, qlens, opts)
    self.mongo:collection(col)
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_distinct(key, query, qlens, opts)
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.fd, self.skid, pack, size, 0)
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
---@param opts lightuserdata? 附加 BSON 选项
---@return lightuserdata|nil mgopack 响应包指针；失败返回 nil
function ctx:findandmodify(col, query, qlens, remove, pipeline, update, ulens, opts)
    self.mongo:collection(col)
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_findandmodify(query, qlens, remove, pipeline, update, ulens, opts)
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.fd, self.skid, pack, size, 0)
    return mgopack
end

---文档计数（count）
---@param col string 集合名
---@param query lightuserdata? BSON 过滤条件；nil 表示全部计数
---@param qlens integer? query 字节数
---@param opts lightuserdata? 附加 BSON 选项
---@return integer|false n 计数整数；失败返回 false
function ctx:count(col, query, qlens, opts)
    self.mongo:collection(col)
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_count(query, qlens, opts)
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.fd, self.skid, pack, size, 0)
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
    local flags = self.mongo:clear_flag()
    local pack, size = self.mongo:pack_startsession()
    self.mongo:set_flag(flags)
    local mgopack, _ = srey.syn_send(self.fd, self.skid, pack, size, 0)
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

---关闭连接（MongoDB 无专用断开命令，直接 close socket）
function ctx:quit()
    srey.sync_close(self.fd, self.skid)
    self.fd, self.skid = INVALID_SOCK, 0
end

return ctx
