require("lib.funcs")
local srey = require("lib.srey")
local syn = require("lib.synsl")
local simple = require("lib.simple")
local cbs = require("lib.cbs")
local dns = require("lib.dns")
local wbsk = require("lib.websock")
local http = require("lib.http")

local function timeout()
    printd("test2 release")
    srey.task_release()
end
local function test_synsend()
    if srey.task_closing() then
        return
    end
    local fd, skid = syn.connect("127.0.0.1", 15000, PACK_TYPE.SIMPLE)
    if INVALID_SOCK == fd then
        printd("synconnect error.")
    else
        local sess = srey.getid()
        local req = "this is synsend test."
        local spack, slens = simple.pack(req, #req)
        local rdata, _ = syn.send(fd, skid, sess, spack, slens)
        if rdata then
            if srey.utostr(simple.unpack(rdata)) ~= req then
                printd("synsend error.")
            end
        else
            printd("synsend error.")
        end
        srey.close(fd, skid)
    end
end
local function test_synsendto()
    if srey.task_closing() then
        return
    end
    local fd, skid = srey.udp("0.0.0.0", 0)
    local req = "this is synsendto test."
    local udata, size = syn.sendto(fd, skid, "127.0.0.1", 15002, req)
    if udata then
        if srey.utostr(udata, size) ~= req then
            printd("synsendto error.")
        end
    else
        printd("synsendto error.")
    end
end
local function test_request()
    if srey.task_closing() then
        return
    end
    local test1<close> = srey.task_grab(TASK_NAME.TEST1)
    if not test1 then
        printd("not find test1.")
        return
    end
    local call = "this is test2 call."
    srey.task_call(test1, call, #call, 1)
    local req = "this is test2 synrequest."
    local ok, data, size = syn.task_request(test1, req, #req, 1)
    if not ok  then
        printd("task_synrequest error")
    else
        if srey.utostr(data, size) ~= req then
            printd("task_synrequest error")
        end
    end
end
local function test_dns()
    if srey.task_closing() then
        return
    end
    local ips = dns.lookup("8.8.8.8", "www.google.com", false)
    if 0 == #ips then
        printd("dns.lookup error")
    end
    if srey.task_closing() then
        return
    end
    ips = dns.lookup("8.8.8.8", "www.google.com", true)
    if 0 == #ips then
        printd("dns.lookup error")
    end
end
local function test_websk()
    if srey.task_closing() then
        return
    end
    local fd, _ = wbsk.connect("124.222.224.186", 8800)
    if INVALID_SOCK == fd then
        printd("wbsk.connect error")
    end
end
local function chunked(cnt)
    cnt.n = cnt.n + 1
    if cnt.n <=3 then
        return string.format(" %s %d", os.date(FMT_TIME, os.time()), cnt.n)
    else
        return nil
    end
end
local function onchuncked(fd, skid, pack, hdata, hsize, fin)
    --printd("size %d, fin %s", hsize, tostring(fin))
end
local function test_http()
    if srey.task_closing() then
        return
    end
    local fd, skid = syn.connect("127.0.0.1", 15004, PACK_TYPE.HTTP)
    if INVALID_SOCK == fd then
        printd("http connect error")
        return
    end
    local hrtn = http.get(fd, skid, "/gettest")
    if not hrtn then
        printd("http.get /gettest error")
    end
    if srey.task_closing() then
        return
    end
    hrtn = http.post(fd, skid, "/getpost", nil, nil, "http post test.")
    if not hrtn then
        printd("http.post /getpost error")
    end
    if srey.task_closing() then
        return
    end
    local cnt = {n = 0}
    hrtn = http.post(fd, skid, "/getchuncked", nil, onchuncked, chunked, cnt)
    if not hrtn then
        printd("http.post /getchuncked error")
    end
    srey.close(fd, skid)
end
local function startup()
    test_synsend()
    test_synsendto()
    test_request()
    test_dns()
    test_websk()
    test_http()
    syn.timeout(5000, timeout)
end
cbs.cb_startup(startup)

local function recv(msg)
    --printd("pack type %d, size %d", msg.pktype, msg.size)
    srey.close(msg.fd, msg.skid)
end
cbs.cb_recv(recv)
