-- 日志模块：将 FATAL/ERROR/WARN/INFO/DEBUG 五个全局函数注入到调用方命名空间。
-- 内部通过 debug.getinfo(3) 获取实际调用位置（文件名+行号），
-- 再转发给 C 层 srey.utils.log 落盘/输出，与 C 代码日志格式保持一致。

local utils = require("srey.utils")

-- 日志级别常量，与 C 层 log_level 枚举对应。
local LOG_LV = {
    FATAL = 0x00,
    ERROR = 0x01,
    WARN  = 0x02,
    INFO  = 0x03,
    DEBUG = 0x04
}

-- 每次现问 C 层要级别，不在 Lua 端缓存：级别是进程级的一份，而每个 task 有自己的 lua_State，
-- 缓存下来就变成每个 VM 一份——调试台把级别调高只对收到命令的那个 task 生效，命令打给 C task
-- 时更是没有任何 VM 会知道。log_getlv 只是一次 ATOMIC_GET，而短路真正要省的是下面的
-- debug.getinfo + string.format
local _getlv = utils.log_getlv

---内部公共日志函数；按级别短路后定位调用位置（debug.getinfo(3) 跳过 _log 与 FATAL/ERROR 等包装层）
---@param lv integer 日志级别（LOG_LV.*）
---@param fmt string 格式串
---@param ... any 格式参数
local function _log(lv, fmt, ...)
    if lv > _getlv() then
        return
    end
    -- "Sl" 只算 source/short_src/linedefined/what/currentline;默认 "flnStu" 还要做
    -- 'n' 的调用名反查(顺调用方字节码找函数名)与 'f'/'u'/'t' 那几项,结果表也大一倍
    local info = debug.getinfo(3, "Sl")
    if not info then
        return
    end
    utils.log(lv, info.short_src, info.currentline, string.format(fmt, ...))
end

---动态调整运行时日志级别（进程级，全部 task 立即生效）；非法级别（非整数或越界）返回 false 不改状态
---@param lv integer 新日志级别（LOG_LV.* FATAL..DEBUG）
---@return boolean ok 合法并已设置返回 true，否则 false
function log_setlv(lv)
    if math.type(lv) ~= "integer" or lv < LOG_LV.FATAL or lv > LOG_LV.DEBUG then
        return false
    end
    utils.log_setlv(lv)
    return true
end

---输出 FATAL 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function FATAL(fmt, ...)
    _log(LOG_LV.FATAL, fmt, ...)
end

---输出 ERROR 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function ERROR(fmt, ...)
    _log(LOG_LV.ERROR, fmt, ...)
end

---输出 WARN 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function WARN(fmt, ...)
    _log(LOG_LV.WARN, fmt, ...)
end

---输出 INFO 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function INFO(fmt, ...)
    _log(LOG_LV.INFO, fmt, ...)
end

---输出 DEBUG 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function DEBUG(fmt, ...)
    _log(LOG_LV.DEBUG, fmt, ...)
end
