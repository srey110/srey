local srey = require("lib.srey")
local websock = require("lib.websock")

srey.startup(
    function ()
        srey.on_recved(
            function (pktype, fd, skid, client, slice, data, size)
                local pack = websock.unpack(data)
                if WEBSOCK_PROT.PING == pack.prot then
                    data, size = websock.pong(client)
                    srey.send(fd, skid, data, size, 0)
                elseif WEBSOCK_PROT.CLOSE == pack.prot then
                    data, size = websock.close(client)
                    srey.send(fd, skid, data, size, 0)
                elseif WEBSOCK_PROT.TEXT == pack.prot then
                    data, size = websock.text_fin(client, pack.fin, pack.data, pack.size)
                    srey.send(fd, skid, data, size, 0)
                elseif WEBSOCK_PROT.BINARY == pack.prot then
                    data, size = websock.binary_fin(client, pack.fin, pack.data, pack.size)
                    srey.send(fd, skid, data, size, 0)
                elseif WEBSOCK_PROT.CONTINUA == pack.prot then
                    data, size =websock.continua(client, pack.fin, pack.data, pack.size)
                    srey.send(fd, skid, data, size, 0)
                end
            end
        )
        srey.on_handshaked(
            function (pktype, fd, skid, client, erro, data, size)
                if 0 ~=  size then
                    printd("Sec-WebSocket-Protocol:" .. srey.ud_str(data, size));
                end
            end
        )
        srey.listen(PACK_TYPE.WEBSOCK, 0, "0.0.0.0", 15004)
    end
)
