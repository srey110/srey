local srey = require("lib.srey")
local http = require("lib.http")

local function _chunked(cnt)
    cnt.n = cnt.n + 1
    if cnt.n <=3 then
        return string.format(" %s %d", os.date("%Y-%m-%d %H:%M:%S", os.time()), cnt.n)
    else
        return nil
    end
end
local function _onchuncked(fin, hdata, hsize)
end
local function _timeout()
    local fd, skid = srey.connect(PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", 15003)
    assert(INVALID_SOCK ~= fd)
    local hrtn = http.get(fd, skid, "/gettest")
    assert(hrtn)
    hrtn = http.post(fd, skid, "/getpost", nil, nil, "http post test.")
    assert(hrtn)
    local cnt = {n = 0}
    hrtn = http.post(fd, skid, "/getchuncked", nil, _onchuncked, _chunked, cnt)
    assert(hrtn)
    srey.close(fd, skid)
    printd("httpd tested.")
end
srey.startup(
    function ()
        srey.timeout(1000, _timeout)
    end
)
