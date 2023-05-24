local srey = require("lib.srey")

local function onstarted()
	print(srey.name() .. " onstarted....")
	local ssl = srey.sslevqury("server")
	srey.listen(UNPACK_TYPE.SIMPLE, "0.0.0.0", 15001, ssl)
end
srey.started(onstarted)

local function echo(fd, data, size)
	srey.send(fd, data, size, PACK_TYPE.SIMPLE)
end
srey.recv(echo)

local function onclosing()
	print(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
