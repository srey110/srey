local srey = require("lib.srey")
local log = require("lib.log")
local hbconfig = require("service.harbor_config")
local config = hbconfig.new("harbor_config.json")

local function addip(ip, describe)
    config:add(ip, describe)
end
srey.regrpc("addip", addip, "add ip in allow list.addip(ip:string, describe :string)")
local function removeip(ip)
    config:remove(ip)
end
srey.regrpc("removeip", removeip, "remove ip from allow list.removeip(ip:string)")

local function harbor_started()
    config:load()
    local ssl
    local name = config:sslname()
    if not strempty(name) then
        ssl = srey.sslevqury(name)
        assert(nil ~= ssl, string.format("qury ssl by name %s failed.", name))
    end
    srey.listen(config:lsnip(), config:port(), ssl, false, UNPACK_TYPE.RPC)
    log.INFO("harbor listen at %s:%d.", config:lsnip(), config:port())
end
srey.started(harbor_started)
local function harbor_closing()
    config:save()
end
srey.closing(harbor_closing)

local function harbor_accept(_, fd)
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
    srey.close(fd)
end
srey.accept(harbor_accept)
