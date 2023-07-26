local crypto = require("lib.crypto")
local core = require("lib.core")

local src = "this is test."

local en = crypto.b64_encode(src)
assert("dGhpcyBpcyB0ZXN0Lg==" == en, "b64_encode error")
local de = crypto.b64_decode(en)
assert(src == de, "b64_decode error")

assert(0x7610 == crypto.crc16(src), " crypto.crc16 error")
assert(0x3B610CF9 == crypto.crc32(src), " crypto.crc32 error")

local md5 = crypto.md5_init()
crypto.md5_update(md5, src)
en = crypto.md5_final(md5)
assert("480FC0D368462326386DA7BB8ED56AD7" == core.tohex(en), "md5 error")

local sha1 = crypto.sha1_init()
crypto.sha1_update(sha1, src)
en = crypto.sha1_final(sha1)
assert("F1B188A879C1C82D561CB8A064D825FDCBFE4191" == core.tohex(en), "md5 error")

local sha256 = crypto.sha256_init()
crypto.sha256_update(sha256, src)
en = crypto.sha256_final(sha256)
assert("FECC75FE2A23D8EAFBA452EE0B8B6B56BECCF52278BF1398AADDEECFE0EA0FCE" == core.tohex(en), "md5 error")
