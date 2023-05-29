local srey = require("lib.srey")
local rpcfd = INVALID_SOCK
local callonce = false
local function ontimeout()
    printd(".............................................")
    local rpctask = srey.qury(TASK_NAME.TAKS2)
    if not callonce then
        callonce = true
        local rpcdes = srey.describe(rpctask)
        --printd(rpcdes)
    end

    srey.call(rpctask, "rpc_add", math.random(10) , math.random(10), "srey.call")
    local rtn, sum, des = srey.request(rpctask, "rpc_add", math.random(10) , math.random(10), "srey.request")
    printd("request rpc_add rtn:" .. tostring(rtn) .. " sum:" .. tostring(sum) .. " des:" .. tostring(des))
    rtn = srey.request(rpctask, "rpc_void")
    printd("request rpc_void rtn:" .. tostring(rtn))
    if INVALID_SOCK ~= rpcfd then
        --[[srey.netcall(rpcfd, TASK_NAME.TAKS2,
                    "rpc_add", math.random(10) , math.random(10), "srey.netcall")--]]
        rtn, sum, des = srey.netreq(rpcfd, TASK_NAME.TAKS2,
                                   "rpc_add", math.random(10) , math.random(10), "srey.netreq")
        printd("netreq rpc_add rtn:" .. tostring(rtn) .. " sum:" .. tostring(sum) .. " des:" .. tostring(des))
        --[[rtn = srey.netreq(rpcfd, TASK_NAME.TAKS2, "rpc_void")
        printd("netreq rpc_void rtn:" .. tostring(rtn))--]]
    end
    srey.timeout(5000, ontimeout)
end
local function onstarted()
    math.randomseed(os.time())
    printd(srey.name() .. " onstarted....")
    srey.listen("0.0.0.0", 15000, nil, true, UNPACK_TYPE.SIMPLE)
    srey.timeout(2000, ontimeout)

    local harbor = srey.qury(TASK_NAME.HARBOR)
    srey.call(harbor, "addip", "192.168.100.1", "1")
    srey.call(harbor, "addip", "192.168.100.2", "2")
    srey.call(harbor, "addip", "192.168.100.3", "3")
    srey.call(harbor, "addip", "192.168.100.1", "1")
    srey.call(harbor, "addip", "192.168.100.4", "4")
    srey.call(harbor, "removeip", "192.168.100.5")
    srey.call(harbor, "removeip", "192.168.100.2")

    printd(srey.name() .. " start connect....")
    local ssl = srey.sslevqury("client")
    rpcfd = srey.connect("127.0.0.1", 8080, ssl, false, UNPACK_TYPE.RPC)
    if INVALID_SOCK ~= rpcfd then
        printd(srey.name() .. " end connect.... fd:" .. rpcfd)
    else
        printd(srey.name() .. " end connect.... error")
    end
end
srey.started(onstarted)

local function onaccept(unptype, fd)
    --prind(srey.name() .. " onaccept.... " .. fd)
end
srey.accept(onaccept)

local function echo(unptype, fd, data, size)
    srey.send(fd, data, size, PACK_TYPE.SIMPLE)
end
srey.recv(echo)

local function onsended(unptype, fd, size)
end
srey.sended(onsended)

local function onsockclose(unptype, fd)
    if unptype == UNPACK_TYPE.RPC and rpcfd == fd then
        printd(srey.name() .. " rpc connect closed")
        rpcfd = INVALID_SOCK
    end
end
srey.closed(onsockclose)

local function onclosing()
    printd(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
