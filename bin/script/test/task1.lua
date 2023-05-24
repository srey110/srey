local srey = require("lib.srey")

srey.listen(UNPACK_TYPE.SIMPLE, "0.0.0.0", 15000)

local function onclosing()
	print(srey.name() .. "onclosing....")
end
srey.closing(onclosing)