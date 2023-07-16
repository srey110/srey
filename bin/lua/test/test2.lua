require("lib.funcs")
local srey = require("lib.srey")
local syn = require("lib.synsl")
local simple = require("lib.simple")
local cbs = require("lib.cbs")
local dns = require("lib.dns")
local wbsk = require("lib.websock")

local function timeout()
    printd("test2 release")
    srey.task_release()
end
local function test_synsend()
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
    local ips = dns.lookup("8.8.8.8", "www.google.com", false)
    if 0 == #ips then
        printd("dns.lookup error")
    end
    ips = dns.lookup("8.8.8.8", "www.google.com", true)
    if 0 == #ips then
        printd("dns.lookup error")
    end
end
local function test_websk()
    local fd, skid = wbsk.connect("124.222.224.186", 8800)
    if INVALID_SOCK == fd then
        printd("wbsk.connect error")
    end
end
local function startup()
    test_synsend()
    test_synsendto()
    test_request()
    test_dns()
    test_websk()
    syn.timeout(5000, timeout)
end
cbs.cb_startup(startup)

local function recv(msg)
    printd("pack type %d, size %d", msg.pktype, msg.size)
    srey.close(msg.fd, msg.skid)
end
cbs.cb_recv(recv)
