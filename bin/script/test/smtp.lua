local srey = require("lib.srey")
local smtp = require("lib.smtp")
require("lib.dns")
local _user = "test@163.com"
local _psw = "FCAZMcsYms"
local _from = "test@163.com"
local _to = "test@qq.com"

srey.startup(
    function ()
        local ips = nslookup("smtp.163.com", false)
        if not ips or 0 == #ips then
            printd("nslookup error.")
            return
        end
        local smtpctx = smtp.new(ips[1], 465, SSL_NAME.CLIENT, _user, _psw)
        if not smtpctx:connect() then
            printd("smtp connect error")
            return
        end
        if not smtpctx:send(_from, _to, "lua subject 11", "lua 12222") then
             printd("smtp reset error")
            return
        end
        smtpctx:ping()
        smtpctx:quit()
        smtpctx:ping()
        if not smtpctx:send(_from, _to, "lua subject 22", "lua 3333") then
             printd("smtp reset error")
            return
        end
        smtpctx:quit()
        printd("smtp tested.")
    end
)
