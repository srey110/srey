local srey = require("lib.srey")
local simple = require("lib.simple")
local http = require("lib.http")
local websock = require("lib.websock")
local math = math
local rpcfd = INVALID_SOCK
local rpcfdid = 0
local udpfd = INVALID_SOCK
local udpskid
local callonce = false

local function testrpc()
    local rpctask = srey.task_qury(TASK_NAME.TASK2)
    local a = math.random(10)
    local b = math.random(10)
    srey.call(rpctask, "rpc_add", a , b, "srey.call")
    a = math.random(10)
    b = math.random(10)
    local rtn, sum, des = srey.request(rpctask, "rpc_add", a , b, "srey.request")
    if not rtn or sum ~= a + b or "srey.request" ~= des then
        printd("srey.request rpc_add failed")
    end
    rtn = srey.request(rpctask, "rpc_void")
    if not rtn then
        printd("srey.request rpc_void failed")
    end
    if INVALID_SOCK ~= rpcfd then
        a = math.random(10)
        b = math.random(10)
        srey.netcall(rpcfd, rpcfdid, TASK_NAME.TASK2, "rpc_add", a , b, "srey.netcall")
        a = math.random(10)
        b = math.random(10)
        rtn, sum, des = srey.netreq(rpcfd, rpcfdid, TASK_NAME.TASK2, "rpc_add", a , b, "srey.netreq")
        if not rtn or sum ~= a + b or "srey.netreq" ~= des then
            printd("srey.netreq rpc_add failed")
        end
        rtn = srey.netreq(rpcfd, rpcfdid, TASK_NAME.TASK2, "rpc_void")
        if not rtn then
            printd("srey.netreq rpc_void failed")
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
local function testwebsock(ipv4, chs, port)
    local connip
    if ipv4 then
        connip = "127.0.0.1"
    else
        connip = "::1"
    end
    local ssl = srey.evssl_qury("client")
    local websockfd
    local skid
    if chs then
        websockfd, skid = websock.connect(connip, port)
        if INVALID_SOCK == websockfd then
            printd("websock connect.... error")
            return
        end
    else
        websockfd, skid = srey.connect(connip, port, PACK_TYPE.HTTP)
        if INVALID_SOCK == websockfd then
            printd("websock connect.... error")
            return
        end
        if not websock.handshake(websockfd, skid) then
            printd("websock handshake.... error")
            return
        end
    end
    websock.ping(websockfd, skid)
    websock.pong(websockfd, skid)
    websock.text(websockfd, skid, randstr(math.random(10, 4096)), nil, randstr(4))
    websock.binary(websockfd, skid, randstr(math.random(10, 4096)), nil, randstr(4))
    local cnt = {
        cnt = 0;
    }
    websock.text(websockfd, skid, randstr(math.random(10, 100)), nil, randstr(4), continuationcb, cnt)
    websock.close(websockfd, skid)
end
local function chuncedmesg(cnt)
    if cnt.cnt >= 3 then
        return nil
    end
    cnt.cnt = cnt.cnt + 1
    return "this is chuncked "..tostring(cnt.cnt)
end
local function onchunked(fd, skid, hinfo, data, size, fin)
    if not fin then
        --printd("hunked: %s", srey.utostr(data ,size))
    else
        --printd("hunked: end")
    end
end
local function testhttp()
    local ssl = srey.evssl_qury("client")
    local fd, skid = srey.connect("127.0.0.1", 15003, PACK_TYPE.HTTP, nil)
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
    hinfo = http.get(fd, skid, "/get_test?a=中文测试1", headers)
    if not hinfo or "HTTP/1.1" ~= hinfo.status[1] or "200" ~=  hinfo.status[2] then
        printd("http.get /get_test error.")
    end
    hinfo = http.post(fd, skid, "/post_nomsg?a=中文测试2", headers)
    if not hinfo or "HTTP/1.1" ~= hinfo.status[1] or "200" ~=  hinfo.status[2] then
        printd("http.post /post_nomsg error.")
    end
    hinfo = http.post(fd, skid, "/post_string", headers, nil, "this is string message")
    if not hinfo or "HTTP/1.1" ~= hinfo.status[1] or "200" ~=  hinfo.status[2] then
        printd("http.post /post_string error.")
    end
    hinfo = http.post(fd, skid, "/post_json", headers, nil, jmsg)
    if not hinfo or "HTTP/1.1" ~= hinfo.status[1] or "200" ~=  hinfo.status[2] then
        printd("http.post /post_json error.")
    end
    hinfo = http.post(fd, skid, "/get_chunked_cb", headers, onchunked, chuncedmesg, cnt)
    if not hinfo or "HTTP/1.1" ~= hinfo.status[1] or "200" ~=  hinfo.status[2]
       or not hinfo.fin or 72 ~= hinfo.cksize  then
        printd("http.post /get_chunked_cb error.")
    end
    srey.close(fd, skid)
