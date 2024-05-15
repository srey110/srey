local url = require("srey.url")
local utils = require("srey.utils")
local base64 = require("srey.base64")
local crc = require("srey.crc")
local md5 = require("srey.md5")
local sha1 = require("srey.sha1")
local sha256 = require("srey.sha256")
local hmac256 = require("srey.hmac256")

local str = "q=this is 测试"
local enstr = url.encode(str)
assert(str == url.decode(enstr), "url encode decode error.")

str = "https://www.google.com/search?q=this is 测试"
local info = url.parse(str)
print(dump(info))

str = "this is test."
enstr = base64.encode(str)
assert(str == base64.decode(enstr), "base64 encode decode error.")

assert(0x7610 == crc.crc16(str), "crc16 error")
assert(0x3B610CF9 == crc.crc32(str), "crc32 error")

local objmd51 = md5.new()
local objmd52 = md5.new()
objmd51:init()
objmd52:init()
objmd51:update(str)
objmd52:update(str)
enstr = objmd51:final()
local enstr2 = objmd52:final()
assert(utils.hex(enstr2) == utils.hex(enstr), "md5 error")
assert("480FC0D368462326386DA7BB8ED56AD7" == utils.hex(enstr), "md5 error")

local objsha1 = sha1.new()
objsha1:init()
objsha1:update(str)
enstr = objsha1:final()
assert("F1B188A879C1C82D561CB8A064D825FDCBFE4191" == utils.hex(enstr), "sha1 error")

local objsha256 = sha256.new()
objsha256:init()
objsha256:update(str)
enstr = objsha256:final()
assert("FECC75FE2A23D8EAFBA452EE0B8B6B56BECCF52278BF1398AADDEECFE0EA0FCE" == utils.hex(enstr), "sha256 error")

local objmac256 = hmac256.new("keys123")
objmac256:init()
objmac256:update(str)
enstr = objmac256:final()
assert("5C129FDFA6A5338987FB781B07D8CA22BC9B08429BAAFC1760DB006830EDAEEA" == utils.hex(enstr), "hmac256 error")
