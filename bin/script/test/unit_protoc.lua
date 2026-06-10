-- protoc.lua + pb 模块测试：从 .proto 字符串加载到 pb，encode/decode round-trip。
-- 覆盖：scalar 类型 / repeated / nested message / enum / bytes / proto3 默认值

local srey   = require("lib.srey")
local runner = require("test.runner")
local protoc = require("lib.protoc")
local pb     = require("pb")

local _SCHEMA = [[
syntax = "proto3";
package srey.test;

enum Color {
    UNKNOWN = 0;
    RED     = 1;
    GREEN   = 2;
    BLUE    = 3;
}

message Tag {
    string  key   = 1;
    string  value = 2;
}

message User {
    int32             id     = 1;
    string            name   = 2;
    double            score  = 3;
    bool              active = 4;
    repeated string   tags   = 5;
    Color             color  = 6;
    bytes             blob   = 7;
    repeated Tag      props  = 8;
}
]]

srey.startup(function()
runner.run("protoc", function(t)
    -- 加载 schema
    local parser = protoc.new()
    local ok = pcall(function() parser:load(_SCHEMA, "test.proto") end)
    t:check(ok, "protoc.Parser:load .proto schema")

    -- pb.types() 列出已注册的类型，应包含我们刚加载的
    do
        local found = { user = false, tag = false, color = false }
        for tn in pb.types() do
            if tn == ".srey.test.User"  then found.user  = true end
            if tn == ".srey.test.Tag"   then found.tag   = true end
            if tn == ".srey.test.Color" then found.color = true end
        end
        t:check(found.user,  "pb.types lists User")
        t:check(found.tag,   "pb.types lists Tag")
        t:check(found.color, "pb.types lists Color")
    end

    -- scalar + repeated + bytes 全字段 round-trip
    do
        local payload = {
            id     = 42,
            name   = "alice",
            score  = 99.5,
            active = true,
            tags   = { "x", "y", "z" },
            color  = "RED",
            blob   = "\x00\x01\x02\xff",
            props  = {
                { key = "lang", value = "lua" },
                { key = "env",  value = "test" },
            },
        }
        local bytes = pb.encode("srey.test.User", payload)
        t:check(bytes ~= nil and #bytes > 0, "encode returns bytes")

        local back = pb.decode("srey.test.User", bytes)
        t:check(back ~= nil, "decode returns table")
        t:eq(42,        back.id,     "decode int32")
        t:eq("alice",   back.name,   "decode string")
        t:check(back.score > 99.49 and back.score < 99.51, "decode double")
        t:eq(true,      back.active, "decode bool")
        t:eq(3,         #back.tags,  "decode repeated string count")
        t:eq("x",       back.tags[1],"decode repeated string [1]")
        t:eq("z",       back.tags[3],"decode repeated string [3]")
        t:eq("RED",     back.color,  "decode enum")
        t:eq("\x00\x01\x02\xff", back.blob, "decode bytes (二进制安全含 \\0/\\xff)")
        t:eq(2,         #back.props, "decode repeated nested msg")
        t:eq("lang",    back.props[1].key,   "decode nested [1].key")
        t:eq("lua",     back.props[1].value, "decode nested [1].value")
        t:eq("env",     back.props[2].key,   "decode nested [2].key")
    end

    -- proto3 默认值：未设置字段 decode 出来等于 zero value
    do
        local empty = pb.encode("srey.test.User", {})
        t:check(empty ~= nil, "encode empty table")
        local back = pb.decode("srey.test.User", empty)
        t:eq(0,       back.id,     "default int32 = 0")
        t:eq("",      back.name,   "default string = ''")
        t:eq(0,       back.score,  "default double = 0")
        t:eq(false,   back.active, "default bool = false")
        t:eq("UNKNOWN", back.color,"default enum = first value (UNKNOWN)")
        t:eq("",      back.blob,   "default bytes = ''")
        -- repeated 字段未填 decode 为空 table 或 nil（取决于 pb 实现，二者都接受）
        local tags_ok = back.tags == nil or (type(back.tags) == "table" and #back.tags == 0)
        t:check(tags_ok, "default repeated tags empty/nil")
    end

    -- 内层 Tag 单独 encode/decode
    do
        local bytes = pb.encode("srey.test.Tag", { key = "k1", value = "v1" })
        local back = pb.decode("srey.test.Tag", bytes)
        t:eq("k1", back.key,   "Tag.key round-trip")
        t:eq("v1", back.value, "Tag.value round-trip")
    end

    -- proto2 默认值：验证 protoc 解析科学计数法 [default] 指数与 \x/\NNN 字符串转义
    -- (proto3 无自定义默认值,这两处修复只能用 proto2 覆盖)
    do
        local p2 = [[
syntax = "proto2";
package srey.test2;
message Defaults {
    optional double sci = 1 [default = 1.5e10];
    optional string esc = 2 [default = "\x41\101"];
}
]]
        local ok2 = pcall(function() protoc.new():load(p2, "test2.proto") end)
        t:check(ok2, "protoc 加载 proto2 schema")
        -- pb decode 不回填 proto2 scalar default,改用 pb.defaults 读 type 的 [default] 表
        local d = pb.defaults("srey.test2.Defaults")
        t:check(d ~= nil and d.sci ~= nil and d.sci > 1.49e10 and d.sci < 1.51e10,
            "proto2 科学计数法默认值保留指数 sci=1.5e10(修前正则 ^ 误入捕获组丢指数=1.5)")
        t:eq("Ae", d and d.esc,
            "proto2 字符串转义默认值 esc=Ae(\\x41=A \\101=e;修前 string.byte 误产生 '6''1')")
    end

    -- 同一 schema 重复加载允许（pcall 不抛即可——pb.load 内部对已注册类型有 dedup 策略）
    -- 但不同 schema 同名类型不允许（这里跳过，避免污染全局 state）
end)
end)
