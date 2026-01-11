local srey = require("lib.srey")
local prtone = false

local function _timeout(n)
    local bg = os.time()
    srey.sleep(1000)
    assert(os.time() - bg == 1)
    if not prtone then
        printd("timeout tested.")
        prtone = true
    end
    srey.timeout(1000, _timeout, n + 1)
end
srey.startup(
    function ()
        srey.timeout(1000, _timeout, 1)
    end
)
