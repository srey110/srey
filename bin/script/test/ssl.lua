local srey = require("lib.srey")
local custz = require("srey.custz")

srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, slice, data, size)
                local sdata, ssize = custz.pack(data, size)
                srey.send(fd, skid, sdata, ssize, 0)
            end
        )
        srey.listen(PACK_TYPE.CUSTZ, SSL_NAME.SERVER, "0.0.0.0", 15001)
    end
)
