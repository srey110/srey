local srey = require("lib.srey")
local hbkey = "p1KaLmIg2kB7fZp5Tm1qbi6dss92C31aPGJK9Lvk7DMPDONXrwD2yVLZrHBio74s"

local function _timeout()
    local fd, skid = srey.connect(PACK_TYPE.HTTP, "127.0.0.1", 8080)
    assert(INVALID_SOCK ~= fd)
    local reqdata = "this is harbor call."
    local resp = srey.net_call(fd, skid, TASK_NAME.comm2, 0, hbkey, reqdata, #reqdata)
    assert(resp)
    reqdata = "this is harbor request."
    local respdata, respsize = srey.net_request(fd, skid, TASK_NAME.comm2, 0, hbkey, reqdata, #reqdata)
    assert(respdata)
    assert("this is comm2 harbor response." == srey.ud_str(respdata, respsize))
    srey.close(fd, skid)
    printd("harbor tested.")
end

srey.startup(
    function ()
        srey.timeout(1000, _timeout)
    end
)

