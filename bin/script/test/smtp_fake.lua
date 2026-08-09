-- 假 SMTP 服务端 + 并发投递用例（Lua 侧，镜像 C 层 test/task_smtp.c 的同名部分）。
-- 真服务端要外网账号跑不通，smtp.lua 的命令串行化就一直只有静态核查。这里只实现刚好够
-- connect / send / quit 走完的那部分协议，专门用来抓命令交错。
-- 交错检测靠事务状态机：MAIL FROM 开事务并记下发件人编号，RCPT 的编号必须与之相同，
-- DATA 收到 "\r\n.\r\n" 收尾，RSET 关事务。两个协程的邮件挤到一起时必然撞上其中一条。
-- 服务端回调与投递协程同属一个 task，单线程依次执行，故下面这些状态用普通表即可。
-- 注册：task.register("test.smtp_fake", "smtp_fake", 0, port)

local srey   = require("lib.srey")
local runner = require("test.runner")
local smtp   = require("lib.smtp")
local mail   = require("lib.mail")

local _PORT = ... or 12526
local CONC_N = 4
local ROUNDS = 4
local TERM = "\r\n.\r\n"

local conns = {}        -- skid -> 连接状态
local interleave = 0    -- 检测到的交错次数
local mails = 0         -- 服务端确认收下的邮件数

local function _reply(fd, skid, resp)
    srey.send(fd, skid, resp, #resp, 1)
end
-- 从 "<c3@t>" 这类地址里取出编号；取不到返回 -1
local function _tag(line, prefix)
    local n = line:match("<" .. prefix .. "(%d+)@")
    return n and tonumber(n) or -1
end
-- 处理一行命令（已去掉 CRLF）
local function _cmd(fd, skid, fc, line)
    local up = line:upper()
    if up:find("^EHLO") or up:find("^HELO") then
        -- 只广告 LOGIN：客户端的 _smtp_get_authtype 优先 PLAIN，不给它选择余地
        _reply(fd, skid, "250-fake.smtp.local\r\n250-AUTH LOGIN\r\n250 OK\r\n")
    elseif up:find("^AUTH LOGIN") then
        fc.authstep = 1
        _reply(fd, skid, "334 VXNlcm5hbWU6\r\n")-- base64("Username:")
    elseif 1 == fc.authstep then
        fc.authstep = 2
        _reply(fd, skid, "334 UGFzc3dvcmQ6\r\n")-- base64("Password:")
    elseif 2 == fc.authstep then
        fc.authstep = 3
        _reply(fd, skid, "235 2.7.0 Authentication successful\r\n")
    elseif up:find("^MAIL FROM:") then
        local tag = _tag(line, "c")
        if -1 ~= fc.sender then-- 上一笔还没收尾就又来一个发件人：交错
            interleave = interleave + 1
            ERROR("fake smtp: MAIL FROM c%d while c%d still open.", tag, fc.sender)
        end
        fc.sender = tag
        _reply(fd, skid, "250 OK\r\n")
    elseif up:find("^RCPT TO:") then
        local tag = _tag(line, "r")
        if tag ~= fc.sender then-- 收件人编号与本事务发件人对不上：交错
            interleave = interleave + 1
            ERROR("fake smtp: RCPT r%d under sender c%d.", tag, fc.sender)
        end
        _reply(fd, skid, "250 OK\r\n")
    elseif up:find("^DATA") then
        fc.indata = true
        _reply(fd, skid, "354 End data with <CR><LF>.<CR><LF>\r\n")
    elseif up:find("^RSET") then
        fc.sender = -1
        _reply(fd, skid, "250 OK\r\n")
    elseif up:find("^QUIT") then
        _reply(fd, skid, "221 Bye\r\n")
    else
        _reply(fd, skid, "250 OK\r\n")-- NOOP 及其余一律 250
    end
end

srey.startup(function()
    srey.on_accepted(function(pktype, fd, skid)
        conns[skid] = { buf = "", authstep = 0, indata = false, sender = -1 }
        _reply(fd, skid, "220 fake.smtp.local ESMTP\r\n")
    end)
    srey.on_closed(function(pktype, fd, skid, client)
        conns[skid] = nil
    end)
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        local fc = conns[skid]
        if not fc or not data then
            return
        end
        fc.buf = fc.buf .. srey.ud_str(data, size)
        while true do
            if fc.indata then
                -- 信体只找结束标记，找不到就只留可能被截断的末尾几字节，不占着整封信
                local s, e = fc.buf:find(TERM, 1, true)
                if not s then
                    if #fc.buf > #TERM - 1 then
                        fc.buf = fc.buf:sub(-(#TERM - 1))
                    end
                    return
                end
                fc.buf = fc.buf:sub(e + 1)
                fc.indata = false
                fc.sender = -1-- 一封收完，事务结束
                mails = mails + 1
                _reply(fd, skid, "250 OK\r\n")
            else
                local s, e = fc.buf:find("\r\n", 1, true)
                if not s then
                    return
                end
                local line = fc.buf:sub(1, s - 1)
                fc.buf = fc.buf:sub(e + 1)
                _cmd(fd, skid, fc, line)
            end
        end
    end)
    if ERR_FAILED == srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", _PORT, NET_EV.ACCEPT) then
        ERROR("fake smtp: listen %d failed.", _PORT)
        return
    end

runner.run("smtp_fake", function(t)
    local ctx = smtp.new("127.0.0.1", _PORT, SSL_NAME.NONE, "user", "psw")
    if not ctx:connect() then
        t:fail("fake smtp connect")
        return
    end
    t:check(true, "fake smtp connected")

    -- 并发：每个协程发自己编号的邮件，服务端按事务状态机校验没有交错
    local done, got = 0, {}
    for i = 1, CONC_N do
        srey.fork(function()
            for _ = 1, ROUNDS do
                local m = mail.new()
                m:from("srey", string.format("c%d@t", i))
                m:addrs_add(string.format("r%d@t", i), MAIL_ADDR_TYPE.TO)
                m:subject("concurrency")
                m:msg("body")
                m:reply(0)
                if not ctx:send(m) then
                    got[i] = "send failed"
                    done = done + 1
                    return
                end
            end
            got[i] = true
            done = done + 1
        end)
    end
    while done < CONC_N do
        srey.sleep(20)
    end
    for i = 1, CONC_N do
        t:check(true == got[i], "并发协程 " .. i .. ": " .. tostring(got[i]))
    end
    t:eq(0, interleave, "服务端未检出命令交错")
    t:eq(CONC_N * ROUNDS, mails, "服务端收下的邮件数")

    -- quit 之后 ping 走重连：ping 只认"连接是否可用"，不区分连接是被谁关的
    ctx:quit()
    local gen = ctx.generation
    t:check(ctx:ping(), "quit 后 ping 自动重连")
    t:check(gen < ctx.generation, "重连让代次前进")
    ctx:quit()
end)
end)
