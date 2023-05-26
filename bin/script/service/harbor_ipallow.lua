local config = require("lib.config")

local ipallow = class("ipallow", config)
function ipallow:ctor(file)
    ipallow.super.ctor(self, file)
end
function ipallow:empty()
    return 0 == #self.m_content
end
function ipallow:have(ip)
    for _, value in ipairs(self.m_content) do
        if value == ip then
            return true
        end
    end
    return false
end
function ipallow:add(ip)
    if self:have(ip) then
        return
    end
    table.insert(self.m_content, ip)
end
function ipallow:remove(ip)
    for key, value in ipairs(self.m_content) do
        if value == ip then
            table.remove(self.m_content, key)
            return
        end
    end
end

return ipallow
