local srey = require("lib.srey")
local custz = require("srey.custz")

srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, slice, data, size)
                local rdata, rsize = custz.unpack(data)
                local sdata, ssize = custz.pack(rdata, rsize)
                srey.send(fd, skid, sdata, ssize, 0)
            end
        )
        srey.listen(PACK_TYPE.CUSTZ, SSL_NAME.SERVER, "0.0.0.0", 15001)
    end
)
