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

local pgsql = require("pgsql")-- C 绑定，只用 pack_type / erro

local M = {}

---connect() 进行中，操作被 fail-fast 拒绝
M.BUSY = "pgsql: connect() in progress"
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
---  1) 必须正向判"是不是期望的那个类型"，只判 ERR 不够——syn_send 只取"下一条 RECV"、
---     不区分包类型，服务端随时可能插进 NOTIFICATION 之类独立包；把它当成功会让真正的
---     响应留到下次被错认，该连接的请求-响应从此整体错位一格
---  2) 不符时若是 ERR 包，取服务端原文而非"类型不符"——erro() 对非 ERR 包返 nil，
---     无脑 `or ""` 会留下空 err，调用方拿不到任何线索
---@param pgpack lightuserdata 服务端响应包
---@param want PGPACK_TYPE 期望的包类型（PGPACK_TYPE 是 pgsql_stmt.lua 定义的全局，调用期已加载）
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

return M
