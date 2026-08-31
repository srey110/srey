-- crypt 绑定层单元测试：url/base64/crc/digest/hmac/cipher

local srey   = require("lib.srey")
local runner = require("test.runner")
local url    = require("srey.url")
local base64 = require("srey.base64")
local crc    = require("srey.crc")
local digest = require("srey.digest")
local hmac   = require("srey.hmac")
local cipher = require("srey.cipher")
local yyjson = require("yyjson")-- yyjson.null 是一个 NULL lightuserdata，用来测空指针拒收

srey.startup(function()
runner.run(function(t)
    -- ── url ────────────────────────────────────────────────────────────
    do
        local raw = "hello world?a=1&b=中文"
        local enc = url.encode(raw)
        t:check(not enc:find(" ", 1, true), "url encode no space")
        t:check(enc:find("%%", 1) ~= nil, "url encode has %")
        t:eq(raw, url.decode(enc), "url round-trip")
        -- url_decode 不写末尾 '\0'、严格在 [0,lens) 内原地缩短;binding 按返回长度取结果,无越界
        local big = string.rep("a", 1024)
        t:eq(big, url.decode(big), "url.decode 1024B no-escape")
    end
    do
        -- URL parse：解析 scheme/host/port/path/param
        local u = url.parse("https://user:pwd@example.com:8443/path?a=1&b=2#frag")
        t:eq("https", u.scheme, "url scheme")
        t:eq("user",  u.user,   "url user")
        t:eq("pwd",   u.psw,    "url psw")
        t:eq("example.com", u.host, "url host")
        t:eq("8443",  u.port,   "url port")
        t:eq("/path", u.path,   "url path")
        t:eq("frag",  u.anchor, "url anchor")
        t:eq("1", u.param.a, "url param a")
        t:eq("2", u.param.b, "url param b")
    end
    do
        -- URL parse 失败：超 1KB / 超 64 段路径返回 nil
        t:eq(nil, url.parse(string.rep("a", 1024)),       "url.parse 超 1KB 返回 nil")
        t:eq(nil, url.parse("/" .. string.rep("a/", 70)), "url.parse 超 64 段返回 nil")
    end
    do
        local u = url.parse("/api?a=1&=2&b=3")
        t:eq("1", u.param.a, "空名参数不截断 param 表: a")
        t:eq("3", u.param.b, "空名参数不截断 param 表: b(修复前丢失)")
        t:eq(nil, u.param[""], "空名参数本身不入表")
        t:eq("a=1&b=3", u.query, "query 跳过空名参数")
        local u2 = url.parse("/p?=x&token=abc")
        t:eq("abc", u2.param.token, "首参数无名时后续参数仍入表")
        t:eq("token=abc", u2.query, "首参数无名不再导致 query 字段整体缺失")
    end
    do
        local token = string.rep("x", 1500)
        local u = url.parse("wss://h/ws?token=" .. token, false)
        t:eq(token, u.param.token, "decode=0 超 1KB 参数值完整入表")
        t:eq("token=" .. token, u.query, "decode=0 长 query 不再被固定 1KB 缓冲整体丢弃")
        local seg = string.rep("p", 1500)
        local u2 = url.parse("wss://h/" .. seg, false)
        t:eq("/" .. seg, u2.path, "decode=0 长 path 不再被截断为空串")
    end

    -- ── base64 ─────────────────────────────────────────────────────────
    do
        local raw = "Hello, Base64!"
        local enc = base64.encode(raw)
        t:eq("SGVsbG8sIEJhc2U2NCE=", enc, "base64 encode 标准 vector")
        t:eq(raw, base64.decode(enc), "base64 round-trip")
        -- 空字符串
        t:eq("", base64.encode(""), "base64 encode empty")
        -- 二进制安全（含 \0）
        local bin = "\x00\x01\x02\xff"
        t:eq(bin, base64.decode(base64.encode(bin)), "base64 binary safe")
        -- 畸形输入返 nil，而不是空串——空串是"空输入"的合法结果，两者必须分得开，
        -- 否则调用方那句 if d then use(d) end 会把损坏数据当成合法内容收下
        t:eq(nil, base64.decode("dXNlcm5hbWU6!!!"), "base64 非法字符返 nil")
        t:eq(nil, base64.decode("SGVs=bG8="), "base64 填充后仍有数据返 nil")
        t:eq("", base64.decode(""), "base64 空输入仍解出空串")
    end

    -- ── crc ────────────────────────────────────────────────────────────
    do
        -- CRC32 标准 vector："123456789" → 0xCBF43926
        t:eq(0xCBF43926, crc.crc32("123456789"), "CRC32 标准 vector")
        -- 同一输入两次 crc 一致
        local a = crc.crc16("srey-crypt-test")
        local b = crc.crc16("srey-crypt-test")
        t:eq(a, b, "CRC16 idempotent")
        t:check(a >= 0 and a <= 0xFFFF, "CRC16 range")
    end

    -- ── digest ─────────────────────────────────────────────────────────
    do
        -- MD5("") = d41d8cd98f00b204e9800998ecf8427e
        local d = digest.new(DIGEST_TYPE.MD5)
        t:eq(16, d:size(), "MD5 size")
        d:update("")
        local out = d:final()
        t:eq("d41d8cd98f00b204e9800998ecf8427e", srey.hex(out, true), "MD5 empty")

        d:reset()
        d:update("abc")
        out = d:final()
        t:eq("900150983cd24fb0d6963f7d28e17f72", srey.hex(out, true), "MD5 abc")
    end
    do
        -- SHA256("abc") = ba7816bf...f20015ad
        local d = digest.new(DIGEST_TYPE.SHA256)
        t:eq(32, d:size(), "SHA256 size")
        d:update("abc")
        local out = d:final()
        t:eq("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
             srey.hex(out, true), "SHA256 abc")
    end
    do
        -- 分段 update 与一次性 update 结果一致
        local d1 = digest.new(DIGEST_TYPE.SHA1)
        d1:update("hello world")
        local h1 = d1:final()
        local d2 = digest.new(DIGEST_TYPE.SHA1)
        d2:update("hello ")
        d2:update("world")
        local h2 = d2:final()
        t:eq(srey.hex(h1, true), srey.hex(h2, true), "SHA1 分段 update 一致")
    end

    -- ── hmac ───────────────────────────────────────────────────────────
    do
        -- RFC 4231 HMAC-SHA256 test case 1: key="\x0b"x20, data="Hi There"
        local key = string.rep("\x0b", 20)
        local h = hmac.new(DIGEST_TYPE.SHA256, key)
        t:eq(32, h:size(), "HMAC-SHA256 size")
        h:update("Hi There")
        local out = h:final()
        t:eq("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
             srey.hex(out, true), "HMAC-SHA256 RFC 4231 #1")

        -- reset 后再计算
        h:reset()
        h:update("Hi There")
        local out2 = h:final()
        t:eq(srey.hex(out, true), srey.hex(out2, true), "HMAC reset round-trip")

        -- final 之后上下文自动复位且密钥仍在：不 reset 直接算下一条消息也对。
        -- 修复前这里对任何密钥都返回同一个常量，`for m in msgs do h:update(m); h:final() end`
        -- 这种写法会给每条消息发出相同的 tag
        h:update("Hi There")
        local out3 = h:final()
        t:eq(srey.hex(out, true), srey.hex(out3, true), "HMAC final 后无需 reset")
        local h2 = hmac.new(DIGEST_TYPE.SHA256, string.rep("\x0c", 20))
        h2:update("Hi There")
        h2:final()
        h2:update("Hi There")
        local other = h2:final()
        t:check(srey.hex(out3, true) ~= srey.hex(other, true), "不同密钥的第二轮 final 不相同")
    end
    do
        -- digest:final 同样自动复位，第二条消息不必先 reset
        local d = digest.new(DIGEST_TYPE.SHA256)
        d:update("abc")
        d:final()
        d:update("abc")
        local again = d:final()
        t:eq("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
             srey.hex(again, true), "digest final 后无需 reset")
    end
    do
        -- (指针, 长度) 入口在长度非 0 时拒收空指针：md5_update / hmac_update 只对 lens==0
        -- 早退，放进去就是从 NULL memcpy
        local d = digest.new(DIGEST_TYPE.MD5)
        t:eq(false, pcall(d.update, d, yyjson.null, 100), "digest:update NULL 指针被拒")
        local h = hmac.new(DIGEST_TYPE.SHA256, string.rep("\x0b", 20))
        t:eq(false, pcall(h.update, h, yyjson.null, 100), "hmac:update NULL 指针被拒")
        -- 长度 0 等价空缓冲，仍然放行
        local okempty = pcall(d.update, d, yyjson.null, 0)
        t:eq(true, okempty, "digest:update NULL 指针 + 0 长度放行")
    end

    -- ── cipher ─────────────────────────────────────────────────────────
    do
        -- AES-128 ECB round-trip（dofinal 自动 PKCS7 padding）
        local key = "0123456789abcdef"
        local plain = "srey cipher test."
        local enc = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 128, 1)
        enc:padding(PADDING_MODEL.PKCS57)
        local ct = enc:dofinal(plain)
        t:check(#ct > 0 and #ct % 16 == 0, "AES-128 ECB ct 16-aligned")

        local dec = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 128, 0)
        dec:padding(PADDING_MODEL.PKCS57)
        local pt = dec:dofinal(ct)
        t:eq(plain, pt, "AES-128 ECB round-trip")

        -- 填充校验失败返 nil 而不是空串：加密空明文再解回来本就是空串，
        -- 两者都当"长度 0"的话，伪造的密文能通过 `if pt then` 这种判断
        local empty_enc = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 128, 1)
        empty_enc:padding(PADDING_MODEL.PKCS57)
        local empty_ct = empty_enc:dofinal("")
        t:eq(16, #empty_ct, "空明文加密得一个整填充块")
        local empty_dec = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 128, 0)
        empty_dec:padding(PADDING_MODEL.PKCS57)
        t:eq("", empty_dec:dofinal(empty_ct), "空明文解回来是空串（成功）")
        local forged = string.char(empty_ct:byte(1) ~ 0xFF) .. empty_ct:sub(2)
        t:eq(nil, empty_dec:dofinal(forged), "填充校验失败返 nil")

        -- padding 取值必须落在枚举内。越界值会让 C 侧 _padding_data 的 switch 一个分支都不命中，
        -- 填充区一个字节都不写，pd_data 里上一块的明文尾巴就被当成填充加密发出去
        local pad = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 128, 1)
        t:eq(false, (pcall(pad.padding, pad, 99)), "越界 padding 报错")
        t:eq(false, (pcall(pad.padding, pad, -1)), "负 padding 报错")
        t:eq(true, (pcall(pad.padding, pad, PADDING_MODEL.NoPadding)), "NoPadding 合法")
        t:eq(true, (pcall(pad.padding, pad, PADDING_MODEL.ANSIX923)), "ANSIX923 合法")

        -- cipher.new 的三个枚举实参必须在绑定层挡住：越界 keybits 会一路走到 aes_init 的
        -- default ASSERTAB，那是 abort 整个进程而不是抛 Lua 错，脚本里写错一个常量就宕服
        t:eq(false, (pcall(cipher.new, CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 1234, 1)),
             "越界 keybits 报错而非 abort")
        t:eq(false, (pcall(cipher.new, CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 0, 1)),
             "keybits=0 报错")
        t:eq(false, (pcall(cipher.new, 99, CIPHER_MODEL.ECB, key, 128, 1)), "越界 engine 报错")
        t:eq(false, (pcall(cipher.new, 0, CIPHER_MODEL.ECB, key, 128, 1)), "engine=0 报错")
        t:eq(false, (pcall(cipher.new, CIPHER_TYPE.AES, 99, key, 128, 1)), "越界 model 报错")
        t:eq(false, (pcall(cipher.new, CIPHER_TYPE.AES, 0, key, 128, 1)), "model=0 报错")
        -- 先窄化再判范围的话，2^32+AES 会截成 AES 顺利通过校验，
        -- 用一个调用方从没指定过的算法加密
        t:eq(false, (pcall(cipher.new, 4294967296 + CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 128, 1)),
             "超 int32 的 engine 报错而非截断成 AES")
        t:eq(false, (pcall(cipher.new, CIPHER_TYPE.AES, 4294967296 + CIPHER_MODEL.ECB, key, 128, 1)),
             "超 int32 的 model 报错而非截断成 ECB")
        t:eq(false, (pcall(cipher.new, CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 4294967296 + 128, 1)),
             "超 int32 的 keybits 报错而非截断成 128")
        t:eq(false, (pcall(cipher.new, CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 128, 2)),
             "encrypt 非 0/1 报错")
        -- keybits 只有 AES 用得上，DES/DES3 传什么都不该被拦
        t:eq(true, (pcall(cipher.new, CIPHER_TYPE.DES, CIPHER_MODEL.ECB, key, 64, 1)),
             "DES 的 keybits=64 仍合法")
        t:eq(true, (pcall(cipher.new, CIPHER_TYPE.AES, CIPHER_MODEL.ECB, key, 256, 1)),
             "AES-256 合法")
    end
    do
        -- AES-128 CBC with IV
        local key = "0123456789abcdef"
        local iv  = "fedcba9876543210"
        local plain = "block cipher CBC mode test data."
        local enc = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.CBC, key, 128, 1)
        enc:padding(PADDING_MODEL.PKCS57)
        enc:iv(iv)
        local ct = enc:dofinal(plain)

        local dec = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.CBC, key, 128, 0)
        dec:padding(PADDING_MODEL.PKCS57)
        dec:iv(iv)
        t:eq(plain, dec:dofinal(ct), "AES-128 CBC round-trip")
    end
    do
        -- AES-128 CTR 流模式：密文长度 == 明文长度
        local key = "0123456789abcdef"
        local iv  = "fedcba9876543210"
        local plain = "stream cipher CTR mode short."
        local enc = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.CTR, key, 128, 1)
        enc:iv(iv)
        local ct = enc:dofinal(plain)
        t:eq(#plain, #ct, "AES-128 CTR ct len == pt len")

        local dec = cipher.new(CIPHER_TYPE.AES, CIPHER_MODEL.CTR, key, 128, 0)
        dec:iv(iv)
        t:eq(plain, dec:dofinal(ct), "AES-128 CTR round-trip")
    end
    do
        -- DES round-trip
        local key = "12345678"
        local plain = "DES test."
        local enc = cipher.new(CIPHER_TYPE.DES, CIPHER_MODEL.ECB, key, 64, 1)
        enc:padding(PADDING_MODEL.PKCS57)
        local ct = enc:dofinal(plain)
        local dec = cipher.new(CIPHER_TYPE.DES, CIPHER_MODEL.ECB, key, 64, 0)
        dec:padding(PADDING_MODEL.PKCS57)
        t:eq(plain, dec:dofinal(ct), "DES ECB round-trip")
    end
end)
end)
