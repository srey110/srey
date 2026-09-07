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
        -- 整数值的浮点要按整数上线：3600000/1000 在 Lua 里是 float，写成 "3600.0"
        -- 会被 Redis 判 not an integer；大数还会退化成 "1e+17"
        t:eq("*4\r\n$5\r\nSETEX\r\n$1\r\nk\r\n$4\r\n3600\r\n$1\r\nv\r\n",
             redis.pack("SETEX", "k", 3600000 / 1000, "v"), "整数值浮点按整数编码")
        t:check(nil ~= redis.pack("ZADD", "k", 1e17, "m"):find("100000000000000000", 1, true),
                "大整数值浮点不退化成科学计数法")
        -- 真正的小数原样保留
        t:check(nil ~= redis.pack("SET", "k", 3.25):find("3.25", 1, true), "小数原样编码")
    end

    -- ── harbor.pack ────────────────────────────────────────────────────
    do
        -- 组出来的是一条 HTTP POST：call=0 走 /request，call=1 走 /call，
        -- dst / type 进查询串，payload 进 body。两次只判"非空"的话，
        -- call 标志接反、dst 没写进 url 都发现不了
        local data, size = harbor.pack(0x10, 0, 0, "hello", 5)
        t:check(data ~= nil and size > 0, "harbor.pack returns data")
        local w = srey.ud_str(data, size)
        t:check(nil ~= w:find("POST /request?dst=16&type=0 ", 1, true), "call=0 走 /request，dst/type 入查询串")
        t:check(nil ~= w:find("Content-Length: 5", 1, true), "Content-Length 按 payload 长度写")
        t:check(w:sub(-5) == "hello", "payload 落在报文末尾")
        utils.ud_free(data)
        -- 无 payload
        data, size = harbor.pack(0x10, 1, 0)
        t:check(data ~= nil and size > 0, "harbor.pack no payload")
        w = srey.ud_str(data, size)
        t:check(nil ~= w:find("POST /call?dst=16&type=0 ", 1, true), "call=1 走 /call")
        t:check(nil ~= w:find("Content-Length: 0", 1, true), "无 payload 时 Content-Length 为 0")
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
        -- 只判长度分不开这几个包：ping / pong / close 三种帧的差别全在首字节的 opcode 上，
        -- 长度一模一样。绑定层把 ping 接到 pong 上，"size >= 2" 一个都发现不了。
        -- 首字节 = FIN(0x80) | opcode，opcode 取自 lib/protocol/websock.h 的 ws_prot
        local function _head(pack, size, n)
            return { srey.ud_str(pack, size):byte(1, n) }
        end
        local pack, size = websock.pack_ping(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_ping")
        t:eq(0x89, pack and _head(pack, size, 1)[1], "ping 首字节 FIN|WS_PING")
        utils.ud_free(pack)
        pack, size = websock.pack_pong(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_pong")
        t:eq(0x8A, pack and _head(pack, size, 1)[1], "pong 首字节 FIN|WS_PONG")
        utils.ud_free(pack)
        pack, size = websock.pack_close(0)
        t:check(pack ~= nil and size >= 2, "websock.pack_close")
        t:eq(0x88, pack and _head(pack, size, 1)[1], "close 首字节 FIN|WS_CLOSE")
        utils.ud_free(pack)

        -- text/binary 服务端帧（mask=0），fin=1：payload "hi" → 2+2 字节。
        -- text 与 binary 同样只差 opcode，长度还各不相同，于是长度对了更容易让人以为测过了
        pack, size = websock.pack_text(0, 1, "hi")
        t:check(pack ~= nil and size == 4, "websock.pack_text small frame")
        local h = pack and _head(pack, size, 2)
        t:eq(0x81, h and h[1], "text 首字节 FIN|WS_TEXT")
        t:eq(0x02, h and h[2], "text 次字节：无掩码 + 长度 2")
        utils.ud_free(pack)
        pack, size = websock.pack_binary(0, 1, "\x01\x02\x03")
        t:check(pack ~= nil and size == 5, "websock.pack_binary small frame")
        h = pack and _head(pack, size, 2)
        t:eq(0x82, h and h[1], "binary 首字节 FIN|WS_BINARY")
        t:eq(0x03, h and h[2], "binary 次字节：无掩码 + 长度 3")
        utils.ud_free(pack)
        -- 客户端帧带 4 字节 mask，长度加 4；掩码位在次字节的最高位
        pack, size = websock.pack_text(1, 1, "hi")
        t:check(pack ~= nil and size == 8, "websock.pack_text client mask")
        h = pack and _head(pack, size, 2)
        t:eq(0x81, h and h[1], "客户端 text 首字节不变")
        t:eq(0x82, h and h[2], "客户端帧次字节置掩码位(0x80) + 长度 2")
        utils.ud_free(pack)
        -- continua fin=1 + 空 payload（终止帧）：opcode 必须是 WS_CONTINUE(0)，
        -- 写成 WS_TEXT 的话对端会把它当一条新消息的开头
        pack, size = websock.pack_continua(0, 1, "")
        t:check(pack ~= nil and size >= 2, "websock.pack_continua end")
        t:eq(0x80, pack and _head(pack, size, 1)[1], "continua 首字节 FIN|WS_CONTINUE")
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
        -- 贴边各一条：只测 63 合法 + 96 被拒的话，上限放宽到 95 也抓不到
        t:check(smtp.new("127.0.0.1", 25, nil, "user", string.rep("y", 63)) ~= nil,
                "smtp.new 正好 63 字节仍合法")
        t:eq(nil, smtp.new("127.0.0.1", 25, nil, "user", string.rep("y", 64)),
             "smtp.new 64 字节即超限(缓冲 64 含结尾 NUL)")
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
        -- 收件人类型裸 cast 会让越界值原样存进去:_mail_pack_addr 只认 TO/CC,
        -- 类型是 9 的地址照发 RCPT TO 却不出现在 To: / Cc: 里,可见收件人静默变密送
        t:eq(false, pcall(function() m:addrs_add("x@example.com", 9) end), "越界收件人类型被拒")
        t:eq(false, pcall(function() m:addrs_add("x@example.com", 0) end), "类型 0 被拒")
        m:addrs_add("alice@example.com", 1) -- TO
        -- 发件人/收件人只存在 C 侧，取值一律回读 mail_ctx。Lua 层再留一份副本的话，
        -- CRLF 净化与定长截断这两道改写就会让两份状态分叉（MIME 头一个样、RCPT TO 另一个样）
        t:eq("srey@example.com", m:from_get(), "from_get 回读 C 侧发件人")
        t:eq("alice@example.com", m:addrs_get()[1], "addrs_get 回读 C 侧收件人")
        m:addrs_add("bob@example.com", 2) -- CC
        m:addrs_add("eve@example.com", 3) -- BCC
        local got = m:addrs_get()
        t:eq(3, #got, "TO/CC/BCC 都在 addrs_get 里")
        t:eq("eve@example.com", got[3], "按加入顺序交出")
        m:addrs_clear()
        t:eq(0, #m:addrs_get(), "addrs_clear 后 addrs_get 为空表")
        -- 这一条是删掉 Lua 镜像的理由：交出来的必须是净化后的存量值，不是传进去的原串
        m:from("Srey", "a\r\nb@example.com")
        t:eq("ab@example.com", m:from_get(), "from_get 交出剔除 CRLF 后的存量值")
        m:from("Srey", "srey@example.com")
        m:addrs_add("alice@example.com", 1)
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
        t:eq(false, pcall(function() return m:from_get() end),  "释放后 from_get 被拒")
        t:eq(false, pcall(function() return m:addrs_get() end), "释放后 addrs_get 被拒")
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
