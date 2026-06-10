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

-- Lua 端缓存当前级别，初始化时从 C 层读取实际值，避免与 C 实际状态不一致。
-- 用于 _log 短路，避免无效日志的 debug.getinfo + string.format 开销。
-- 契约：所有 setlv 入口必须走本模块的 log_setlv，不可绕过直接调 utils.log_setlv。
local _curlv = utils.log_getlv()

---内部公共日志函数；按级别短路后定位调用位置（debug.getinfo(3) 跳过 _log 与 FATAL/ERROR 等包装层）
---@param lv integer 日志级别（LOG_LV.*）
---@param fmt string 格式串
---@param ... any 格式参数
local function _log(lv, fmt, ...)
    if lv > _curlv then
        return
    end
    local info = debug.getinfo(3)
    if not info then
        return
    end
    utils.log(lv, info.source, info.currentline, string.format(fmt, ...))
end

---动态调整运行时日志级别，同步更新 Lua 缓存与 C 层
---@param lv integer 新日志级别（LOG_LV.*）
function log_setlv(lv)
    _curlv = lv
    utils.log_setlv(lv)
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
