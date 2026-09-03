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

local srey = require("lib.srey")
local pgsql = require("srey.pgsql")-- C 绑定，只用 pack_type / erro

local M = {}

---发送失败或连接已断
M.SEND = "pgsql: send failed or connection closed"

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
function M.check_type(pgpack, want)
    local pktype = pgsql.pack_type(pgpack)
    if want == pktype then
        return nil
    end
    if PGPACK_TYPE.ERR == pktype then
        return pgsql.erro(pgpack) or ""
    end
    return M.type_mismatch(pktype)
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

---只关心"这一趟成没成"的单次往返：取 fd/skid → syn_send → 判响应类型。
---与 M.request 的区别是全程不碰 err——ping / stmt:close 属于文件头 err 契约的例外，
---写了只会把上一次有用的错误抹掉。形状同 mysql_pub.request_ok
---@param self any 带 pg 字段的 ctx（pgsql_ctx 或 pgsql_stmt_ctx）
---@param pack lightuserdata 已组好的请求包，所有权随 syn_send 转移
---@param size integer 包字节数
---@param want PGPACK_TYPE 期望的响应包类型
---@return boolean ok 收到包且类型相符
function M.request_ok(self, pack, size, want)
    local fd, skid = self.pg:sock_id()
    local pgpack = srey.syn_send(fd, skid, pack, size, 0)
    return nil ~= pgpack and want == pgsql.pack_type(pgpack)
end

---组好包之后的固定四步：取 fd/skid → syn_send → 发送失败写 SEND → 按 want 校验响应类型。
---七个请求点共用；ping / stmt:close 不走这里——它们从不写 err，理由见文件头的 err 契约
---@param self any 带 pg 与 err 字段的 ctx（pgsql_ctx 或 pgsql_stmt_ctx）
---@param pack lightuserdata 已组好的请求包，所有权随 syn_send 转移
---@param size integer 包字节数
---@param want PGPACK_TYPE 期望的响应包类型
---@return lightuserdata|false pgpack 响应包；失败时 err 已写好并返回 false
function M.request(self, pack, size, want)
    local fd, skid = self.pg:sock_id()
    local pgpack = srey.syn_send(fd, skid, pack, size, 0)
    if not pgpack then
        return M.fail(self, M.SEND)
    end
    local e = M.check_type(pgpack, want)
    if e then
        return M.fail(self, e)
    end
    return pgpack
end

---复位"最近一次操作"的两项状态。必须一起复位：只复位 err 的话，命令失败直接 return 时
---affected_rows() 报的还是上一条成功命令的行数——一次失败的写被记成影响了 N 行。挂法同 M.fail
---@param self any 带 err 与 affected 字段的 ctx
function M.reset(self)
    self.err = ""
    self.affected = 0
end

return M
