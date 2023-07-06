require("lib.define")
local sutils = require("srey.utils")
local debug = debug
local string = string
local log = {}

local function _log(loglv, fmt, ...)
    local info = debug.getinfo(3)
    if nil == info then
        return
    end
    sutils.log(loglv, info.source, info.currentline, string.format(fmt, ...))
end
function log.setlv(loglv)
    sutils.log_setlv(loglv)
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
