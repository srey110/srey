require("lib.funcs")
local srey = require("lib.srey")
local cbs = require("lib.cbs")

local function startup()
    local fd, _ = srey.udp("0.0.0.0", 15002)
    if INVALID_SOCK == fd then
        printd("udp error.")
    end
end
cbs.cb_startup(startup)

local function recvfrom(msg)
    srey.sendto(msg.fd, msg.skid, msg.ip, msg.port, msg.udata, msg.size)
end
cbs.cb_recvfrom(recvfrom)
