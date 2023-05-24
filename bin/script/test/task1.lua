local srey = require("lib.srey")

local function ontimeout()
	print(os.date("%H-%M-%S", os.time()) .. " task:".. srey.name() .. " ontimeout....")
	srey.timeout(3000, ontimeout)
end
local function onstarted()
	print(srey.name() .. " onstarted....")
	srey.listen(UNPACK_TYPE.SIMPLE, "0.0.0.0", 15000, nil, 1)
	srey.timeout(3000, ontimeout)
	print(srey.name() .. " sart sleep 3s.." .. os.date("%H-%M-%S", os.time()))
	srey.sleep(3000)
	print(srey.name() .. " end sleep 3s.." .. os.date("%H-%M-%S", os.time()))
	print(srey.name() .. " start connect....")
	local ssl = srey.sslevqury("client")
	local fd = srey.connect(UNPACK_TYPE.SIMPLE, "127.0.0.1", 15001, ssl)
	if nil ~= fd then
		print(srey.name() .. " end connect.... fd:" .. fd)
	else
		print(srey.name() .. " end connect.... error")
	end
end
srey.started(onstarted)

local function onaccept(ptype, fd)
	print(srey.name() .. " onaccept.... " .. fd)
end
srey.accept(onaccept)

local function echo(ptype, fd, data, size)
	srey.send(fd, data, size, PACK_TYPE.SIMPLE)
end
srey.recv(echo)

local function onsended(ptype, fd, size)
	--print(fd .. " sended size:" .. size)
end
srey.sended(onsended)

local function onsockclose(ptype, fd)
	print(fd .. " closed")
end
srey.closed(onsockclose)

local function onclosing()
	print(srey.name() .. " onclosing....")
end
srey.closing(onclosing)