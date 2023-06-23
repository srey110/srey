local srey = require("lib.srey")
local simple = require("lib.simple")
local http = require("lib.http")
local websock = require("lib.websock")
local math = math
local rpcfd = INVALID_SOCK
local rpcfdid = 0
local callonce = false

local function testrpc()
    local rpctask = srey.task_qury(TASK_NAME.TASK2)
    srey.call(rpctask, "rpc_add", math.random(10) , math.random(10), "srey.call")
    local rtn, sum, des = srey.request(rpctask, "rpc_add", math.random(10) , math.random(10), "srey.request")
    --printd("sum:"..tostring(sum).. " des:".. tostring(des))
    if not rtn then
        printd("srey.request failed")
    end
    rtn = srey.request(rpctask, "rpc_void")
    if not rtn then
        printd("srey.request failed")
    end
    if INVALID_SOCK ~= rpcfd then
        srey.netcall(rpcfd, rpcfdid, TASK_NAME.TASK2, "rpc_add", math.random(10) , math.random(10), "srey.netcall")
        rtn, sum, des = srey.netreq(rpcfd, rpcfdid, TASK_NAME.TASK2, "rpc_add", math.random(10) , math.random(10), "srey.netreq")
        --printd("sum:"..tostring(sum).. " des:".. tostring(des))
        if not rtn then
            printd("srey.netreq failed")
        end
        rtn = srey.netreq(rpcfd, rpcfdid, TASK_NAME.TASK2, "rpc_void")
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
local function testwebsock(chs, port)
    local ssl = srey.evssl_qury("client")
    local websockfd
    local skid
    if chs then
        websockfd, skid = websock.connect("127.0.0.1", port)
        if INVALID_SOCK == websockfd then
            printd("websock connect.... error")
            return
        end
    else
        websockfd, skid = srey.connect("127.0.0.1", port, PACK_TYPE.HTTP)
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
    websock.continuation(websockfd, skid, randstr(4), continuationcb, cnt)
    websock.close(websockfd, skid)
end
local function chuncedmesg(cnt)
    if cnt.cnt >= 3 then
        return nil
    end
    cnt.cnt = cnt.cnt + 1
    return "this is chuncked "..tostring(cnt.cnt)
end
local function onchunked(fd, skid, data, size)
    if data then
        --printd("hunked: %d", size)
    else
        --printd("hunked: end")
        srey.close(fd, skid)
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
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, skid, "/post_nomsg?a=中文测试2", headers)
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, skid, "/post_string", headers, nil, "this is string message")
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, skid, "/post_json", headers, nil, jmsg)
    if not hinfo then
        printd("http error.")
    end
    hinfo = http.post(fd, skid, "/get_chunked_cb", headers, onchunked, chuncedmesg, cnt)
    if not hinfo then
        printd("http error.")
    end
end
local function ontimeout()
    --printd(".............................................")
    if not callonce then
        callonce = true
    end
    testrpc()
    testwebsock(true, 15003)
    testwebsock(true, 15004)
    testwebsock(false, 15003)
    testwebsock(false, 15004)
    testhttp()
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
        printd("rpc end connect.... fd: %d id: %d", rpcfd, rpcfdid)
    else
        printd("rpc end connect.... error")
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
