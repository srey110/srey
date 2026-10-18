-- pgsql 客户端（pgsql.lua）与预处理语句（pgsql_stmt.lua）共用的东西：失败原因常量、
-- 错误文案、响应包类型判定。
-- 单独成模块而不是挂在某个类表上：pgsql.lua 依赖 pgsql_stmt.lua，反过来 require 会成环；
-- 而挂 class 表又会被 class() 的 __index 带到每个实例上，冒出 inst.BUSY / inst:type_mismatch()
-- 这种伪方法——后者把实例当 pktype 传进去，返回 "unexpected pgsql response type: table: 0x..."，
-- 还跟真方法 inst:erro() 形似。放这里两边都 require，谁也不污染谁。
--
-- err 契约（两个文件共同遵守）：带 err 的操作入口一律先把 self.err 复位为 ""，
-- 否则失败路径不写 err 时 erro() 会把上一次无关的错误报给调用方，据此排障会走偏。
-- ping / cancel 不在此列：它们从不写 err，erro() 报告的始终是最后一次带契约操作的结果——
-- 若给它们加复位而不写内容，只会把上一次有用的错误抹成空串，更难排查。

local pgsql = require("srey.pgsql")-- C 绑定，只用 pack_type / erro / pack_copy_fail

local M = {}

-- PostgreSQL 响应包类型，与 C 层 pgpack_type 枚举一一对应。
---@enum PGPACK_TYPE
local PGPACK_TYPE = {
    OK           = 0x00, -- 命令执行成功
    ERR          = 0x01, -- 服务端返回错误
    NOTIFICATION = 0x02, -- 异步通知（LISTEN/NOTIFY）
    COPY_IN      = 0x03, -- COPY FROM STDIN 就绪
    COPY_OUT     = 0x04, -- COPY TO STDOUT 数据流
}
M.PACK_TYPE = PGPACK_TYPE
-- PostgreSQL 参数与结果集的传输格式，与 C 层取值一致。
---@enum PG_FORMAT
local PG_FORMAT = {
    TEXT   = 0, -- 文本
    BINARY = 1, -- 二进制
}
M.FORMAT = PG_FORMAT

---发送失败或连接已断
M.SEND = "pgsql: send failed or connection closed"
---组包失败：消息体超过 INT32_MAX（协议长度字段装不下）
M.PACK = "pgsql: pack failed, message exceeds INT32_MAX"
---锁内执行时抛了错（多为组包参数不对），原文在 ERROR 日志里
M.THROWN = "pgsql: command raised an error, see ERROR log"

---响应类型与预期不符时的 err 文案；各写各的会让同一故障在不同 API 上报出不一样的话
---@param pktype PGPACK_TYPE 实际收到的包类型
---@return string err 错误描述
function M.type_mismatch(pktype)
    return string.format("unexpected pgsql response type: %s", tostring(pktype))
end

---校验响应包类型：相符返回 nil，不符返回该写进 err 的文案（调用方 `return self:_fail(e)`）。
---两条判定合在一处，七个请求点共用：
---  1) 正向判"是不是期望的那个类型"，只判 ERR 不够——收到 OK 说明服务端不在预期状态，放过去会一路错下去
---  2) 不符时若是 ERR 包取服务端原文，erro() 对非 ERR 包返 nil，无脑 `or ""` 会留下空 err
---不需要防 NOTIFICATION：异步通知走 _pgsql_may_resume 分流给 srey.on_recved，到不了命令等待者
---@param pgpack lightuserdata 服务端响应包
---@param want PGPACK_TYPE 期望的包类型
---@return string? err 相符为 nil；不符为应写入 err 的文案
---@return PGPACK_TYPE? pktype 不符时为实际收到的包类型
function M.check_type(pgpack, want)
    local pktype = pgsql.pack_type(pgpack)
    if want == pktype then
        return nil
    end
    if PGPACK_TYPE.ERR == pktype then
        return pgsql.erro(pgpack) or "", pktype
    end
    return M.type_mismatch(pktype), pktype
end

