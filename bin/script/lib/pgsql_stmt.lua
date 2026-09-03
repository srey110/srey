-- PostgreSQL 预处理语句执行器（pgsql_stmt_ctx 类）。
-- 由 pgsql_ctx:prepare() 返回，持有语句名称与所属连接引用。
-- 同一 stmt 可多次调用 execute（每次绑定不同参数）。
-- 执行完毕后调用 close 通知服务端释放语句资源。

local srey   = require("lib.srey")
local pgsql  = require("srey.pgsql")
local reader = require("srey.pgsql.reader")
local ppub   = require("lib.pgsql_pub")-- 与 pgsql.lua 共用的失败原因，见该模块头部

---预处理语句执行上下文
---@class pgsql_stmt_ctx
---@field name string 服务端语句名称（Parse 时传入）
---@field owner any pgsql_ctx Lua 包装实例，守卫读其实时 generation
---@field pg any C 层 pgsql 对象引用（= owner.pg），每次操作动态读 fd/skid 以感知重连
---@field format PG_FORMAT 结果集期望格式（二进制 / 文本），prepare 时确定
---@field affected integer 最近一次执行影响的行数
---@field closed boolean close() 已发出 Close，此后 execute 一律拒绝（口径同 mysql_stmt）
---@field err string 最近一次错误信息
local ctx = class("pgsql_stmt_ctx")
-- _fail / _reset 与 pgsql.lua 共用 ppub 的同一份实现（说明见那边）。
-- 这边只有 execute 一个带 err 契约的入口，故不设 _busy
ctx._fail = ppub.fail
ctx._reset = ppub.reset

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

---执行预处理语句（Bind + Describe + Execute + Sync）。
---与 pgsql_ctx:query 不同，这里不返回数组：扩展协议一次 Execute 只有一条语句、一个结果，
---没有"第几条"可言
---@param bind any? pgsql_bind_ctx 参数绑定上下文
---@return boolean|_pgsql_reader_ctx result reader=结果集；true=无结果集 OK（行数走 affected_rows）；
---false=失败或语句失效
function ctx:execute(bind)
    -- 借宿主连接的执行器：语句和普通查询走的是同一条连接，两者之间也不能交错
    return srey.serial_ret(false, self.owner.serial(self._execute, self, bind))
end
function ctx:_execute(bind)
    self:_reset()
    -- 关过的语句服务端已经不认了。不挡的话 Bind 照发，回来的是 26000 prepared statement
    -- does not exist，调用方从 erro() 上分不清"关过了"和"服务端出了别的问题"
    if self.closed then
        WARN("pgsql stmt already closed, please re-prepare.")
        return self:_fail("pgsql: stmt already closed")
    end
    if self.gen ~= self.owner.generation then
        WARN("pgsql stmt invalidated by reconnect, please re-prepare.")
        return self:_fail("pgsql: stmt invalidated by reconnect")
    end
    local pack, size = pgsql.pack_stmt_execute(self.name, bind, self.format)
    local pgpack = ppub.request(self, pack, size, PGPACK_TYPE.OK)
    if not pgpack then
        return false
    end
    local rd = reader.iter(pgpack, self.format)
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
    if self.closed then
        return false
    end
    self.closed = true
    if self.gen ~= self.owner.generation then
        -- 重连后服务端已自动清理旧语句，无需再发 Close
        return true
    end
    local pack, size = pgsql.pack_stmt_close(self.name)
    return ppub.request_ok(self, pack, size, PGPACK_TYPE.OK)
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
