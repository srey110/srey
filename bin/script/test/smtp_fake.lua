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

local conns = {} -- skid -> 连接状态
local interleave = 0 -- 检测到的交错次数
local mails = 0 -- 服务端确认收下的邮件数
local fail_rset = false -- 置 true 让下一条 RSET 被回 500(一次性)，压客户端的拆连接收尾
local ehlo_injected = false -- 客户端把问候里的裸 LF 原样拼进 EHLO 行就置 true
local hostile = false -- 逐连接轮换：一次发正常应答，一次发合法但刁钻的形态，两边都得走通
local ehlo_hosts = {} -- 收到过的 EHLO 参数,用来确认正常问候下主机名是照着服务端给的填

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
        if line:find("\n", 1, true) then
            ehlo_injected = true
        end
        ehlo_hosts[line:match("^%a+%s+(%S+)") or ""] = true
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
        -- 多行 235 是合法形式，客户端只消费首行的话剩下那行会错开后续配对；
        -- 单行才是常见形态，两种轮换着发，谁都不能少了覆盖
        if fc.hostile then
            _reply(fd, skid, "235-2.7.0 Authentication successful\r\n235 2.7.0 Welcome\r\n")
        else
            _reply(fd, skid, "235 2.7.0 Authentication successful\r\n")
        end
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
        if fail_rset then
            fail_rset = false
            _reply(fd, skid, "500 rset rejected\r\n")
        else
            _reply(fd, skid, "250 OK\r\n")
        end
    elseif up:find("^QUIT") then
        _reply(fd, skid, "221 Bye\r\n")
    else
        _reply(fd, skid, "250 OK\r\n")-- NOOP 及其余一律 250
    end
end

