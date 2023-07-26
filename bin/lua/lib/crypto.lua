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
--[[
描述:md5_ctx 初始化
参数:
    md5 :md5_ctx or nil
    size :integer
返回:
    md5_ctx
--]]
function cypt.md5_init(md5)
    return crypto.md5_init(md5)
end
function cypt.md5_update(md5, data, size)
    assert(md5 and data, "invalid argument.")
    crypto.md5_update(md5, data, size)
end
function cypt.md5_final(md5)
    assert(md5, "invalid argument.")
    return crypto.md5_final(md5)
end

function cypt.sha1_init(sha1)
    return crypto.sha1_init(sha1)
end
function cypt.sha1_update(sha1, data, size)
    assert(sha1 and data, "invalid argument.")
    crypto.sha1_update(sha1, data, size)
end
function cypt.sha1_final(sha1)
    assert(sha1, "invalid argument.")
    return crypto.sha1_final(sha1)
end

function cypt.sha256_init(sha256)
    return crypto.sha256_init(sha256)
end
function cypt.sha256_update(sha256, data, size)
    assert(sha256 and data, "invalid argument.")
    crypto.sha256_update(sha256, data, size)
end
function cypt.sha256_final(sha256)
    assert(sha256, "invalid argument.")
    return crypto.sha256_final(sha256)
end
function cypt.url_encode(data, size)
    assert(data, "invalid argument.")
    return crypto.url_encode(data, size)
end

return cypt
