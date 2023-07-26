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
    if not data then
        return nil
    end
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
    if not data then
        return nil
    end
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
    if not data then
        return nil
    end
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
    if not data then
        return nil
    end
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
    if not md5 or not data then
        return
    end
    crypto.md5_update(md5, data, size)
end
function cypt.md5_final(md5)
    return crypto.md5_final(md5)
end

function cypt.sha1_init(sha1)
    return crypto.sha1_init(sha1)
end
function cypt.sha1_update(sha1, data, size)
    if not sha1 or not data then
        return
    end
    crypto.sha1_update(sha1, data, size)
end
function cypt.sha1_final(sha1)
    return crypto.sha1_final(sha1)
end

function cypt.sha256_init(sha256)
    return crypto.sha256_init(sha256)
end
function cypt.sha256_update(sha256, data, size)
    if not sha256 or not data then
        return
    end
    crypto.sha256_update(sha256, data, size)
end
function cypt.sha256_final(sha256)
    return crypto.sha256_final(sha256)
end
function cypt.url_encode(data, size)
    if not data then
        return nil
    end
    return crypto.url_encode(data, size)
end

return cypt
