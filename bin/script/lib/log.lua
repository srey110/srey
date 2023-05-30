local srey = require("srey.core")
local debug = debug
local string = string
local log = {}

--日志级别
local LOG_LV = {
    FATAL = 0x00,
    ERROR = 0x01,
    WARN = 0x02,
    INFO = 0x03,
    DEBUG = 0x04
}
local function _log(loglv, fmt, ...)
    local info = debug.getinfo(3)
    if nil == info then
        return
    end
    srey.log(loglv, info.source, info.currentline, string.format(fmt, ...))
end
--设置log级别 是否打印
function log.setlog(lv, prt)
    srey.setlog(lv, prt)
end
--致命
function log.FATAL(fmt, ...)
    _log(LOG_LV.FATAL, fmt, ...)
end
--错误
function log.ERROR(fmt, ...)
    _log(LOG_LV.ERROR, fmt, ...)
end
--告警
function log.WARN(fmt, ...)
    _log(LOG_LV.WARN, fmt, ...)
end
--信息
function log.INFO(fmt, ...)
    _log(LOG_LV.INFO, fmt, ...)
end
--debug
function log.DEBUG(fmt, ...)
    _log(LOG_LV.DEBUG, fmt, ...)
end

return log
