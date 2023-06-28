local sutils = require("srey.utils")
local debug = debug
local string = string
local log = {}

local LOG_LV = {
    ERROR = 0x00,
    WARN = 0x01,
    INFO = 0x02,
    DEBUG = 0x03
}
local function _log(loglv, fmt, ...)
    local info = debug.getinfo(3)
    if nil == info then
        return
    end
    sutils.log(loglv, info.source, info.currentline, string.format(fmt, ...))
end
function log.ERROR(fmt, ...)
    _log(LOG_LV.ERROR, fmt, ...)
end
function log.WARN(fmt, ...)
    _log(LOG_LV.WARN, fmt, ...)
end
function log.INFO(fmt, ...)
    _log(LOG_LV.INFO, fmt, ...)
end
function log.DEBUG(fmt, ...)
    _log(LOG_LV.DEBUG, fmt, ...)
end

return log
