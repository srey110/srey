local srey = require("lib.srey")

local function add(a, b)
    return a + b
end
srey.regrpc("add", add)

local linkcnt = 0
local recvcnt = 0
local sendcnt = 0
local lsn
local function sock_recv(sock, size)
    recvcnt = recvcnt + 1
    local buf = srey.bufremove(sock, size)
    srey.socksend(sock, buf)
end
local function sock_send(sock, size)
    sendcnt = sendcnt + 1
end
local function sock_close(sock)
    linkcnt = linkcnt - 1
end
local function acp_cb(sock)
    srey.enablerw(sock, sock_recv, sock_send, sock_close)
    linkcnt = linkcnt + 1
end
local function onstart()
    lsn = srey.listen("0.0.0.0", 15000, acp_cb)
    while true
    do
        print(os.date("%H:%M:%S") .. " " .. "linkcnt:" .. linkcnt .. " recvcnt:" .. recvcnt .. " sendcnt:" .. sendcnt)
        srey.sleep(2000)
    end
end
srey.start(onstart)

local function onstop()
    srey.freelsn(lsn)
end
srey.stop(onstop)
