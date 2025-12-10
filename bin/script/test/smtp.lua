local srey = require("lib.srey")
local smtp = require("lib.smtp")
local mail = require("lib.mail")
require("lib.dns")
local _user = "test@163.com"
local _psw = "FCAZMcsYms"

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
        local mailctx = mail.new()
        mailctx:from("srey", "test@163.com")
        mailctx:addrs_add("test@qq.com", MAIL_ADDR_TYPE.TO)
        mailctx:addrs_add("test@gmail.com", MAIL_ADDR_TYPE.TO)
        mailctx:subject("subject_lua")
        mailctx:msg("this message from lua.")
        if not smtpctx:send(mailctx) then
            printd("smtp send main error")
            return
        end
        mailctx:html("<!DOCTYPE html><html><title>HTML Tutorial</title><body><h1>This is a heading,LUA</h1><p>This is a paragraph.</p></body></html>")
        mailctx:attach_add("D:\\....\\panda.jpg")
        if not smtpctx:send(mailctx) then
            printd("smtp send main error")
            return
        end
        smtpctx:quit()
        printd("smtp tested.")
    end
)
