local srey = require("lib.srey")

srey.udp("0.0.0.0", 15002)

local function onclosing()
	print(srey.name() .. "onclosing....")
end
srey.closing(onclosing)
