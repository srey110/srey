local sutils = require("srey.utils")
local simple = {}

--[[
描述: 解包
参数:
    pack :simple_head_ctx
返回:
    data, size
--]]
function simple.unpack(pack)
    return sutils.simple_unpack(pack)
end
--[[
描述: 打包
参数:
    pack : string or userdata
    lens : integer
返回:
    data, size
--]]
function simple.pack(data, lens)
    return sutils.simple_pack(data, lens)
end

return simple
