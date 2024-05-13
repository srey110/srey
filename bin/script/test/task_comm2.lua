local srey = require("lib.srey")

srey.startup(
    function ()
        srey.on_requested(
            function (reqtype, sess, src, data, size)
                if INVALID_TNAME == src then
                    local tmp = srey.ud_str(data, size)
                    assert("this is comm1 call." == tmp or "this is harbor call." == tmp)
                elseif TASK_NAME.comm1 == src then
                    local tmp = srey.ud_str(data, size)
                    assert("this is comm1 request." == tmp)
                    srey.response(src, sess, ERR_OK, "this is comm2 response.")
                elseif TASK_NAME.HARBOR == src  then
                    local tmp = srey.ud_str(data, size)
                    assert("this is harbor request." == tmp)
                    srey.response(src, sess, ERR_OK, "this is comm2 harbor response.")
                end
            end
        )
    end
)