---写 err 并返回 false 的合并写法，让"置原因"与"报失败"成为一步，不会只做一半。
---两个类各自 `ctx._fail = ppub.fail` 挂上去，故仍按 self:_fail(err) 调用
---@param self any 带 err 字段的 ctx（pgsql_ctx 或 pgsql_stmt_ctx）
---@param err string 失败原因
---@return boolean always false
function M.fail(self, err)
    self.err = err
    return false
end

---只关心"这一趟成没成"的单次往返：经连接的 _send_wait 发出并等应答 → 判响应类型。
---与 M.request 的区别是全程不碰 err——ping / stmt:close 属于文件头 err 契约的例外，
---写了只会把上一次有用的错误抹掉。形状同 mysql_pub.request_ok
---@param self any pgsql_ctx 或 pgsql_stmt_ctx（stmt 借宿主连接收发）
---@param pack lightuserdata? 已组好的请求包，所有权随 syn_send 转移；nil 为组包失败，不发直接返回 false
---@param size integer? 包字节数
---@param want PGPACK_TYPE 期望的响应包类型
---@return boolean ok 收到包且类型相符
function M.request_ok(self, pack, size, want)
    if not pack then
        return false
    end
    local pgpack = (self.owner or self):_send_wait(pack, size)
    return nil ~= pgpack and want == pgsql.pack_type(pgpack)
end

-- 服务端意外进了 COPY IN 态（COPY FROM STDIN 交给了 query 之类）：发 CopyFail 拉出来，应答只收不看。
-- stmt 走扩展协议：CopyFail 后服务端要等 Sync 才回 ReadyForQuery，Execute 带的那个已在 COPY IN 期被忽略，只能关连接
local function _copy_abort(self)
    if self.owner then
        self.owner:_closereset()
        return
    end
    local pack, size = pgsql.pack_copy_fail("unexpected COPY FROM STDIN")
    M.request_ok(self, pack, size, PGPACK_TYPE.ERR)
end

---组好包之后的固定三步：经连接的 _send_wait 发出并等应答 → 没等到写 SEND → 按 want 校验响应类型。
---七个请求点共用；ping / stmt:close 不走这里——它们从不写 err，理由见文件头的 err 契约。
---意外收到 COPY_IN 时先把服务端拉出 COPY IN 态再按类型不符返回，否则它攥着事务与表锁、下一条命令必失败
---@param self any 带 err 字段的 ctx（pgsql_ctx 或 pgsql_stmt_ctx，stmt 借宿主连接收发）
---@param pack lightuserdata? 已组好的请求包，所有权随 syn_send 转移；nil 为组包失败，写 PACK 不发
---@param size integer? 包字节数
---@param want PGPACK_TYPE 期望的响应包类型
---@return lightuserdata|false pgpack 响应包；失败时 err 已写好并返回 false
function M.request(self, pack, size, want)
    if not pack then
        return M.fail(self, M.PACK)
    end
    local pgpack = (self.owner or self):_send_wait(pack, size)
    if not pgpack then
        return M.fail(self, M.SEND)
    end
    local e, pktype = M.check_type(pgpack, want)
    if e then
        if PGPACK_TYPE.COPY_IN == pktype then
            _copy_abort(self)
        end
        return M.fail(self, e)
    end
    return pgpack
end

-- M.call 的返回收口
local function _settle(self, ok, ...)
    if ok then
        return ...
    end
    if "" == self.err then
        self.err = M.THROWN
    end
    return false
end
---带 err 契约的入口一律经此进锁执行 f（stmt 借宿主连接的执行器）。f 抛错时 serial 只打 ERROR 日志、
---折成失败，这里再补上 err，不让 erro() 返回空串
---@param self any pgsql_ctx 或 pgsql_stmt_ctx
---@param f fun(self:any, ...):any 锁内执行的方法
---@return any ... f 的全部返回值；f 抛错时为 false
function M.call(self, f, ...)
    return _settle(self, (self.owner or self).serial(f, self, ...))
end

---复位"最近一次操作"的两项状态。必须一起复位：只复位 err 的话，命令失败直接 return 时
---affected_rows() 报的还是上一条成功命令的行数——一次失败的写被记成影响了 N 行。挂法同 M.fail
---@param self any 带 err 与 affected 字段的 ctx
function M.reset(self)
    self.err = ""
    self.affected = 0
end

return M
