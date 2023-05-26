local srey = require("lib.srey")
local harbor_ipallow = require("service.harbor_ipallow")
local port = 8080
local signkey = "x3njVVstBXMAxNdxNbKINeBnS9fVyoR6"
local ipallow = harbor_ipallow.new("harbor_ipallow.json")

local function addip(ip, describe)
    ipallow:add(ip, describe)
end
srey.regrpc("addip", addip, "add ip in allow list.addip(ip:string, describe :string)")
local function removeip(ip)
    ipallow:remove(ip)
end
srey.regrpc("removeip", removeip, "remove ip from allow list.removeip(ip:string)")

local function harbor_started()
    ipallow:load()
    srey.listen("0.0.0.0", port, nil, 0, UNPACK_TYPE.SIMPLE)
end
srey.started(harbor_started)
local function harbor_closing()
    ipallow:save()
end
srey.closing(harbor_closing)

local function harbor_accept(_, fd)
    if ipallow:empty() then
        return
    end
    local ip,_ = srey.remoteaddr(fd)
    if nil == ip then
        return
    end
    if ipallow:have(ip) then
        return
    end
    srey.close(fd)
end
srey.accept(harbor_accept)

local function harbor_recv(unptype, fd, data, size)

end
srey.recv(harbor_recv)
