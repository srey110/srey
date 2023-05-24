local srey = require("lib.srey")

local ssl = srey.sslevqury("server")
srey.listen(UNPACK_TYPE.SIMPLE, "0.0.0.0", 15001, ssl)

local function onclosing()
	print(srey.name() .. "onclosing....")
end
srey.closing(onclosing)
