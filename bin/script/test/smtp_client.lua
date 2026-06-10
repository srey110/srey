-- SMTP 客户端测试（仿照 C 层 test/task_smtp.c）。
-- 注册参数（与 C 一致）：
--   task.register("test.smtp_client", "smtp_client", 0,
--                 sslname, smtp_sv, smtp_port, smtp_user, smtp_psw,
--                 mail_from, mail_addr1, mail_addr2, mail_attach)
--
-- 行为：连接 -> 发第一封纯文本邮件 -> 发第二封 text+HTML+附件邮件 -> QUIT。
-- 默认在 startup.lua 中**不启用**（用户名/密码/邮箱地址需用户填实，再去掉注释）。

local srey   = require("lib.srey")
local runner = require("test.runner")
local smtp   = require("lib.smtp")
local mail   = require("lib.mail")
require("lib.dns")

local _sslname, _smtp_sv, _smtp_port,
      _smtp_user, _smtp_psw,
      _mail_from, _mail_addr1, _mail_addr2, _mail_attach = ...

srey.startup(function()
runner.run("smtp_client", function(t)
    -- 域名需先 DNS 解析为 IP，IP 直连
    local ip = _smtp_sv
    if "hostname" == host_type(_smtp_sv) then
        local ips = nslookup(_smtp_sv, false)
        if not ips or 0 == #ips then
            t:fail("dns_lookup " .. tostring(_smtp_sv))
            return
        end
        ip = ips[1]
    end
    local smtpctx = smtp.new(ip, _smtp_port, _sslname, _smtp_user, _smtp_psw)
    if not smtpctx:connect() then
        t:fail("smtp connect")
        return
    end
    t:check(true, "smtp connected")

    -- 第一封：纯文本，TO 收件人，无附件，无 Reply-To
    local mailctx = mail.new()
    mailctx:from("srey", _mail_from)
    if _mail_addr1 and _mail_addr1 ~= "" then
        mailctx:addrs_add(_mail_addr1, MAIL_ADDR_TYPE.TO)
    end
    if _mail_addr2 and _mail_addr2 ~= "" then
        mailctx:addrs_add(_mail_addr2, MAIL_ADDR_TYPE.CC)
    end
    mailctx:subject("srey smtp test")
    mailctx:msg("this is text message")
    mailctx:reply(0)
    t:check(smtpctx:send(mailctx), "smtp send 1 (plain)")

    -- 第二封：text + HTML + 附件 + Reply-To 头
    mailctx:html("<!DOCTYPE html><html><title>HTML Tutorial</title><body>"
        .. "<h1>This is a heading</h1><p>This is a paragraph.</p></body></html>")
    if _mail_attach and _mail_attach ~= "" then
        mailctx:attach_add(_mail_attach)
    end
    mailctx:reply(1)
    t:check(smtpctx:send(mailctx), "smtp send 2 (html + attach)")

    smtpctx:quit()
end)
end)
