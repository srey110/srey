local srey = require("lib.srey")
local http = require("lib.http")
local rpcfd = INVALID_SOCK
local callonce = false

local function chuncedmesg(cnt)
    if cnt.cnt >= 3 then
        return nil
    end
    cnt.cnt = cnt.cnt + 1
    return "this is chuncked "..tostring(cnt.cnt)
end
local function  onchunked(data, lens)
    if data then
        local dinfo = srey.utostr(data, lens)
        printd("hunked: %s", dinfo)
    else
        printd("hunked: end")
    end
end
local function httptest()
    local ssl = srey.sslevqury("client")
    local fd = srey.connect("127.0.0.1", 15003, UNPACK_TYPE.HTTP, ssl, false)
    if INVALID_SOCK == fd then
        return
    end
    local headers = {
        Host = "127.0.0.1:15003",
        Accept = "text/html, application/xhtml+xml, image/jxr, */*"
    }
    local jmsg = {
        a = 1,
        b = 2,
        c = 3
    }
    local cnt = {
        cnt = 0;
    }
    local hinfo
    hinfo = http.get(fd, "/get_test", headers)
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, "/post_nomsg", headers)
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, "/post_string", headers, "this is string message")
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, "/post_json", headers, nil, jmsg)
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, "/post_chunked", headers, nil, chuncedmesg, cnt)
    if not hinfo then
        printd("http error.")
    end
    cnt.cnt = 0
    hinfo = http.post(fd, "/get_chunked_cb", headers, onchunked, chuncedmesg, cnt)
    if not hinfo then
        printd("http error.")
    end
    srey.close(fd)
end
local function ontimeout()
    printd(".............................................")
    local rpctask = srey.qury(TASK_NAME.TAKS2)
    if not callonce then
        callonce = true
        local rpcdes = srey.describe(rpctask)
        httptest()
        --printd(rpcdes)
    end

    srey.call(rpctask, "rpc_add", math.random(10) , math.random(10), "srey.call")
    local rtn, sum, des = srey.request(rpctask, "rpc_add", math.random(10) , math.random(10), "srey.request")
    printd("request rpc_add rtn:" .. tostring(rtn) .. " sum:" .. tostring(sum) .. " des:" .. tostring(des))
    rtn = srey.request(rpctask, "rpc_void")
    printd("request rpc_void rtn:" .. tostring(rtn))
    if INVALID_SOCK ~= rpcfd then
        srey.netcall(rpcfd, TASK_NAME.TAKS2,
                    "rpc_add", math.random(10) , math.random(10), "srey.netcall")--]]
        rtn, sum, des = srey.netreq(rpcfd, TASK_NAME.TAKS2,
                                   "rpc_add", math.random(10) , math.random(10), "srey.netreq")
        printd("netreq rpc_add rtn:" .. tostring(rtn) .. " sum:" .. tostring(sum) .. " des:" .. tostring(des))
        rtn = srey.netreq(rpcfd, TASK_NAME.TAKS2, "rpc_void")
        printd("netreq rpc_void rtn:" .. tostring(rtn))
    end
    --httptest()
    srey.timeout(5000, ontimeout)
end
local function onstarted()
    math.randomseed(os.time())
    printd(srey.name() .. " onstarted....")
    srey.register("test.task3", TASK_NAME.TAKS3)
    srey.listen("0.0.0.0", 15000, UNPACK_TYPE.SIMPLE, nil, true)
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
    rpcfd = srey.connect("127.0.0.1", 8080, UNPACK_TYPE.RPC, ssl, false)
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
srey.accepted(onaccept)

local function echo(unptype, fd, data, size)
    data, size = srey.simple_data(data)
    srey.send(fd, data, size, PACK_TYPE.SIMPLE)
end
srey.recved(echo)

local function onsended(unptype, fd, size)
end
srey.sended(onsended)

local function onsockclose(unptype, fd)
    if unptype == UNPACK_TYPE.RPC and rpcfd == fd then
        printd(srey.name() .. "................. error rpc connect closed")
        rpcfd = INVALID_SOCK
    end
end
srey.closed(onsockclose)

local function onclosing()
    printd(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
