require("lib.utils")
local mail = require("srey.smtp.mail")

 MAIL_ADDR_TYPE = {
    TO = 0x01,
    CC = 0x02,
    BCC = 0x03
}

local ctx = class("smtp_mail_ctx")
function ctx:ctor()
    self.sender = ""
    self.addrs = {}
    self.mail = mail.new()
end
function ctx:subject(subject)
    self.mail:subject(subject)
end
function ctx:msg(msg)
    self.mail:msg(msg)
end
function ctx:html(html)
    self.mail:html(html)
end
function ctx:from(name, email)
    self.sender = email
    self.mail:from(name, email)
end
function ctx:from_get()
    return self.sender
end
function ctx:addrs_add(email, type)
    table.insert(self.addrs, email)
    self.mail:addrs_add(email, type)
end
function ctx:addrs_get()
    return self.addrs
end
function ctx:addrs_clear()
    self.addrs = {}
    self.mail:addrs_clear()
end
function ctx:attach_add(file)
    self.mail:attach_add(file)
end
function ctx:attach_clear()
    self.mail:attach_clear()
end
function ctx:clear()
    self.addrs = {}
    self.mail:clear()
end
function ctx:pack()
    return self.mail:pack()
end

return ctx
