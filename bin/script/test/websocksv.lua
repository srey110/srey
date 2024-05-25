local srey = require("lib.srey")
local websock = require("lib.websock")

srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, slice, data, size)
                local pack = websock.unpack(data)
                if WEBSOCK_PROTO.PING == pack.proto then
                    data, size = websock.pong(client)
                    srey.send(fd, skid, data, size, 0)
                elseif WEBSOCK_PROTO.CLOSE == pack.proto then
                    data, size = websock.close(client)
                    srey.send(fd, skid, data, size, 0)
                elseif WEBSOCK_PROTO.TEXT == pack.proto then
                    data, size = websock.text_fin(client, pack.fin, pack.data, pack.size)
                    srey.send(fd, skid, data, size, 0)
                elseif WEBSOCK_PROTO.BINARY == pack.proto then
                    data, size = websock.binary_fin(client, pack.fin, pack.data, pack.size)
                    srey.send(fd, skid, data, size, 0)
                elseif WEBSOCK_PROTO.CONTINUA == pack.proto then
                    data, size =websock.continua(client, pack.fin, pack.data, pack.size)
                    srey.send(fd, skid, data, size, 0)
                end
            end
        )
        srey.listen(PACK_TYPE.WEBSOCK, 0, "0.0.0.0", 15004)
    end
)
