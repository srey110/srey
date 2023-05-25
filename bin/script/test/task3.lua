local srey = require("lib.srey")

local function onstarted()
	print(srey.name() .. " onstarted....")
	srey.udp("0.0.0.0", 15002)
end
srey.started(onstarted)

local function onrecvfrom(unptype, fd, data, size, ip, port)
	srey.sendto(fd, ip, port, data, size)
end
srey.recvfrom(onrecvfrom)

local function onclosing()
	print(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
