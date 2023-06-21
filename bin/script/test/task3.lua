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

local function onrecvfrom(fd, data, size, ip, port)
    srey.sendto(fd, ip, port, data, size)
end
srey.recvfromed(onrecvfrom)
local function chunckeddata(cnt)
    if cnt.cnt >= 3 then
        return nil
    end
    cnt.cnt = cnt.cnt + 1
    return "this is chunked return "..tostring(cnt.cnt)
end
local function http_rtn(fd, chunked)
    local cnt = {
        cnt = 0
    }
    if chunked then
        http.response(fd, 200, nil, chunckeddata, cnt)
    else
        http.response(fd, 200, nil, "OK")
    end
end
local function onrecv(pktype, fd, data, size)
    if PACK_TYPE.HTTP == pktype then
        local hpack = http.unpack(data, 1)
        local chunked = hpack.chunked
        if 1 == chunked then
            --printd(dump(hpack.head))
        elseif 2 == chunked  then
            if 0 == hpack.size then
                --printd("chunked %d: end", fd)
                http_rtn(fd, true)
            else
                --printd("chunked %d lens: %d", fd, hpack.size)
            end
        else
            local sign = websock.upgrade(hpack)
            if sign then
                local task4 = srey.task_qury(TASK_NAME.TASK4)
                srey.ud_data(fd, task4)
                websock.allowed(fd, sign)
                srey.call(task4, "handshaked", fd)
            else
                http_rtn(fd, false)
            end
        end
    end
end
srey.recved(onrecv)
