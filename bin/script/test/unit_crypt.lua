-- crypt 绑定层单元测试：url/base64/crc/digest/hmac/cipher

local srey   = require("lib.srey")
local runner = require("test.runner")
local url    = require("srey.url")
local base64 = require("srey.base64")
local crc    = require("srey.crc")
local digest = require("srey.digest")
local hmac   = require("srey.hmac")
local cipher = require("srey.cipher")

srey.startup(function()
runner.run("crypt", function(t)
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
        -- srey.hex 输出大写，转小写后比对标准向量
        t:eq("d41d8cd98f00b204e9800998ecf8427e", srey.hex(out, #out):lower(), "MD5 empty")

        d:reset()
        d:update("abc")
        out = d:final()
        t:eq("900150983cd24fb0d6963f7d28e17f72", srey.hex(out, #out):lower(), "MD5 abc")
    end
    do
        -- SHA256("abc") = ba7816bf...f20015ad
        local d = digest.new(DIGEST_TYPE.SHA256)
        t:eq(32, d:size(), "SHA256 size")
        d:update("abc")
        local out = d:final()
        t:eq("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
             srey.hex(out, #out):lower(), "SHA256 abc")
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
        t:eq(srey.hex(h1, #h1):lower(), srey.hex(h2, #h2):lower(), "SHA1 分段 update 一致")
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
             srey.hex(out, #out):lower(), "HMAC-SHA256 RFC 4231 #1")

        -- reset 后再计算
        h:reset()
        h:update("Hi There")
        local out2 = h:final()
        t:eq(srey.hex(out, #out):lower(), srey.hex(out2, #out2):lower(), "HMAC reset round-trip")
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
