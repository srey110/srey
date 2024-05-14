local srey = require("lib.srey")
local websock = require("lib.websock")

srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, sess, slice, data, size)
                local pack = websock.unpack(data)
                if WEBSOCK_PROTO.PING == pack.proto then
                    websock.pong(fd, skid, client)
                elseif WEBSOCK_PROTO.CLOSE == pack.proto then
                    websock.close(fd, skid, client)
                elseif WEBSOCK_PROTO.TEXT == pack.proto then
                    websock.text_fin(fd, skid, client, pack.fin, pack.data, pack.size)
                elseif WEBSOCK_PROTO.BINARY == pack.proto then
                    websock.binary_fin(fd, skid, client, pack.fin, pack.data, pack.size)
                elseif WEBSOCK_PROTO.CONTINUA == pack.proto then
                    websock.continua(fd, skid, client, pack.fin, pack.data, pack.size)
                end
            end
        )
        srey.listen(PACK_TYPE.WEBSOCK, 0, "0.0.0.0", 15004)
    end
)
