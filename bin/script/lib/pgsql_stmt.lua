-- PostgreSQL 预处理语句执行器（pgsql_stmt_ctx 类）。
-- 由 pgsql_ctx:prepare() 返回，持有语句名称与所属连接引用。
-- 同一 stmt 可多次调用 execute（每次绑定不同参数）。
-- 执行完毕后调用 close 通知服务端释放语句资源。

local srey   = require("lib.srey")
local pgsql  = require("pgsql")
local reader = require("pgsql.reader")
local ppub   = require("lib.pgsql_pub")-- 与 pgsql.lua 共用的失败原因，见该模块头部

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
-- 与 pgsql.lua 的同名方法同义：写 err 并返回 false，让"置原因"与"报失败"成为一步。
-- 这边只有 execute 一个带 err 契约的入口，故不设 _busy，入口的复位写在 execute 里
---@param err string 失败原因
---@return boolean always false
function ctx:_fail(err)
    self.err = err
    return false
end

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
---@return boolean|_pgsql_reader_ctx result reader=结果集；true=无结果集 OK；false=失败或语句失效
function ctx:execute(bind)
    -- 借宿主连接的执行器：语句和普通查询走的是同一条连接，两者之间也不能交错
    return srey.serial_ret(false, self.owner.serial(self._execute, self, bind))
end
function ctx:_execute(bind)
    self.err = ""-- 复位:erro() 只反映最近一次操作
    if self.gen ~= self.owner.generation then
        WARN("pgsql stmt invalidated by reconnect, please re-prepare.")
        return self:_fail("pgsql: stmt invalidated by reconnect")
    end
    local fd, skid = self.pg:sock_id()
    local pack, size = pgsql.pack_stmt_execute(self.name, bind, self.format)
    local pgpack, _ = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return self:_fail(ppub.SEND)
    end
    local e = ppub.check_type(pgpack, PGPACK_TYPE.OK)
    if e then
        return self:_fail(e)
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
    return srey.serial_ret(false, self.owner.serial(self._close, self))
end
function ctx:_close()
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
