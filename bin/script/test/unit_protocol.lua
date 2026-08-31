-- protocol 绑定层单元测试（不走网络）：
-- websock pack_*, smtp pack_*, mail pack, redis.pack, harbor.pack, http.code_status

local srey    = require("lib.srey")
local runner  = require("test.runner")
local utils   = require("srey.utils")
local websock = require("srey.websock")
local http    = require("srey.http")
local redis   = require("lib.redis")
local harbor  = require("srey.harbor")
local smtp    = require("srey.smtp")
local mail    = require("srey.smtp.mail")
local yyjson  = require("yyjson")-- yyjson.null 是一个 NULL lightuserdata，用来测空指针拒收
local base64  = require("srey.base64")

srey.startup(function()
runner.run(function(t)
    -- ── http.code_status ───────────────────────────────────────────────
    t:eq("OK",                    http.code_status(200), "http 200")
    t:eq("Not Found",             http.code_status(404), "http 404")
    t:eq("Internal Server Error", http.code_status(500), "http 500")

    -- ── 封包访问器拒收空指针 ───────────────────────────────────────────
    do
        -- 这一族拿到 pack 就直接解引用，空指针由 LUACHECK_LUDATA 在参数处拦下
        t:eq(false, pcall(websock.unpack, yyjson.null), "websock.unpack NULL 指针被拒")
        t:eq(false, pcall(http.chunked, yyjson.null), "http.chunked NULL 指针被拒")
        t:eq(false, pcall(http.status, yyjson.null), "http.status NULL 指针被拒")
    end

    -- ── redis.pack ─────────────────────────────────────────────────────
    -- RESP 协议格式：*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n
    do
        local req = redis.pack("SET", "key", "hello")
        t:eq("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nhello\r\n", req, "redis.pack SET")
        local req2 = redis.pack("GET", "key")
        t:eq("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n", req2, "redis.pack GET")
        local req3 = redis.pack("DEL", "k1", "k2", "k3")
        t:check(req3:sub(1, 4) == "*4\r\n", "redis.pack DEL header")
    end

    -- ── harbor.pack ────────────────────────────────────────────────────
    do
        local data, size = harbor.pack(0x10, 0, 0, "hello", 5)
        t:check(data ~= nil and size > 0, "harbor.pack returns data")
        utils.ud_free(data)
        -- 无 payload
        data, size = harbor.pack(0x10, 1, 0)
        t:check(data ~= nil and size > 0, "harbor.pack no payload")
        utils.ud_free(data)
    end

    -- ── websock pack 系列（验证返回非空，记得 ud_free） ────────────────
    do
        -- handshake：返回 (pack, size, hsctx)，hsctx 也需要释放
        local pack, size, hsctx = websock.pack_handshake("example.com", nil, "chat")
        t:check(pack ~= nil and size > 0, "websock.pack_handshake")
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("GET ", 1, true) ~= nil, "handshake has GET")
        t:check(txt:find("Upgrade: websocket", 1, true) ~= nil, "handshake has Upgrade")
        t:check(txt:find("Host: example.com", 1, true) ~= nil, "handshake has Host")
        t:check(txt:find("Sec%-WebSocket%-Protocol: chat") ~= nil, "handshake has Sec-WebSocket-Protocol")
        utils.ud_free(pack)
        utils.ud_free(hsctx)
    end
    do
        -- secprots：nil/非 lightuserdata 安全返回 nil（真实协商结果由 C 集成测试覆盖）
        t:check(nil == websock.secprots(nil), "websock.secprots(nil) 返回 nil")
    end
    do
        local pack, size = websock.pack_ping(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_ping")
        utils.ud_free(pack)
        pack, size = websock.pack_pong(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_pong")
        utils.ud_free(pack)
        pack, size = websock.pack_close(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_close")
        utils.ud_free(pack)
    end
    do
        -- text/binary 服务端帧（mask=0），fin=1：payload "hi" → 2+2 字节
        local pack, size = websock.pack_text(0, 1, "hi")
        t:check(pack ~= nil and size == 4, "websock.pack_text small frame")
        utils.ud_free(pack)
        pack, size = websock.pack_binary(0, 1, "\x01\x02\x03")
        t:check(pack ~= nil and size == 5, "websock.pack_binary small frame")
        utils.ud_free(pack)
        -- 客户端帧带 4 字节 mask，长度加 4
        pack, size = websock.pack_text(1, 1, "hi")
        t:check(pack ~= nil and size == 8, "websock.pack_text client mask")
        utils.ud_free(pack)
        -- continua fin=1 + nil payload（终止帧）
        pack, size = websock.pack_continua(0, 1, "")
        t:check(pack ~= nil and size >= 2, "websock.pack_continua end")
        utils.ud_free(pack)
    end

    -- ── smtp pack 系列 ─────────────────────────────────────────────────
    do
        local s = smtp.new("127.0.0.1", 25, nil, "user", "psw")
        t:check(s ~= nil, "smtp.new 合法参数")
        -- ip / user / psw 超长返回 nil 而不是静默截断：截断后的密码拿去认证只换回
        -- 服务端一句 535，调用方看不出是自己传长了（同 mysql.new / pgsql.new / mongo.new）
        local toolong = string.rep("x", 96)
        t:eq(nil, smtp.new("127.0.0.1", 25, nil, "user", toolong), "smtp.new 密码超长返 nil")
        t:eq(nil, smtp.new("127.0.0.1", 25, nil, toolong, "psw"),  "smtp.new 用户名超长返 nil")
        t:eq(nil, smtp.new(toolong, 25, nil, "user", "psw"),       "smtp.new ip 超长返 nil")
        t:check(smtp.new("127.0.0.1", 25, nil, "user", string.rep("y", 63)) ~= nil,
                "smtp.new 正好 63 字节仍合法")
        -- 正常地址
        local pack, size = s:pack_from("alice@example.com")
        t:check(pack ~= nil and size > 0, "smtp pack_from")
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("MAIL FROM:", 1, true) ~= nil, "smtp pack_from has prefix")
        t:check(txt:find("<alice@example.com>", 1, true) ~= nil, "smtp pack_from has addr")
        t:check(txt:sub(-2) == "\r\n", "smtp pack_from ends CRLF")
        utils.ud_free(pack)

        pack, size = s:pack_rcpt("bob@example.com")
        t:check(pack ~= nil and size > 0, "smtp pack_rcpt")
        txt = srey.ud_str(pack, size)
        t:check(txt:find("RCPT TO:", 1, true) ~= nil, "smtp pack_rcpt has prefix")
        utils.ud_free(pack)

        -- CRLF 注入防御
        pack, size = s:pack_from("evil@example.com\r\nINJECT")
        t:eq(nil, pack, "smtp pack_from CRLF injection rejected")
        pack, size = s:pack_rcpt("evil@example.com\nINJECT")
        t:eq(nil, pack, "smtp pack_rcpt LF injection rejected")

        -- 固定命令
        pack, size = s:pack_reset()
        t:check(pack ~= nil and srey.ud_str(pack, size):find("RSET", 1, true) ~= nil, "smtp pack_reset")
        utils.ud_free(pack)
        pack, size = s:pack_quit()
        t:check(pack ~= nil and srey.ud_str(pack, size):find("QUIT", 1, true) ~= nil, "smtp pack_quit")
        utils.ud_free(pack)
        pack, size = s:pack_data()
        t:check(pack ~= nil and srey.ud_str(pack, size):find("DATA", 1, true) ~= nil, "smtp pack_data")
        utils.ud_free(pack)
        pack, size = s:pack_ping()
        t:check(pack ~= nil and srey.ud_str(pack, size):find("NOOP", 1, true) ~= nil, "smtp pack_ping (NOOP)")
        utils.ud_free(pack)
    end

    -- ── mail pack（MIME 输出） ─────────────────────────────────────────
    do
        -- reply 的入参形态单独拿个对象验，免得改动下面 pack 断言所依赖的状态。
        -- 标注是 integer?、文档写"nil 视为 0"，那不带参数（LUA_TNONE）也得照收：
        -- 只判 LUA_TNIL 的话会落到 luaL_checkinteger 上报 "number expected, got no value"
        local rm = mail.new()
        t:eq(true, pcall(rm.reply, rm), "mail:reply() 不带参数等同 0")
        t:eq(true, pcall(rm.reply, rm, nil), "mail:reply(nil) 等同 0")
        t:eq(true, pcall(rm.reply, rm, 1), "mail:reply(1) 请求回执")
        -- 契约是"非 0 即真"而不是只收 0/1，别顺手收紧成 lpub_opt_flag
        t:eq(true, pcall(rm.reply, rm, 2), "mail:reply 非 0 值一律当请求回执")

        local m = mail.new()
        m:from("Srey", "srey@example.com")
        m:addrs_add("alice@example.com", 1)  -- TO
        m:subject("unit test")
        m:msg("plain text body")
        m:html("<h1>hi</h1>")
        local pack, size = m:pack()
        t:check(pack ~= nil and size > 0, "mail pack returns content")
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("Subject:", 1, true) ~= nil, "mail has Subject")
        t:check(txt:find("From:", 1, true) ~= nil, "mail has From")
        t:check(txt:find("To:", 1, true) ~= nil, "mail has To")
        -- 正文与 html 段都是 base64：8bit 原样写出时一段没换行的长正文会造出超过
        -- RFC 5321 §4.5.3.1.6 那 1000 octet 上限的 DATA 行，纯文本单段还会因为缺
        -- Content-Type 被按 us-ascii 解释
        t:check(txt:find("plain text body", 1, true) == nil, "正文不再以明文出现")
        t:check(txt:find(base64.encode("plain text body"), 1, true) ~= nil, "正文以 base64 出现")
        t:check(txt:find("multipart/alternative", 1, true) ~= nil, "mail has multipart/alternative")
        t:check(txt:find("text/html", 1, true) ~= nil, "mail has text/html header")
        -- RFC 5322 §3.4 的 name-addr 形式
        t:check(txt:find("From: Srey <srey@example.com>", 1, true) ~= nil, "From 用 display-name <addr>")
        utils.ud_free(pack)
    end

    -- ── mail 释放后再调用 ─────────────────────────────────────────────
    -- REG_MTABLE 令 __gc 经 __index 也是个普通方法，业务一行 m:__gc() 就能提前释放；
    -- 此后任何方法都必须报可捕获的 Lua 错，而不是拿着已释放的 mail_ctx 往下走
    do
        local m = mail.new()
        m:from("Srey", "srey@example.com")
        m:addrs_add("alice@example.com", 1)
        m:__gc()

        -- array_free 只置空 ptr 不复位 size/maxsize，无守卫时 array_push_back 跳过扩容分支
        -- 直接往 NULL 基址 memcpy，是空指针写而非断言
        t:eq(false, pcall(function() m:addrs_add("bob@example.com", 1) end), "释放后 addrs_add 被拒")
        t:eq(false, pcall(function() m:attach_clear() end), "释放后 attach_clear 被拒")
        t:eq(false, pcall(function() m:subject("x") end),   "释放后 subject 被拒")
        t:eq(false, pcall(function() m:clear() end),        "释放后 clear 被拒")
        local ok, err = pcall(function() return m:pack() end)
        t:eq(false, ok, "释放后 pack 被拒")
        t:check(type(err) == "string" and nil ~= err:find("freed"), "错误信息点明已释放")

        -- 显式释放后真正的 __gc 仍会跑一遍，必须幂等
        t:eq(true, pcall(function() m:__gc() end), "重复 __gc 安全")
    end
end)
end)
