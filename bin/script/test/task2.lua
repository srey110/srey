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

local function rpc_add(a, b, c)
    local rtn = a + b + c
    return rtn
end
srey.regrpc("rpc_add", rpc_add, "describe:rpc_add(a, b, c)")

local function onclosing()
    print(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