end
local function testudp()
    if INVALID_SOCK == udpfd then
        udpfd, udpskid = srey.udp("0.0.0.0", 0)
    end
    if INVALID_SOCK ~= udpfd  then
        local resp, size = srey.synsendto(udpfd, udpskid, "127.0.0.1", 15002, "this is udp message.")
        if not resp or "this is udp message." ~= srey.utostr(resp, size) then
            printd("synsendto error.")
        end
    else
        printd(" srey.udp error.")
    end
end
local function ontimeout()
    if not callonce then
        callonce = true
    end
    testrpc()
    testwebsock(true, true, 15003)
    testwebsock(false, true, 15004)
    testwebsock(true, false, 15003)
    testwebsock(false, false, 15004)
    testhttp()
    testudp()

    printd("....................... %.2f ........................", collectgarbage("count") / 1024)
    srey.timeout(3000, ontimeout)
end
local function onstarted()
    printd(srey.task_name() .. " onstarted....")
    srey.listen("0.0.0.0", 15000, PACK_TYPE.SIMPLE, nil, true)
    srey.timeout(3000, ontimeout)
    local harbor = srey.task_qury(TASK_NAME.HARBOR)
    srey.call(harbor, "addip", "192.168.100.1", "1")
    srey.call(harbor, "addip", "192.168.100.2", "2")
    srey.call(harbor, "addip", "192.168.100.3", "3")
    srey.call(harbor, "addip", "192.168.100.1", "1")
    srey.call(harbor, "addip", "192.168.100.4", "4")
    srey.call(harbor, "addip", "127.0.0.1", "10")
    srey.call(harbor, "removeip", "192.168.100.5")
    srey.call(harbor, "removeip", "192.168.100.2")
    local ssl = srey.evssl_qury("client")
    rpcfd, rpcfdid = srey.connect("127.0.0.1", 8080, PACK_TYPE.RPC, ssl)
    if INVALID_SOCK ~= rpcfd then
        printd("rpc connect.... fd: %d id: %d", rpcfd, rpcfdid)
    else
        printd("rpc connect.... error")
    end
end
srey.started(onstarted)

local function onaccept(pktype, fd, skid)
    --printd(srey.name() .. " onaccept.... " .. fd)
end
srey.accepted(onaccept)
local autoclose = true
local function echo(pktype, fd, skid, data, size)
    if PACK_TYPE.SIMPLE == pktype then
        if autoclose and math.random(1, 100) <= 1 then
            srey.close(fd, skid)
            return
        end
        local pack, lens = simple.unpack(data)
        srey.send(fd, skid, pktype, pack, lens)
    elseif PACK_TYPE.WEBSOCK == pktype then
        local proto, fin, pack, plens = websock.unpack(data)
        if WEBSOCK_PROTO.PING == proto then
            --printd("PING")
        elseif WEBSOCK_PROTO.CLOSE == proto then
            --printd("CLOSE")
        elseif WEBSOCK_PROTO.TEXT == proto  then
            --printd("TEXT size: %d", plens)
        elseif WEBSOCK_PROTO.BINARY == proto  then
            --printd("BINARY size: %d", plens)
        elseif WEBSOCK_PROTO.CONTINUE == proto then
            --printd("CONTINUE fin: %d size: %d", fin, plens)
        end
    end
end
srey.recved(echo)

local function onsockclose(pktype, fd, skid)
    if pktype == PACK_TYPE.RPC and rpcfd == fd then
        printd("rpc connect closed")
        rpcfd = INVALID_SOCK
    end
    if pktype == PACK_TYPE.WEBSOCK then
        --printd("websocket connect closed")
    end
end
srey.closed(onsockclose)
