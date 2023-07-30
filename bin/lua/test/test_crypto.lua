local algo = require("lib.algo")
local core = require("lib.core")

local src = "this is test."

local en = algo.b64_encode(src)
assert("dGhpcyBpcyB0ZXN0Lg==" == en, "b64_encode error")
local de = algo.b64_decode(en)
assert(src == de, "b64_decode error")

assert(0x7610 == algo.crc16(src), " crypto.crc16 error")
assert(0x3B610CF9 == algo.crc32(src), " crypto.crc32 error")

local md51 = algo.md5_new()
local md52 = algo.md5_new()
md51:init()
md51:update(src)
md52:init()
md52:update(src)
en = md51:final(src)
local en2 = md52:final(src)
assert(en == en2, "md5 error")
assert("480FC0D368462326386DA7BB8ED56AD7" == core.tohex(en), "md5 error")

local sha1 = algo.sha1_new()
sha1:init()
sha1:update(src)
en = sha1:final()
assert("F1B188A879C1C82D561CB8A064D825FDCBFE4191" == core.tohex(en), "md5 error")

local sha256 = algo.sha256_new()
sha256:init()
sha256:update(src)
en = sha256:final()
assert("FECC75FE2A23D8EAFBA452EE0B8B6B56BECCF52278BF1398AADDEECFE0EA0FCE" == core.tohex(en), "md5 error")

local ring = algo.hash_ring_new(8, HASH_RING_FUNC.SHA1)
