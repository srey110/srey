-- 假 PostgreSQL 服务端：SCRAM 已开始后服务端改要明文 / MD5，客户端必须直接断开、口令不发出去。
-- 真服务端不会中途换方法，协议层单元用例又分不清"守卫拒了"和"发包失败"(都是 PROT_ERROR)，只能看线上字节。
-- 服务端回调与用例协程同属一个 task，单线程依次执行，故下面这些状态用普通变量即可。
-- 注册：task.register("test.pgsql_fake", "pgsql_fake", 0, port)

local srey   = require("lib.srey")
local runner = require("test.runner")
local pgsql  = require("lib.pgsql")

local _PORT = ... or 12527
local PASSWORD = "srey-fake-password"
local AUTH_PLAIN <const> = 0x03
local AUTH_MD5 <const> = 0x05
local AUTH_SASL <const> = 0x0a

local msgs = {} -- 本轮服务端按序收到的消息
local scram = false -- true：先走 SASL，收到 client-first 后再换方法；false：一上来就要 code（对照组）
local code = AUTH_PLAIN -- 本轮要求的方法
local nego = false -- 非 false：版本协商轮，回 NegotiateProtocolVersion 降到 3.0，值是随后回的取消密钥长度

-- Authentication 消息：'R' + Int32(长度，含自身) + Int32(认证码) + 附加内容
local function _auth(c, extra)
    local body = string.pack(">I4", c) .. (extra or "")
    return "R" .. string.pack(">I4", #body + 4) .. body
end

srey.startup(function()
    srey.on_recved(function(pktype, sk, _, _, data, size)
        if PACK_TYPE.NONE ~= pktype or not data then
            return
        end
        msgs[#msgs + 1] = srey.ud_str(data, size)
        local resp
        if nego then
            if 1 == #msgs then
                resp = "v" .. string.pack(">I4I4I4", 12, 0, 0)
                    .. _auth(0)
                    .. "K" .. string.pack(">I4I4", 8 + nego, 4242) .. string.rep("k", nego)
                    .. "Z" .. string.pack(">I4", 5) .. "I"
            end
        elseif 1 == #msgs and scram then
            resp = _auth(AUTH_SASL, "SCRAM-SHA-256\0\0")
        elseif 1 == #msgs or (2 == #msgs and scram) then
            resp = _auth(code, AUTH_MD5 == code and "salt" or "")
        end
        if resp then
            srey.send(sk, resp, #resp, 1)
        else
            srey.close(sk)
        end
    end)

runner.run(function(t)
    -- listen 守卫必须在 run 体内，理由同 smtp_fake
    local lid = srey.listen(PACK_TYPE.NONE, SSL_NAME.NONE, "127.0.0.1", _PORT, NET_EV.ACCEPT)
    if ERR_FAILED == lid then
        t:fail("fake pgsql: listen " .. _PORT .. " failed")
        return
    end
    -- 跑一轮：连不上才对；等一小会儿让客户端已经发出的字节落到上面的回调里
    local function _round(use_scram, auth, label)
        msgs, scram, code = {}, use_scram, auth
        local pg = pgsql.new("127.0.0.1", _PORT, SSL_NAME.NONE, "user", PASSWORD, "db")
        t:check(not pg:connect(), label .. " 建链失败")
        srey.sleep(100)
    end

    -- 对照组：一上来就要明文，口令照发。证明下面"没看到口令"不是因为这个假服务端看不见
    _round(false, AUTH_PLAIN, "对照组")
    t:check(msgs[2] and msgs[2]:find(PASSWORD, 1, true) ~= nil, "对照组：明文认证时服务端收到了口令")

    _round(true, AUTH_PLAIN, "SCRAM 中换明文")
    t:eq(2, #msgs, "SCRAM 中换明文：只收到 Startup 与 client-first，没有第三条消息")
    t:check(msgs[2] and msgs[2]:find("SCRAM-SHA-256", 1, true) ~= nil, "SCRAM 中换明文：第二条确是 client-first")
    t:check(not table.concat(msgs):find(PASSWORD, 1, true), "SCRAM 中换明文：口令没发出去")

    _round(true, AUTH_MD5, "SCRAM 中换 MD5")
    t:eq(2, #msgs, "SCRAM 中换 MD5：只收到 Startup 与 client-first，没有第三条消息")

    -- 版本协商：请求 3.2，服务端回 NegotiateProtocolVersion 降到 3.0，之后按 3.0 只认 4 字节密钥
    msgs, nego = {}, 4
    local pg = pgsql.new("127.0.0.1", _PORT, SSL_NAME.NONE, "user", PASSWORD, "db")
    t:check(pg:set_protocol(2), "set_protocol(2)")
    t:check(pg:connect(), "降到 3.0：建链成功")
    t:eq(2, msgs[1] and string.unpack(">I2", msgs[1], 7), "Startup 请求的次版本是 2")
    local cp = pg.pg:pack_cancel()
    t:eq(16, cp and #cp, "降到 3.0 后取消请求按 4 字节密钥组包")
    pg:quit()
    msgs, nego = {}, 32
    pg = pgsql.new("127.0.0.1", _PORT, SSL_NAME.NONE, "user", PASSWORD, "db")
    pg:set_protocol(2)
    t:check(not pg:connect(), "降到 3.0 后密钥不是 4 字节：拒连")
    nego = false

    srey.unlisten(lid)
end)
end)
