local salgo = require("srey.algo")
local algo={}

--[[
描述:base64编码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function algo.b64_encode(data, size)
    assert(data, "invalid argument.")
    return salgo.b64_encode(data, size)
end
--[[
描述:base64解码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function algo.b64_decode(data, size)
    assert(data, "invalid argument.")
    return salgo.b64_decode(data, size)
end
--[[
描述:crc16
参数:
    data :string or userdata
    size :integer
返回:
    integer
--]]
function algo.crc16(data, size)
    assert(data, "invalid argument.")
    return salgo.crc16(data, size)
end
--[[
描述:crc32
参数:
    data :string or userdata
    size :integer
返回:
    integer
--]]
function algo.crc32(data, size)
    assert(data, "invalid argument.")
    return salgo.crc32(data, size)
end
--[[
描述:md5 new
    函数 :init() 
         :update(data, size); 
         :final() string
返回:
    md5_ctx
--]]
function algo.md5_new()
    return md5_new()
end
--[[
描述:sha1 new
    函数 :init() 
         :update(data, size);
         :final() string
返回:
    sha1_ctx
--]]
function algo.sha1_new()
    return sha1_new()
end
--[[
描述:sha256 new
    函数 :init() 
         :update(data, size);
         :final() string
返回:
    sha256_ctx
--]]
function algo.sha256_new()
    return sha256_new()
end
--[[
描述:hmac sha256 new
    函数 :init() 
         :update(data, size);
         :final() string
参数:
    key :string
返回:
    hmac_sha256
--]]
function algo.hmac_sha256_new(key)
    return hmac_sha256_new(key)
end
--[[
描述:hash ring new
    函数 :mode(HASH_RING_MODE);
         :print()
         :add(name:string, namelen:integer)
         :remove(name:string, namelen:integer)
         :get(name:string, namelen:integer) string or nil
         :find(key:string, keylen:integer) string or nil
         :finds(n:integer, key:string, keylen:integer) table or nil
参数:
    replicas :integer
    hsfunc :HASH_RING_FUNC 
返回:
    sha256_ctx
--]]
function algo.hash_ring_new(replicas, hsfunc)
    return hash_ring_new(replicas, hsfunc or HASH_RING_FUNC.SHA1)
end
--[[
描述:url 编码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function algo.url_encode(data, size)
    assert(data, "invalid argument.")
    return salgo.url_encode(data, size)
end
--[[
描述:sha1 base64 编码
参数:
    data :string or userdata
    size :integer
返回:
    string
--]]
function algo.sha1_b64(data, size)
    assert(data, "invalid argument.")
    return salgo.sha1_b64(data, size)
end

return algo
