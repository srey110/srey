local srey = require("lib.srey")
local http = require("lib.http")
local websock = require("lib.websock")

local function onstarted()
    printd(srey.task_name() .. " onstarted....")
    srey.udp("0.0.0.0", 15002)
    local ssl = srey.evssl_qury("server")
    srey.listen("0.0.0.0", 15003, PACK_TYPE.HTTP)
end
srey.started(onstarted)

local function onrecvfrom(fd, skid, data, size, ip, port)
    srey.sendto(fd, skid, ip, port, data, size)
end
srey.recvfromed(onrecvfrom)
local function chunckeddata(cnt)
    if cnt.cnt >= 3 then
        return nil
    end
    cnt.cnt = cnt.cnt + 1
    return "this is chunked return "..tostring(cnt.cnt)
end
local function http_rtn(fd, skid, chunked)
    local cnt = {
        cnt = 0
    }
    if chunked then
        http.response(fd, skid, 200, nil, chunckeddata, cnt)
    else
        http.response(fd, skid, 200, nil, "OK")
    end
end
local function onrecv(pktype, fd, skid, pack, size)
    if PACK_TYPE.HTTP == pktype then
        local chunked = http.chunked(pack)
        local data, lens = http.data(pack)
        if 1 == chunked then
        elseif 2 == chunked  then
            if not data then
                --printd("chunked %d: end", fd)
                http_rtn(fd, skid, true)
            else
                --printd("chunked %d lens: %d", fd, lens)
            end
        else
            local sign = websock.upgrade(pack)
            if sign then
                local task4 = srey.task_qury(TASK_NAME.TASK4)
                srey.ud_data(fd, skid, task4)
                websock.allowed(fd, skid, sign)
                srey.call(task4, "handshaked", fd, skid)
            else
                http_rtn(fd, skid, false)
            end
        end
    end
end
srey.recved(onrecv)
