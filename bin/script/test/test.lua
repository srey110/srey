require("lib.funcs")
local json = require("cjson")
local msgpack = require("cmsgpack")
local srey = require("lib.srey")
local log = require("lib.log")
local protoc = require("lib.protoc")
local pb = require("pb")

printd("program path:%s", _propath)

log.ERROR("ERROR")
log.WARN("WARN")
log.INFO("INFO")
log.DEBUG("DEBUG")

assert(protoc:load [[
   message Phone {
      optional string name        = 1;
      optional int64  phonenumber = 2;
   }
   message Person {
      optional string name     = 1;
      optional int32  age      = 2;
      optional string address  = 3;
      repeated Phone  contacts = 4;
   } ]])
-- lua 表数据
local data = {
   name = "ilse",
   age  = 18,
   contacts = {
      { name = "alice", phonenumber = 12312341234 },
      { name = "bob",   phonenumber = 45645674567 }
   }
}
-- 将Lua表编码为二进制数据
local bytes = assert(pb.encode("Person", data))
--printd(pb.tohex(bytes))
-- 再解码回Lua表
local data2 = assert(pb.decode("Person", bytes))
--printd(dump(data2))

local ab = {
    person = {
        {
            name = "Alice",
            id = 10000,
            phone = {
                { number = "123456789" , type = 1 },
                { number = "7654321" , type = 2 },
            }
        },
        {
            name = "Bob",
            id = 20000,
            phone = {
                { number = "01234567890" , type = 3 },
            }
        }
    }
}
--To-be-closed
local tbcmt = {
    __close = function()
        printd(" close to-be-closed var")
    end
}
local function create_tbcv()
    local tbcv = {}
    setmetatable(tbcv, tbcmt)
    return tbcv
end
do
    local tbcv <close> = create_tbcv()
end

printd("randstr %s", randstr(5))
printd("randstr %s", randstr(10))

local rtn = json.encode(ab)
rtn = json.decode(rtn)

rtn = msgpack.pack(ab)
rtn = msgpack.unpack(rtn)
