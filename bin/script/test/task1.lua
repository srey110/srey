local srey = require("lib.srey")
local function ontimeout()
    local rpctask = srey.qury(TASK_NAME.TAKS2)
    local rpcdes = srey.describe(rpctask)
    srey.call(rpctask, "rpc_add", math.random(10) , math.random(10), math.random(10))
    local sum = srey.request(rpctask, "rpc_add", math.random(10) , math.random(10), math.random(10))
    print(os.date("%H-%M-%S", os.time()) .. " request rpc_add rtn:" .. tostring(sum))
    srey.timeout(3000, ontimeout)
end
local function onstarted()
    math.randomseed(os.time())
    print(srey.name() .. " onstarted....")
    srey.listen("0.0.0.0", 15000, nil, 1, UNPACK_TYPE.SIMPLE)
    srey.timeout(3000, ontimeout)

    local harbor = srey.qury(TASK_NAME.HARBOR)
    srey.call(harbor, "addip", "192.168.100.1", "1")
    srey.call(harbor, "addip", "192.168.100.2", "2")
    srey.call(harbor, "addip", "192.168.100.3", "3")
    srey.call(harbor, "addip", "192.168.100.1", "1")
    srey.call(harbor, "addip", "192.168.100.4", "4")
    srey.call(harbor, "removeip", "192.168.100.5")
    srey.call(harbor, "removeip", "192.168.100.2")
    
    print(srey.name() .. " sart sleep 3s.." .. os.date("%H-%M-%S", os.time()))
    srey.sleep(3000)
    print(srey.name() .. " end sleep 3s.." .. os.date("%H-%M-%S", os.time()))
    
    print(srey.name() .. " start connect....")
    local ssl = srey.sslevqury("client")
    local fd = srey.connect("127.0.0.1", 15001, ssl, 0, UNPACK_TYPE.SIMPLE)
    if nil ~= fd then
        print(srey.name() .. " end connect.... fd:" .. fd)
    else
        print(srey.name() .. " end connect.... error")
    end
end
srey.started(onstarted)

local function onaccept(unptype, fd)
    print(srey.name() .. " onaccept.... " .. fd)
end
srey.accept(onaccept)

local function echo(unptype, fd, data, size)
    srey.send(fd, data, size, PACK_TYPE.SIMPLE)
end
srey.recv(echo)

local function onsended(unptype, fd, size)
    --print(fd .. " sended size:" .. size)
end
srey.sended(onsended)

local function onsockclose(unptype, fd)
    print(fd .. " closed")
end
srey.closed(onsockclose)

local function onclosing()
    print(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