srey.startup(function()
    srey.on_accepted(function(pktype, fd, skid)
        hostile = not hostile
        conns[skid] = { buf = "", authstep = 0, indata = false, sender = -1, hostile = hostile }
        if hostile then
            -- 裸 LF 不是 CRLF，客户端的多行响应扫描认不出它，会一路活到 EHLO 参数里
            _reply(fd, skid, "220 fake.smtp.local\nRSET injected\r\n")
        else
            _reply(fd, skid, "220 normal.smtp.local ESMTP\r\n")
        end
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
runner.run(function(t)
    -- listen 守卫必须在 run 体内：在外面 return 会跳过整个 runner.run，本模块永远不向
    -- reporter 上报，而 reporter 要凑齐 #TESTS 个模块才打印汇总——一个没到，整轮的
    -- 汇总一行都不出，其余模块全过也看不见
    local lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", _PORT, NET_EV.ACCEPT)
    if ERR_FAILED == lid then
        t:fail("fake smtp: listen " .. _PORT .. " failed")
        return
    end
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
    -- 有界等待：srey.fork 的错被 _coro_cb 的 xpcall 吞掉不传播，
    -- 协程体任一处抛错就再也不会 done+1，无界 while 会把整个模块挂到 reporter 超时
    for _ = 1, 1500 do-- 1500 x 20ms = 30s 上限
        if done >= CONC_N then break end
        srey.sleep(20)
    end
    t:eq(CONC_N, done, "并发协程全部完成 (" .. done .. "/" .. CONC_N .. ")")
    for i = 1, CONC_N do
        t:check(true == got[i], "并发协程 " .. i .. ": " .. tostring(got[i]))
    end
    t:eq(0, interleave, "服务端未检出命令交错")
    t:eq(CONC_N * ROUNDS, mails, "服务端收下的邮件数")
    -- 问候里的裸 LF 不得被原样拼进 EHLO——那等于往自己的命令行里插了第二条命令。
    -- 16 封信共用一条连接，全程只有这一次 EHLO：这是一次采样，不是 16 次
    t:eq(false, ehlo_injected, "问候里的裸 LF 未被拼进 EHLO 行")

    -- quit 之后 ping 走重连：ping 只认"连接是否可用"，不区分连接是被谁关的
    ctx:quit()
    local gen = ctx.generation
    t:check(ctx:ping(), "quit 后 ping 自动重连")
    t:check(gen < ctx.generation, "重连让代次前进")

    -- quit 同样要让代次前进：不动的话，quit 前 prepare 出来的 stmt / session 拿旧代次一比仍算有效
    local stale_gen = ctx.generation
    ctx:quit()
    t:check(stale_gen < ctx.generation, "quit 让代次前进")
    -- 再重连 + 再断开，把代次推到离 stale_gen 更远的位置：这样下面那次 _doconnect 拿到的
    -- 才是真正的"过期代次"，短路条件的第一项(代次已变)成立，考的就是第二项 established
    t:check(ctx:ping(), "再重连一次令代次继续前进")
    ctx:quit()
    -- 短路条件若只认代次就会对着已关的连接报成功，故这里必须真把连接建起来。
    -- 直接调内部 _doconnect 是为了造出"过期代次"这个入参，此刻无并发协程，绕开锁安全
    t:eq(true, ctx:_doconnect(stale_gen), "拿过期代次的 connect 返回成功")
    t:check(INVALID_SOCK ~= ctx.conn:sock_id(), "且连接是真建起来的，不是短路返回")

    -- RSET 失败：邮件本身已投成功故 send 返 true，但连接要就地拆掉且状态同步落账——
    -- established 不清的话，之后排队醒来的 connect 会对着这条已关的连接短路报成功
    local rgen = ctx.generation
    fail_rset = true
    local rm = mail.new()
    rm:from("srey", "c9@t")
    rm:addrs_add("r9@t", MAIL_ADDR_TYPE.TO)
    rm:subject("rset_fail")
    rm:msg("body")
    rm:reply(0)
    t:eq(true, ctx:send(rm), "RSET 失败不影响邮件本身的成败")
    t:eq(false, ctx.established, "RSET 失败后 established 已清")
    t:check(rgen < ctx.generation, "RSET 失败让代次前进")
    t:check(ctx:ping(), "RSET 失败后 ping 能重连")
    ctx:quit()

    -- 建链失败时代次同样要前进：_connect 里的 try_connect 已经无条件覆写过 sk.fd，原来那条连接
    -- 不在了。只在成功时前进的话，失败重连之后 stmt/session 拿旧代次一比仍算"没换过连接"，
    -- st:close() 会朝 INVALID_SOCK 发包返 false，而它的注解写的是"重连后无需再发返 true"。
    -- 直接换掉 _connect 是为了造出"建链失败"这个状态，此刻无并发协程，绕开锁安全
    do
        local saved = rawget(ctx, "_connect")
        local fgen = ctx.generation
        ctx._connect = function() return false end
        t:eq(false, ctx:_doconnect(fgen), "_connect 失败时 _doconnect 返 false")
        ctx._connect = saved
        t:check(fgen < ctx.generation, "建链失败同样让代次前进")
        t:eq(false, ctx.established, "建链失败后 established 为 false")
        t:check(ctx:ping(), "之后照常能重连")
        ctx:quit()
    end

    -- 取值全部回读 C 侧（lib.mail 不再留副本），故被拒的调用不可能改到状态：
    -- smtp:send 发 RCPT TO 用的就是 MIME 头里那份，两者再没有分叉的余地
    do
        local em = mail.new()
        em:from("srey", "c10@t")
        em:addrs_add("r10@t", MAIL_ADDR_TYPE.TO)
        t:eq(false, pcall(em.addrs_add, em, "bad@t", 9), "越界收件人类型被拒")
        t:eq(1, #em:addrs_get(), "抛错后收件人列表不变")
        t:eq(false, pcall(em.from, em, "x", nil), "from 非法入参被拒")
        t:eq("c10@t", em:from_get(), "抛错后发件人不变")
    end

    -- 两种问候形态都得走通：正常那半边照服务端给的主机名填 EHLO，刁钻那半边退回 localhost。
    -- 放在最后是因为服务端逐连接轮换，要等上面这些重连都发生过才凑齐两种
    t:check(ehlo_hosts["normal.smtp.local"], "正常问候下 EHLO 用服务端给的主机名")
    t:check(ehlo_hosts["localhost"], "裸 LF 问候下 EHLO 退回 localhost")

    -- 收尾:把端口还回去。不还的话进程要等 SIGINT 才收，紧接着再起一次 srey 就撞
    -- EADDRINUSE，上面那道 listen 守卫直接判失败
    srey.unlisten(lid)
end)
end)
