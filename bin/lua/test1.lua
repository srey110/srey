local srey = require("lib.srey")

local function sock_recv(sock, size)
end
local function sock_send(sock, size)
end
local function sock_close(sock)
end
local conn
local function onstart()
    srey.sleep(1000)
    conn = srey.connect("127.0.0.1", 15000, 3000)
    if nil ~= conn then
        print("test1 srey.connect:" .. srey.sockid(conn))
        assert(srey.enablerw(conn, sock_recv, sock_send, sock_close))
    end
end
srey.start(onstart)

local test2 = nil
local function rpc_test()
    if nil == test2 then
        test2 = srey.grab("test2")
    end
    
    srey.call(test2, "add", 1, 3)    
    local ok, add = srey.request(test2, "add", 1, 3)
    assert(ok and 4 == add)
    
    srey.timeout(1000, rpc_test)
end
srey.timeout(1000, rpc_test)

local function onstop()
    srey.release(test2)
    srey.sockclose(conn)
end
srey.stop(onstop)
