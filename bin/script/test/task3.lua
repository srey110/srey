local srey = require("lib.srey")
local http = require("lib.http")

local function onstarted()
    printd(srey.name() .. " onstarted....")
    srey.udp("0.0.0.0", 15002)
    local ssl = srey.sslevqury("server")
    srey.listen("0.0.0.0", 15003, PACK_TYPE.HTTP, ssl)
end
srey.started(onstarted)

local function onrecvfrom(unptype, fd, data, size, ip, port)
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
local function onrecv(unptype, fd, data, size)
    if PACK_TYPE.HTTP == unptype then
        local chunked = srey.http_chunked(data)
        if 1 == chunked then
            local hinfo = srey.http_head(data)
            --printd(dump(hinfo))
        elseif 2 == chunked  then
            local msg, lens = srey.http_data(data)
            if nil == msg then
                printd("chunked %d: end", fd)
                http_rtn(fd, true)
            else
                printd("chunked %d lens: %d", fd, lens)
            end
        else
            local hinfo = srey.http_head(data)
            local msg, lens = srey.http_data(data)
            if nil ~= msg then
                --printd("message lens: %d", lens)
            end
            http_rtn(fd, false)
        end
    end
end
srey.recved(onrecv)

local function onclosing()
    printd(srey.name() .. " onclosing....")
end
srey.closing(onclosing)
