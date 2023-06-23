local srey = require("lib.srey")

local function onstarted()
    printd(srey.task_name() .. " onstarted....")
    local ssl = srey.evssl_qury("server")
    srey.listen("0.0.0.0", 15001, PACK_TYPE.NONE, ssl)
end
srey.started(onstarted)

local function echo(pktype, fd, skid, data, size)
    srey.send(fd, skid, pktype, data, size)
end
srey.recved(echo)

local function rpc_add(a, b, des)
    --printd(des)
    local rtn = a + b
    return rtn, des
end
srey.regrpc("rpc_add", rpc_add)
local function rpc_void()
end
srey.regrpc("rpc_void", rpc_void)

local function onclosing()
    printd(srey.task_name() .. " onclosing....")
end
srey.closing(onclosing)
