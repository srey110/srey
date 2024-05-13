local srey = require("lib.srey")

srey.startup(
    function ()
        srey.on_recvedfrom(
            function (fd, skid, sess, ip, port, data, size)
                --printd("%s:%d, send message %s", ip, port, srey.ud_str(data, size))
                srey.sendto(fd, skid, ip, port, data, size)
            end
        )
        srey.udp("0.0.0.0", 15003)
    end
)
