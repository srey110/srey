-- MIME 邮件构造器（smtp_mail_ctx 类）。
-- 封装 C 层 srey.smtp.mail，提供面向对象的邮件组装接口：
-- 主题、正文（纯文本/HTML）、发件人、收件人（TO/CC/BCC）、附件，
-- 最终通过 pack() 序列化为 SMTP DATA 阶段所需的 MIME 字节串。
-- 使用方：创建实例后填充各字段，再将实例传给 smtp_ctx:send()。

require("lib.utils")
local mail = require("srey.smtp.mail")

-- 收件人类型枚举
---@enum MAIL_ADDR_TYPE
MAIL_ADDR_TYPE = {
    TO  = 0x01,   -- 主要收件人
    CC  = 0x02,   -- 抄送
    BCC = 0x03    -- 密送（不在邮件头部显示）
}

-- smtp_mail_ctx：单封邮件的构造上下文。
-- self.sender：发件人邮箱地址（供 SMTP MAIL FROM 命令使用）。
-- self.addrs ：收件人邮箱列表（供 SMTP RCPT TO 命令使用）。
-- self.mail  ：C 层邮件对象，负责 MIME 格式化。
local ctx = class("smtp_mail_ctx")
function ctx:ctor()
    self.sender = ""
    self.addrs  = {}
    self.mail   = mail.new()
end

---设置邮件主题
---@param subject string 主题
function ctx:subject(subject)
    self.mail:subject(subject)
end

---设置纯文本正文
---@param msg string 正文内容
function ctx:msg(msg)
    self.mail:msg(msg)

end

---设置 HTML 正文；与纯文本正文同时存在时构成 multipart/alternative
---@param html string HTML 内容
function ctx:html(html)
    self.mail:html(html)
end

---设置发件人；email 同时缓存到 self.sender 供 SMTP MAIL FROM 使用
---@param name string 显示名称
---@param email string 邮箱地址
function ctx:from(name, email)
    self.sender = email
    self.mail:from(name, email)
end

---获取发件人邮箱地址
---@return string email 发件人邮箱
function ctx:from_get()
    return self.sender
end

---添加一个收件人；email 同时追加到 self.addrs 供上层 SMTP RCPT TO 遍历
---@param email string 收件人邮箱
---@param type MAIL_ADDR_TYPE 收件人类型
function ctx:addrs_add(email, type)
    table.insert(self.addrs, email)
    self.mail:addrs_add(email, type)
end

---获取收件人邮箱列表（用于 SMTP RCPT TO 命令遍历）
---@return string[] addrs 收件人邮箱数组
function ctx:addrs_get()
    return self.addrs
end

---清空所有收件人
function ctx:addrs_clear()
    self.addrs = {}
    self.mail:addrs_clear()
end

---添加附件
---@param file string 本地文件路径
function ctx:attach_add(file)
    self.mail:attach_add(file)
end

---清空所有附件
function ctx:attach_clear()
    self.mail:attach_clear()
end

---设置是否请求邮件回执（DSN）
---@param reply integer? nil 或 0 不请求；非 0 请求回执
function ctx:reply(reply)
    self.mail:reply(reply)
end

---重置邮件内容（主题、正文、收件人、附件均清空）
function ctx:clear()
    self.addrs = {}
    self.mail:clear()
end

---将邮件序列化为 SMTP DATA 正文字节串
---@return lightuserdata data MIME 字节串指针
---@return integer size 字节数
function ctx:pack()
    return self.mail:pack()
end

return ctx
