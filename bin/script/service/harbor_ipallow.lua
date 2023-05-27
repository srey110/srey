local config = require("lib.config")

local ipallow = class("ipallow", config)
function ipallow:ctor(file)
    ipallow.super.ctor(self, file)
end
function ipallow:empty()
    return tbempty(self.m_content)
end
function ipallow:have(ip)
    return nil ~= self.m_content[ip]
end
function ipallow:add(ip, describe)
    self.m_content[ip] = (nil == describe and "" or describe)
end
function ipallow:remove(ip)
    self.m_content[ip] = nil
end

return ipallow
