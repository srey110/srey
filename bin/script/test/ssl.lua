local srey = require("lib.srey")
local simple = require("srey.simple")

srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, sess, slice, data, size)
                local rdata, rsize = simple.unpack(data)
                local sdata, ssize = simple.pack(rdata, rsize)
                srey.send(fd, skid, sdata, ssize, 0)
            end
        )
        srey.listen(PACK_TYPE.SIMPLE, SSL_NAME.SERVER, "0.0.0.0", 15001)
    end
)
