local srey = require("lib.srey")

local function _timeout(n)
    local bg = os.time()
    srey.sleep(1000)
    assert(os.time() - bg == 1)
    srey.timeout(1000, _timeout, n + 1)
end
srey.startup(
    function ()
        srey.timeout(1000, _timeout, 1)
    end
)
