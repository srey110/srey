local core = require("srey.core")
local utile = require("lib.utile")
local debug = debug
local log = {}

local LOGLV = {
    FATAL = 0, 
    ERROR = 1, 
    WARN = 2, 
    INFO = 3, 
    DEBUG = 4
}
local function _log(loglv, msg)
    local info = debug.getinfo(3)
    if nil == info then
        return
    end
    core.log(loglv, info.source, info.currentline, utile.dump(msg))
end
function log.FATAL(msg)
    _log(LOGLV.FATAL, msg)
end
function log.ERROR(msg)
    _log(LOGLV.ERROR, msg)
end
function log.WARN(msg)
    _log(LOGLV.WARN, msg)
end
function log.INFO(msg)
    _log(LOGLV.INFO, msg)
end
function log.DEBUG(msg)
    _log(LOGLV.DEBUG, msg)
end

return log
