-- 日志模块：将 FATAL/ERROR/WARN/INFO/DEBUG 五个全局函数注入到调用方命名空间。
-- 实际调用位置（文件名+行号）由 C 层 srey.utils.log 按栈层号自己取，
-- 落盘/输出格式与 C 代码日志一致。

local utils = require("srey.utils")
-- 每次现问 C 层要级别，不在 Lua 端缓存：级别是进程级的一份，而每个 task 有自己的 lua_State，
-- 缓存下来就变成每个 VM 一份——调试台把级别调高只对收到命令的那个 task 生效，命令打给 C task
-- 时更是没有任何 VM 会知道。log_getlv 只是一次 ATOMIC_GET，而短路真正要省的是下面的
-- 取调用位置 + string.format
local _getlv = utils.log_getlv

-- 日志级别常量，与 C 层 log_level 枚举对应。
local LOG_LV = {
    FATAL = 0x00,
    ERROR = 0x01,
    WARN  = 0x02,
    INFO  = 0x03,
    DEBUG = 0x04
}
local LV_FATAL = LOG_LV.FATAL
local LV_ERROR = LOG_LV.ERROR
local LV_WARN = LOG_LV.WARN
local LV_INFO = LOG_LV.INFO
local LV_DEBUG = LOG_LV.DEBUG

---内部公共日志函数。级别由包装函数先判，被压掉的日志不进这个变参函数。
---栈层 3 是业务调用方（0 utils.log、1 _log、2 FATAL/ERROR 等包装层），包装层必须普通调用 _log，不能尾调用，否则层数错位
---@param lv integer 日志级别（LOG_LV.*）
---@param fmt string 格式串
---@param ... any 格式参数
local function _log(lv, fmt, ...)
    utils.log(lv, 3, string.format(fmt, ...))
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
    if LV_FATAL > _getlv() then
        return
    end
    _log(LV_FATAL, fmt, ...)
end

---输出 ERROR 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function ERROR(fmt, ...)
    if LV_ERROR > _getlv() then
        return
    end
    _log(LV_ERROR, fmt, ...)
end

---输出 WARN 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function WARN(fmt, ...)
    if LV_WARN > _getlv() then
        return
    end
    _log(LV_WARN, fmt, ...)
end

---输出 INFO 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function INFO(fmt, ...)
    if LV_INFO > _getlv() then
        return
    end
    _log(LV_INFO, fmt, ...)
end

---输出 DEBUG 级别日志
---@param fmt string 格式串
---@param ... any 格式参数
function DEBUG(fmt, ...)
    if LV_DEBUG > _getlv() then
        return
    end
    _log(LV_DEBUG, fmt, ...)
end
