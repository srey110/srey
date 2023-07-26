local crypto = require("srey.crypto")
local cypt={}

--[[
描述:base64编码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function cypt.b64_encode(data, size)
    assert(data, "invalid argument.")
    return crypto.b64_encode(data, size)
end
--[[
描述:base64解码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function cypt.b64_decode(data, size)
    assert(data, "invalid argument.")
    return crypto.b64_decode(data, size)
end
--[[
描述:crc16
参数:
    data :string or userdata
    size :integer
返回:
    integer
--]]
function cypt.crc16(data, size)
    assert(data, "invalid argument.")
    return crypto.crc16(data, size)
end
--[[
描述:crc32
参数:
    data :string or userdata
    size :integer
返回:
    integer
--]]
function cypt.crc32(data, size)
    assert(data, "invalid argument.")
    return crypto.crc32(data, size)
end

function cypt.md5_update(data, size)
    assert(data, "invalid argument.")
    crypto.md5_update(data, size)
end
function cypt.md5_final()
    return crypto.md5_final()
end

function cypt.sha1_update(data, size)
    assert(data, "invalid argument.")
    crypto.sha1_update(data, size)
end
function cypt.sha1_final()
    return crypto.sha1_final()
end

function cypt.sha256_update(data, size)
    assert(data, "invalid argument.")
    crypto.sha256_update(data, size)
end
function cypt.sha256_final()
    assert("invalid argument.")
    return crypto.sha256_final()
end
function cypt.url_encode(data, size)
    assert(data, "invalid argument.")
    return crypto.url_encode(data, size)
end

return cypt
