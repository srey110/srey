-- PostgreSQL 预处理语句执行器（pgsql_stmt_ctx 类）。
-- 由 pgsql_ctx:prepare() 返回，持有语句名称与所属连接引用。
-- 同一 stmt 可多次调用 execute（每次绑定不同参数）。
-- 执行完毕后调用 close 通知服务端释放语句资源。

local srey   = require("lib.srey")
local pgsql  = require("pgsql")
local reader = require("pgsql.reader")

---@enum PGPACK_TYPE
PGPACK_TYPE = {
    OK           = 0x00,
    ERR          = 0x01,
    NOTIFICATION = 0x02,
    COPY_IN      = 0x03,
    COPY_OUT     = 0x04,
}
---@enum PG_FORMAT
PG_FORMAT = {
    TEXT   = 0,
    BINARY = 1,
}

-- pgsql_stmt_ctx：预处理语句执行上下文。
-- self.name      ：服务端语句名称（Parse 时传入）。
-- self.owner     ：pgsql_ctx Lua 包装实例，守卫读其实时 generation。
-- self.pg        ：C 层 pgsql 对象引用（= owner.pg），每次操作时动态读取 fd/skid 以感知重连。
-- self.format    ：结果集期望格式（二进制 / 文本），prepare 时确定。
-- self.affected  ：最近一次执行影响的行数。
-- self.err       ：最近一次错误信息。
local ctx = class("pgsql_stmt_ctx")

---构造函数
---@param owner any pgsql_ctx Lua 包装实例（持有实时 generation 与 C pgsql 对象）
---@param name string 预处理语句名
---@param format PG_FORMAT 结果列格式，默认 BINARY
function ctx:ctor(owner, name, format)
    self.name   = name
    self.format = format or PG_FORMAT.BINARY
    self.owner  = owner
    self.pg     = owner.pg
    self.affected = 0
    self.err = ""
    -- 记录创建时的连接代次；pgsql ping 失败重连后 owner.generation +1，旧 statement name 已失效
    self.gen = owner.generation
end

---执行预处理语句（Bind + Describe + Execute + Sync）
---@param bind any? pgsql_bind_ctx 参数绑定上下文
---@return boolean|_pgsql_reader_ctx result reader=结果集；true=无结果集 OK；false=失败
function ctx:execute(bind)
    if self.gen ~= self.owner.generation then
        WARN("pgsql stmt invalidated by reconnect, please re-prepare.")
        return false
    end
    local fd, skid = self.pg:sock_id()
    local pack, size = pgsql.pack_stmt_execute(self.name, bind, self.format)
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
        return false
    end
    local rd = reader.new(pgpack, self.format)
    if rd then
        return rd
    end
    self.affected = pgsql.affected_rows(pgpack)
    return true
end

---发送 Close + Sync，通知服务端释放该预处理语句
---@return boolean ok 关闭成功 true
function ctx:close()
    if self.gen ~= self.owner.generation then
        -- 重连后服务端已自动清理旧语句，无需再发 Close
        return true
    end
    local fd, skid = self.pg:sock_id()
    local pack, size = pgsql.pack_stmt_close(self.name)
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return false
    end
    return PGPACK_TYPE.OK == pgsql.pack_type(pgpack)
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

return ctx
