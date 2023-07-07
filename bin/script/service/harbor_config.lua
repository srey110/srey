local config = require("lib.config")

local harbor_config = class("harbor_config", config)
function harbor_config:ctor(file)
    harbor_config.super.ctor(self, file)
    local content = self:content()
    content.ips = {}
    content.lsnip = "0.0.0.0"
    content.port = "8080"
    content.sslname = SSL_NAME.NONE
end
function harbor_config:lsnip()
    return self:content().lsnip
end
function harbor_config:port()
    return self:content().port
end
function harbor_config:sslname()
    return self:content().sslname
end
function harbor_config:empty()
    return tbempty(self:content().ips)
end
function harbor_config:have(ip)
    return nil ~= self:content().ips[ip]
end
function harbor_config:add(ip, describe)
    self:content().ips[ip] = describe or ""
end
function harbor_config:remove(ip)
    self:content().ips[ip] = nil
end

return harbor_config
