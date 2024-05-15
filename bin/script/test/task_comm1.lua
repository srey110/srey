local srey = require("lib.srey")

srey.startup(
    function ()
        srey.call(TASK_NAME.comm2, 0, "this is comm1 call.")
        local data, size = srey.request(TASK_NAME.comm2, 0, "this is comm1 request.")
        assert(data)
        assert("this is comm2 response." == srey.ud_str(data, size))
    end
)
