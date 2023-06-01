local srey = require("lib.srey")

local function onstarted()
    printd(srey.name() .. " onstarted....")
    local ssl = srey.sslevqury("server")
    srey.listen("0.0.0.0", 15001, UNPACK_TYPE.NONE, ssl)
end
srey.started(onstarted)

local function echo(unptype, fd, data, size)
    srey.send(fd, data, size)
end
srey.recved(echo)

local function rpc_add(a, b, des)
    local rtn = a + b
    --printd("%s", des)
    --srey.sleep(500)
    return rtn, des
end
srey.regrpc("rpc_add", rpc_add, "describe:rpc_add(a, b, c)")
local function rpc_void()
end
srey.regrpc("rpc_void", rpc_void, "describe:rpc_void)")

local function onclosing()
    printd(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
