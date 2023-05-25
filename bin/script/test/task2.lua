local srey = require("lib.srey")

local function onstarted()
    print(srey.name() .. " onstarted....")
    local ssl = srey.sslevqury("server")
    srey.listen("0.0.0.0", 15001, ssl)
end
srey.started(onstarted)

local function echo(unptype, fd, data, size)
    srey.send(fd, data, size)
end
srey.recv(echo)

local function onclosing()
    print(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
