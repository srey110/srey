local sutils = require("srey.utils")
local simple = {}

--[[
描述: 解包
参数:
    data :userdata
    toud 数据是否以userdata方式传递 0 否 1 是
返回:
    table
--]]
function simple.unpack(data, toud)
    return sutils.simple_pack(data, toud)
end

return simple
