require("lib.funcs")
local srey = require("srey.core")
local log = {}

local function _log(loglv, msg)
    local info = debug.getinfo(3)
    if nil == info then
        return
    end
    srey.log(loglv, info.source, info.currentline, dump(msg))
end
--设置log级别 是否打印
function log.setlog(lv, prt)
    srey.setlog(lv, prt)
end
--致命
function log.FATAL(msg)
    _log(LOG_LV.FATAL, msg)
end
--错误
function log.ERROR(msg)
    _log(LOG_LV.ERROR, msg)
end
--告警
function log.WARN(msg)
    _log(LOG_LV.WARN, msg)
end
--信息
function log.INFO(msg)
    _log(LOG_LV.INFO, msg)
end
--debug
function log.DEBUG(msg)
    _log(LOG_LV.DEBUG, msg)
end

return log
