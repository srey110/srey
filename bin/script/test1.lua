local srey = require("lib.srey")
local log = require("lib.log")

log.FATAL("FATAL")
log.ERROR("ERROR")
log.WARN("WARN")
log.INFO("INFO")
log.DEBUG("DEBUG")

srey.listen(1, "0.0.0.0", 15000)
local ssl = srey.sslevqury("server")
srey.listen(1, "0.0.0.0", 15001, ssl)
srey.udp("0.0.0.0", 15002)

local function onclosing()
	print("onclosing....")
end
srey.closing(onclosing)