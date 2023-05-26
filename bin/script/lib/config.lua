local srey = require("lib.srey")
local json = require("cjson")
local pathsep = srey.pathsep()
local config_path = string.format("%s%sconfigs%s", srey.path(), pathsep, pathsep)

local config = class("config")
function config:ctor(file)
    self.m_file = string.format("%s%s", config_path, file)
    self.m_content = {}
end
function config:load()
    local file = io.open(self.m_file , "r")
    if nil == file then
        return
    end
    local info = file:read("a")
    file:close()
    if 0 == #info then
        return
    end
    self.m_content = json.decode(info)
end
function config:save()
    local file = io.open(self.m_file, "w+")
    if nil == file then
        return
    end
    file:write(json.encode(self.m_content))
    file:close()
end

return config
