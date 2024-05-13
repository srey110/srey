local utils = require("srey.utils")
local LOG_LV = {
    FATAL = 0x00,
    ERROR = 0x01,
    WARN = 0x02,
    INFO = 0x03,
    DEBUG = 0x04
}

local function _log(lv, fmt, ...)
    local info = debug.getinfo(3)
    if not info then
        return
    end
    utils.log(lv, info.source, info.currentline, string.format(fmt, ...))
end
function log_setlv(lv)
    utils.log_setlv(lv)
end
function FATAL(fmt, ...)
    _log(LOG_LV.FATAL, fmt, ...)
end
function ERROR(fmt, ...)
    _log(LOG_LV.ERROR, fmt, ...)
end
function WARN(fmt, ...)
    _log(LOG_LV.WARN, fmt, ...)
end
function INFO(fmt, ...)
    _log(LOG_LV.INFO, fmt, ...)
end
function DEBUG(fmt, ...)
    _log(LOG_LV.DEBUG, fmt, ...)
end
