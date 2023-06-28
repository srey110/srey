local srey = require("lib.srey")
local log = require("lib.log")
local hbconfig = require("service.harbor_config")
local config = hbconfig.new("harbor_config.json")

local function addip(ip, describe)
    config:add(ip, describe)
end
srey.regrpc("addip", addip)
local function removeip(ip)
    config:remove(ip)
end
srey.regrpc("removeip", removeip)

local function harbor_started()
    config:load()
    local ssl
    local name = config:sslname()
    if not strempty(name) then
        ssl = srey.evssl_qury(name)
    end
    srey.listen(config:lsnip(), config:port(), PACK_TYPE.RPC, ssl)
    log.INFO("harbor listen at %s:%d.", config:lsnip(), config:port())
end
srey.started(harbor_started)
local function harbor_closing()
    config:save()
end
srey.closing(harbor_closing)

local function harbor_accept(_, fd, skid)
    if config:empty() then
        return
    end
    local ip,_ = srey.remoteaddr(fd)
    if nil == ip then
        log.WARN("get remote ip port failed.")
        return
    end
    if config:have(ip) then
        return
    end
    log.WARN("block %s link.", ip)
    srey.close(fd, skid)
end
srey.accepted(harbor_accept)
