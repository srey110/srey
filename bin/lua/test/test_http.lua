require("lib.funcs")
local srey = require("lib.srey")
local cbs = require("lib.cbs")
local http = require("lib.http")
local cksize
local files = {}

local function startup()
    local ssl = srey.evssl_qury(SSL_NAME.SERVER)
    srey.listen("0.0.0.0", 15004, PACK_TYPE.HTTP)
end
cbs.cb_startup(startup)

local function chunked(cnt)
    cnt.n = cnt.n + 1
    if cnt.n <=3 then
        return string.format(" %s %d", os.date(FMT_TIME, os.time()), cnt.n)
    else
        return nil
    end
end
local function recv(msg)
    local pack = http.unpack(msg.data)
    local head = {
        Server = "Srey"
    }
    if 0 == pack.chunked then
        local sign = http.upgrade_websock(msg.data)
        if sign then
            srey.ud_name(msg.fd, msg.skid, TASK_NAME.TEST_WBSK)
            http.upgrade_websock_allowed(msg.fd, msg.skid, sign)
            return
        end
        local rd = math.random(0, 2)
        if 0 == rd then
            http.response(msg.fd, msg.skid, 200, head, os.date(FMT_TIME, os.time()))
        elseif 1 == rd then
            local jmsg = {
                time = os.date(FMT_TIME, os.time()),
                data = pack.data
            }
            http.response(msg.fd, msg.skid, 200, head, jmsg)
        elseif 2 == rd then
            http.response(msg.fd, msg.skid, 200, head)
        end
    elseif 1 == pack.chunked then
        --printd("chunked start")
        cksize = 0
        --local filename = string.format("%s%s%d %s", _propath, _pathsep, msg.skid, os.date("%H-%M-%S", os.time()))
        --local file = io.open(filename, "wb")
        --files[msg.skid] = file
    elseif 2 == pack.chunked then
        if pack.data then
            cksize = cksize + #pack.data
            --files[msg.skid]:write(pack.data)
        else
            --files[msg.skid]:close()
            --files[msg.skid] = nil
            --printd("chunked end  size %d", cksize)
            local cnt = {n = 0}
            http.response(msg.fd, msg.skid, 200, head, chunked, cnt)
        end
    end
end
cbs.cb_recv(recv)
