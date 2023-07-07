local srey = require("lib.srey")
local http = require("lib.http")
local websock = require("lib.websock")

local function onstarted()
    printd(srey.task_name() .. " onstarted...dns_lookup:")
    --8.8.8.8 114.114.114.114 208.67.222.222
    local ips = srey.dns_lookup("8.8.8.8", "www.google.com", false)
    if ips then
        printd("domain ipv4 ips:" .. dump(ips))
    end
    ips = srey.dns_lookup("8.8.8.8", "www.google.com", true)
    if ips then
        printd("domain ipv6 ips:" .. dump(ips))
    end
    ips = srey.dns_lookup("114.114.114.114", "www.baidu.com", false)
    if ips then
        printd("domain ips:" .. dump(ips))
    end
    ips = srey.dns_lookup("208.67.222.222", "www.baidu.com", true)
    if ips then
        printd("domain ips:" .. dump(ips))
    end
    srey.udp("0.0.0.0", 15002)
    local ssl = srey.evssl_qury(SSL_NAME.SERVER)
    srey.listen("0.0.0.0", 15003, PACK_TYPE.HTTP)
end
srey.started(onstarted)

local function onrecvfrom(fd, skid, data, size, ip, port)
    --printd(srey.utostr(data, size))
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
