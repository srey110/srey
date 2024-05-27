local srey = require("lib.srey")
local custz = require("srey.custz")

srey.startup(
    function ()
        srey.on_accepted(
            function (pktype, fd, skid)
                --printd("accept socket %d, skid %s", fd, tostring(skid))
            end
        )
        srey.on_recved(
            function (pktype, fd, skid, client, slice, data, size)
                local rdata, rsize = custz.unpack(data)
                --printd("socket %d, skid %s recv %s", fd, tostring(skid), srey.ud_str(rdata, rsize))
                local sdata, ssize = custz.pack(rdata, rsize)
                srey.send(fd, skid, sdata, ssize, 0)
            end
        )
        srey.on_sended(
            function (pktype, fd, skid, client, size)
                --printd("socket %d, skid %s sended %d", fd, tostring(skid), size)
            end
        )
        srey.on_closed(
            function (pktype, fd, skid, client)
                --printd("socket %d, skid %s closed", fd, tostring(skid))
            end
        )
        srey.listen(PACK_TYPE.CUSTZ, 0, "0.0.0.0", 15000, NET_EV.ACCEPT)-- | APPEND_EV.SEND)
    end
)
