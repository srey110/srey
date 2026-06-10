-- BSON 模块封装：将 C 层 bson（构建器）和 bson.iter（迭代器）合并后返回。
-- 使用方：local bson = require("lib.bson")
--          local b = bson.new()          -- 构建器
--          local iter = bson.iter.new(b) -- 迭代器

local cbson      = require("bson")
local cbson_iter = require("bson.iter")

cbson.iter = cbson_iter

cbson.TYPE = {
    EOD        = 0x00,
    DOUBLE     = 0x01,
    UTF8       = 0x02,
    DOCUMENT   = 0x03,
    ARRAY      = 0x04,
    BINARY     = 0x05,
    OID        = 0x07,
    BOOL       = 0x08,
    DATE       = 0x09,
    NULL       = 0x0A,
    REGEX      = 0x0B,
    JSCODE     = 0x0D,
    INT32      = 0x10,
    TIMESTAMP  = 0x11,
    INT64      = 0x12,
    DECIMAL128 = 0x13,
    MAXKEY     = 0x7F,
    MINKEY     = 0xFF,
}
cbson.SUBTYPE = {
    BINARY     = 0x00,
    FUNCTION   = 0x01,
    UUID       = 0x04,
    MD5        = 0x05,
    ENCRYPTED  = 0x06,
    COMPRESSED = 0x07,
    SENSITIVE  = 0x08,
    VECTOR     = 0x09,
    USER       = 0x80,
}

return cbson
