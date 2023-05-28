local config = require("lib.config")

local harbor_config = class("harbor_config", config)
function harbor_config:ctor(file)
    harbor_config.super.ctor(self, file)
    self.m_content.ips = {}
    self.m_content.lsnip = "0.0.0.0"
    self.m_content.port = "8080"
    self.m_content.sslname = ""
end
function harbor_config:lsnip()
    return self.m_content.lsnip
end
function harbor_config:port()
    return self.m_content.port
end
function harbor_config:sslname()
    return self.m_content.sslname
end
function harbor_config:empty()
    return tbempty(self.m_content.ips)
end
function harbor_config:have(ip)
    return nil ~= self.m_content.ips[ip]
end
function harbor_config:add(ip, describe)
    self.m_content.ips[ip] = (nil == describe and "" or describe)
end
function harbor_config:remove(ip)
    self.m_content.ips[ip] = nil
end

return harbor_config
