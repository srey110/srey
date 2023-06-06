local srey = require("lib.srey")
local http = require("lib.http")
local websock = require("lib.websock")

local function onstarted()
    printd(srey.name() .. " onstarted....")
    srey.udp("0.0.0.0", 15002)
    local ssl = srey.sslevqury("server")
    srey.listen("0.0.0.0", 15003, PACK_TYPE.HTTP)
end
srey.started(onstarted)

local function onrecvfrom(pktype, fd, data, ip, port)
    srey.sendto(fd, ip, port, data)
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
local function onrecv(pktype, fd, data)
    if PACK_TYPE.HTTP == pktype then
        local chunked = data.chunked
        if 1 == chunked then
            --printd(dump(data.head))
        elseif 2 == chunked  then
            if 0 == #data.data then
                --printd("chunked %d: end", fd)
                http_rtn(fd, true)
            else
                --printd("chunked %d lens: %d", fd, #data.data)
            end
        else
            local sign = websock.upgrade(data)
            if sign then
                local task4 = srey.qury(TASK_NAME.TASK4)
                srey.bindtask(fd, task4)
                websock.allowed(fd, sign)
                srey.call(task4, "handshaked", fd)
            else
                http_rtn(fd, false)
            end
        end
    end
end
srey.recved(onrecv)

local function onclosing()
    printd(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
