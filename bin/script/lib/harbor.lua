local srey = require("lib.srey")
local port = 8080
local signkey = ""

local function harbor_started()
    srey.listen("0.0.0.0", port, nil, 0, UNPACK_TYPE.SIMPLE)
end
srey.started(harbor_started)
