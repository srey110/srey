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
    return sutils.simple_pack(pack)
end

return simple
