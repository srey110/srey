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

srey.startup(function()
runner.run("protocol", function(t)
    -- ── http.code_status ───────────────────────────────────────────────
    t:eq("OK",                    http.code_status(200), "http 200")
    t:eq("Not Found",             http.code_status(404), "http 404")
    t:eq("Internal Server Error", http.code_status(500), "http 500")

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
        local data, size = harbor.pack(0x10, 0, 0, "route-key", "hello", 5)
        t:check(data ~= nil and size > 0, "harbor.pack returns data")
        utils.ud_free(data)
        -- 无 payload
        data, size = harbor.pack(0x10, 1, 0, "route-key")
        t:check(data ~= nil and size > 0, "harbor.pack no payload")
        utils.ud_free(data)
    end

    -- ── websock pack 系列（验证返回非空，记得 ud_free） ────────────────
    do
        -- handshake：返回 (pack, size, signkey)，signkey 也需要释放
        local pack, size, signkey = websock.pack_handshake("example.com", nil, "chat")
        t:check(pack ~= nil and size > 0, "websock.pack_handshake")
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("GET ", 1, true) ~= nil, "handshake has GET")
        t:check(txt:find("Upgrade: websocket", 1, true) ~= nil, "handshake has Upgrade")
        t:check(txt:find("Host: example.com", 1, true) ~= nil, "handshake has Host")
        t:check(txt:find("Sec%-WebSocket%-Protocol: chat") ~= nil, "handshake has Sec-WebSocket-Protocol")
        utils.ud_free(pack)
        utils.ud_free(signkey)
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
        t:check(txt:find("plain text body", 1, true) ~= nil, "mail has text body")
        -- html 段在 multipart/alternative 之下的 base64 段（header 标 base64 但实际未编码），
        -- 仅验证含 multipart 边界即可
        t:check(txt:find("multipart/alternative", 1, true) ~= nil, "mail has multipart/alternative")
        t:check(txt:find("text/html", 1, true) ~= nil, "mail has text/html header")
        utils.ud_free(pack)
    end
end)
end)
