local srey = require("lib.srey")
local custz = require("srey.custz")
local pktype = PACK_TYPE.CUSTZ_FLAG
srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, slice, data, size)
                local sdata, ssize = custz.pack(pktype, data, size)
                srey.send(fd, skid, sdata, ssize, 0)
            end
        )
        srey.listen(pktype, SSL_NAME.SERVER, "0.0.0.0", 15001)
    end
)
