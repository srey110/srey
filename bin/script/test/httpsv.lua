local srey = require("lib.srey")
local http = require("lib.http")

local function _chunked(cnt)
    cnt.n = cnt.n + 1
    if cnt.n <=3 then
        return string.format(" %s %d", os.date("%Y-%m-%d %H:%M:%S", os.time()), cnt.n)
    else
        return nil
    end
end
srey.startup(
    function ()
        local head = {
            Server = "Srey"
        }
        srey.on_recved(
            function (pktype, fd, skid, client, sess, slice, data, size)
                local chunked = http.chunked(data)
                if 0 == chunked then
                    --升级 websock  http://www.websocket-test.com/
                    local sign = http.websock_upgrade(data)
                    if sign then
                        http.websock_allowed(fd, skid, sign, TASK_NAME.wbsksv)
                        return
                    end

                    local rd = math.random(0, 2)
                    if 0 == rd then
                        http.response(fd, skid, 200, head, os.date("%Y-%m-%d %H:%M:%S", os.time()))
                    elseif 1 == rd then
                        local jmsg = {
                            time = os.date("%Y-%m-%d %H:%M:%S", os.time())
                        }
                        http.response(fd, skid, 200, head, jmsg)
                    elseif 2 == rd then
                        http.response(fd, skid, 200, head, http.code_status(200))
                    end
                elseif 1 == chunked then
                    --printd("chunked start")
                elseif 2 == chunked then
                    local ckdata = http.datastr(data)
                    if ckdata then
                        --printd("chunked data %s", ckdata)
                    else
                        --printd("chunked end")
                        local cnt = {n = 0}
                        http.response(fd, skid, 200, head, _chunked, cnt)
                    end
                end
            end
        )
        srey.listen(PACK_TYPE.HTTP, 0, "0.0.0.0", 15005)
    end
)
