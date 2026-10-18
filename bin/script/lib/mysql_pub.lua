-- mysql 客户端（mysql.lua）与预处理语句（mysql_stmt.lua）共用的响应包类型与判型。
-- 单独成模块而不是挂在某个类表上，理由同 pgsql_pub.lua：mysql.lua 依赖 mysql_stmt.lua，
-- 反过来 require 会成环；挂 class 表又会被 class() 的 __index 带到每个实例上冒出伪方法。
--
-- 与 pgsql_pub.request 的差别：mysql 侧没有 err 字段，且六个请求点的判型各不相同
-- （OK / STMT_PREPARE / 整个交给 _read_results），所以"发出去再收回来"直接用 conn_pub 的 _send_wait，
-- 判型留给调用方；只有出现最多的那一种（判 MPACK_OK）另给一个薄壳。

local mysql = require("srey.mysql")-- C 绑定，只用 pack_type

local M = {}

-- MySQL 响应包类型，与 C 层 mysql_pack_type 枚举一一对应。
---@enum MYSQL_PACK_TYPE
local MYSQL_PACK_TYPE = {
    MPACK_OK = 0x00,     -- 命令执行成功（无结果集）
    MPACK_ERR = 0x01,    -- 服务端返回错误
    MPACK_QUERY = 0x02,  -- 查询结果集
    MPACK_STMT_PREPARE = 0x03,  -- 预处理语句准备响应
    MPACK_STMT_EXECUTE = 0x04   -- 预处理语句执行响应
}
M.PACK_TYPE = MYSQL_PACK_TYPE

---conn_pub 的 _send_wait 之上再判一次 MPACK_OK，给"一个往返、应答只看成没成"的命令用（selectdb / ping / stmt reset）。
---必须正向判型，不能只判"收到了包"：conn_pub 的 _pingreconn 拿 _ping 的返回值当唯一重连判据，
---只判非 nil 的话，服务端以 ERR 应答（shutdown 期的 1053、连接被 KILL 之类）会被报成健康，
---于是永不重连，此后每一轮 ping + query 都重复失败。连接因前一次多结果集没收干净而错位时同理：
---读到的可能是上一条命令残留的包，判型能发现，只判非 nil 则会把它当自己的应答吃掉，错位无法自愈。
---收到的包先经 owner.mysql:record 落到连接级 erro / last_id / affectd_rows 上，同 mysql_ctx:_read_results
---@param owner any 持有连接的 mysql_ctx 实例（stmt 传它的宿主），调用方须已持 owner.serial
---@param pack lightuserdata 已组好的请求包，所有权随 syn_send 转移
---@param size integer 包字节数
---@return boolean ok 收到包且类型为 MPACK_OK
function M.request_ok(owner, pack, size)
    local mpack = owner:_send_wait(pack, size)
    if nil == mpack then
        return false
    end
    owner.mysql:record(mpack)
    return MYSQL_PACK_TYPE.MPACK_OK == mysql.pack_type(mpack)
end

return M
