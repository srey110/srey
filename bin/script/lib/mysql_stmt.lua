-- MySQL 预处理语句执行器（mysql_stmt_ctx 类）。
-- 由 mysql_ctx:prepare() 返回，持有 C 层 stmt 句柄。
-- 同一 stmt 可多次调用 execute（每次绑定不同参数），
-- 执行完毕后调用 reset 让服务端恢复到 prepare 后的就绪状态。
-- 不再使用时调用 close 通知服务端释放句柄——GC 只做本地释放，不会替你发 COM_STMT_CLOSE。

local srey   = require("lib.srey")
local stmt   = require("mysql.stmt")
local mpub   = require("lib.mysql_pub")-- 与 mysql.lua 共用的请求收尾，见该模块头部

-- mysql_stmt_ctx：预处理语句执行上下文。
-- self.stmt      ：C 层 stmt 对象，持有服务端 statement_id；动态调用 sock_id() 感知重连。
-- self.owner     ：mysql_ctx Lua 包装实例，守卫读其实时 generation，多结果集收包复用其 _read_results。
-- self.closed    ：close() 已发出 COM_STMT_CLOSE，此后 execute / reset 一律拒绝。
local ctx = class("mysql_stmt_ctx")

---构造函数
---@param owner any mysql_ctx Lua 包装实例（持有实时 generation 与 C mysql 对象）
---@param mpack lightuserdata COM_STMT_PREPARE 响应包，C 层据此初始化列元数据和参数个数
function ctx:ctor(owner, mpack)
    self.stmt = stmt.new(owner.mysql, mpack)
    if not self.stmt then
        error("mysql stmt.new failed", 2)
    end
    self.owner = owner
    -- 记录创建时的连接代次；mysql ping 失败重连后 owner.generation +1，旧 statement_id 已失效
    self.gen = owner.generation
end


---执行预处理语句（COM_STMT_EXECUTE）
---@param mbind any? mysql_bind_ctx 参数绑定上下文
---@return (_mysql_reader_ctx|boolean)[]|nil results 结果集数组（元素 reader=结果集 / true=OK 包 / false=ERR 包）；
---网络失败、多结果集中途断连、语句失效返回 nil。
---绑定参数个数与语句声明不符属调用方契约违反，C 侧直接抛出，经 serial 的 xpcall 记 ERROR 后同样返回 nil
function ctx:execute(mbind)
    -- 借宿主连接的执行器：语句和普通查询走的是同一条连接，两者之间也不能交错
    return srey.serial_ret(nil, self.owner.serial(self._execute, self, mbind))
end
function ctx:_execute(mbind)
    if self.closed then
        WARN("mysql stmt already closed, please re-prepare.")
        return nil
    end
    if self.gen ~= self.owner.generation then
        WARN("mysql stmt invalidated by reconnect, please re-prepare.")
        return nil
    end
    local pack, size = self.stmt:pack_stmt_execute(mbind)
    local mpack, fd, skid = mpub.request(self.stmt, pack, size)
    if not mpack then
        return nil
    end
    return self.owner:_read_results(fd, skid, mpack)
end

---发送 COM_STMT_RESET：清除服务端语句执行状态，保留 prepare 结果，下次 execute 可绑定新参数
---@return boolean ok 重置成功 true（语句失效返回 false）
function ctx:reset()
    return srey.serial_ret(false, self.owner.serial(self._reset, self))
end
function ctx:_reset()
    if self.closed then
        WARN("mysql stmt already closed, please re-prepare.")
        return false
    end
    if self.gen ~= self.owner.generation then
        WARN("mysql stmt invalidated by reconnect, please re-prepare.")
        return false
    end
    local pack, size = self.stmt:pack_stmt_reset()
    return mpub.request_ok(self.stmt, pack, size)
end

---发送 COM_STMT_CLOSE，通知服务端释放该语句句柄。
---不调用则句柄要留到连接关闭才回收，长连接上每请求 prepare 一次会逐步撞满
---max_prepared_stmt_count（默认 16382），此后 prepare 一律失败。
---服务端不回响应，故只发不等；关闭后本 stmt 不可再 execute / reset。
---重连后服务端已随旧连接清掉该语句，此时不再发包（发出去就是拿旧 id 打新连接）
---@return boolean ok 已发出 true（语句已关闭或重连后已失效返回 false）
function ctx:close()
    return srey.serial_ret(false, self.owner.serial(self._close, self))
end
function ctx:_close()
    if self.closed then
        return false
    end
    self.closed = true
    if self.gen ~= self.owner.generation then
        return false
    end
    local fd, skid = self.stmt:sock_id()
    local pack, size = self.stmt:pack_stmt_close()
    return srey.send(fd, skid, pack, size, 0)
end

---返回最近一次错误信息并清除错误状态
---execute 用 false 元素表示服务端回了 ERR 包，原因只能从这里取
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---（读取即清除，更要紧挨着命令读——别的协程先读一次就把它清空了）
---@return string err 错误描述（服务端文案，随版本与 locale 变）
---@return integer code MySQL 错误号；无错误时为 0。要区分可重试（1213 死锁 / 1205 锁等待超时）
---与不可重试（1062 重复键）只能靠它，别去匹配文案
function ctx:erro()
    return self.owner:erro()
end

---返回最近一次 INSERT 操作产生的自增 ID
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---@return integer id last insert id
function ctx:last_id()
    return self.owner:last_id()
end

---返回最近一次 UPDATE/DELETE/INSERT 影响的行数
---**须在命令返回后、本协程下次挂起之前读取**：这是每连接一份的状态，被最近一条完成的
---命令覆盖。一旦让出，别的协程可能已在同一连接上跑完自己的命令并把它改掉
---@return integer rows affected rows
function ctx:affectd_rows()
    return self.owner:affectd_rows()
end

return ctx
