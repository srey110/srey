local srey = require("lib.srey")
local simple = require("srey.simple")

srey.startup(
    function ()
        srey.on_accepted(
            function (pktype, fd, skid)
                --printd("accept socket %d, skid %s", fd, tostring(skid))
            end
        )
        srey.on_recved(
            function (pktype, fd, skid, client, sess, slice, data, size)
                local rdata, rsize = simple.unpack(data)
                --printd("socket %d, skid %s recv %s", fd, tostring(skid), srey.ud_str(rdata, rsize))
                local sdata, ssize = simple.pack(rdata, rsize)
                srey.send(fd, skid, sdata, ssize, 0)
            end
        )
        srey.on_sended(
            function (pktype, fd, skid, client, sess, size)
                --printd("socket %d, skid %s sended %d", fd, tostring(skid), size)
            end
        )
        srey.on_closed(
            function (pktype, fd, skid, sess)
                --printd("socket %d, skid %s closed", fd, tostring(skid))
            end
        )
        srey.listen(PACK_TYPE.SIMPLE, 0, "0.0.0.0", 15000, APPEND_EV.ACCEPT | APPEND_EV.CLOSE)-- | APPEND_EV.SEND)
    end
)
