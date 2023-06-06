local srey = require("lib.srey")
local http = require("lib.http")
local websock = require("lib.websock")
local math = math
local rpcfd = INVALID_SOCK
local callonce = false

local function chuncedmesg(cnt)
    if cnt.cnt >= 3 then
        return nil
    end
    cnt.cnt = cnt.cnt + 1
    return "this is chuncked "..tostring(cnt.cnt)
end
local function  onchunked(data)
    if #data > 0 then
        --printd("hunked: %s", data)
    else
        --printd("hunked: end")
    end
end
local function httptest()
    local ssl = srey.sslevqury("client")
    local fd = srey.connect("127.0.0.1", 15003, PACK_TYPE.HTTP, nil)
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
    hinfo = http.get(fd, "/get_test?a=中文测试1", headers)
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, "/post_nomsg?a=中文测试2", headers)
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, "/post_string", headers, nil, "this is string message")
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
local function testrpc()
    local rpctask = srey.qury(TASK_NAME.TASK2)
    local rpcdes = srey.describe(rpctask)
    --printd(rpcdes)
    srey.call(rpctask, "rpc_add", math.random(10) , math.random(10), "srey.call")
    local rtn, sum, des = srey.request(rpctask, "rpc_add", math.random(10) , math.random(10), "srey.request")
    if not rtn then
        printd("srey.request failed")
    end
    rtn = srey.request(rpctask, "rpc_void")
    if not rtn then
        printd("srey.request failed")
    end
    if INVALID_SOCK ~= rpcfd then
        srey.netcall(rpcfd, TASK_NAME.TASK2, "rpc_add", math.random(10) , math.random(10), "srey.netcall")
        rtn, sum, des = srey.netreq(rpcfd, TASK_NAME.TASK2, "rpc_add", math.random(10) , math.random(10), "srey.netreq")
        if not rtn then
            printd("srey.netreq failed")
        end
        rtn = srey.netreq(rpcfd, TASK_NAME.TASK2, "rpc_void")
        if not rtn then
            printd("srey.netreq failed")
        end
    end
end
local function continuationcb(cnt)
    cnt.cnt = cnt.cnt + 1
    if cnt.cnt >= 3 then
        return true, "this is continuation "..tostring(cnt.cnt)
    end
    return false, "this is continuation "..tostring(cnt.cnt)
end
local function testwebsock(upgrade)
    local cnt = {
        cnt = 0;
    }
    local ssl = srey.sslevqury("client")
    local websockfd
    if upgrade then
        websockfd = srey.connect("127.0.0.1", 15003, PACK_TYPE.HTTP, nil)
    else
        websockfd = srey.connect("127.0.0.1", 15004, PACK_TYPE.WEBSOCK, nil)
    end
    if INVALID_SOCK == websockfd then
        printd("end websock connect.... error")
        return
    end
    if upgrade and not websock.handshake(websockfd, "/") then
        printd("websock handshake failed.")
        return
    end
    websock.ping(websockfd)
    websock.pong(websockfd)
    websock.text(websockfd, randstr(math.random(10, 4096)), nil, randstr(4))
    websock.binary(websockfd, randstr(math.random(10, 4096)), nil, randstr(4))
    websock.continuation(websockfd, randstr(4), continuationcb, cnt)
    websock.close(websockfd)
end
local function testwebsock2()
    local cnt = {
        cnt = 0;
    }
    local ssl = srey.sslevqury("client")
    local websockfd =  srey.connect("127.0.0.1", 15003, PACK_TYPE.WEBSOCK, nil)
    if INVALID_SOCK == websockfd then
        printd("end websock connect.... error")
        return
    end
    websock.ping(websockfd)
    websock.pong(websockfd)
    websock.text(websockfd, randstr(math.random(10, 4096)), nil, randstr(4))
    websock.binary(websockfd, randstr(math.random(10, 4096)), nil, randstr(4))
    websock.continuation(websockfd, randstr(4), continuationcb, cnt)
    websock.close(websockfd)
end
local function ontimeout()
    --printd(".............................................")
    if not callonce then
        callonce = true
    end
    httptest()
    testrpc()
    testwebsock(true)
    testwebsock(false)
    testwebsock2()
    srey.timeout(3000, ontimeout)
end
local function onstarted()
    printd(srey.name() .. " onstarted....")
    srey.listen("0.0.0.0", 15000, PACK_TYPE.SIMPLE, nil, true)
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
    rpcfd = srey.connect("127.0.0.1", 8080, PACK_TYPE.RPC, ssl, false)
    if INVALID_SOCK ~= rpcfd then
        printd("rpc end connect.... fd:" .. rpcfd)
    else
        printd("rpc end connect.... error")
    end
end
srey.started(onstarted)

local function onaccept(pktype, fd)
    --printd(srey.name() .. " onaccept.... " .. fd)
end
srey.accepted(onaccept)

local function echo(pktype, fd, data)
    if PACK_TYPE.SIMPLE == pktype then
        if math.random(1, 100) <= 1 then
            srey.close(fd)
            return
        end
        srey.send(fd, data, nil, pktype)
    elseif PACK_TYPE.WEBSOCK == pktype then
        if WEBSOCK_PROTO.PING == data.proto then
            --printd("PING")
        elseif WEBSOCK_PROTO.CLOSE == data.proto then
            --printd("CLOSE")
        elseif WEBSOCK_PROTO.TEXT == data.proto  then
            --printd("TEXT size: %d", frame.size)
        elseif WEBSOCK_PROTO.BINARY == data.proto  then
            --printd("BINARY size: %d", frame.size)
        elseif WEBSOCK_PROTO.CONTINUE == data.proto then
            --printd("CONTINUE fin: %d size: %d", frame.fin, frame.size)
        end
    end
end
srey.recved(echo)

local function onsended(pktype, fd, size)
end
srey.sended(onsended)

local function onsockclose(pktype, fd)
    if pktype == PACK_TYPE.RPC and rpcfd == fd then
        printd("rpc connect closed")
        rpcfd = INVALID_SOCK
    end
    if pktype == PACK_TYPE.WEBSOCK then
        --printd("websocket connect closed")
    end
end
srey.closed(onsockclose)

local function onclosing()
    printd("onclosing....")
end
srey.closing(onclosing)
