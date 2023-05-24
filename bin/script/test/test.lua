local json = require("cjson")
local utile = require("lib.utile")
local msgpack = require("cmsgpack")
local srey = require("lib.srey")
local log = require("lib.log")

print("program path:" .. srey.path())

log.FATAL("FATAL")
log.ERROR("ERROR")
log.WARN("WARN")
log.INFO("INFO")
log.DEBUG("DEBUG")

local ab = {
    person = {
        {
            name = "Alice",
            id = 10000,
            phone = {
                { number = "123456789" , type = 1 },
                { number = "87654321" , type = 2 },
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

local rtn = json.encode(ab)
--print("json.encode:")
--print(rtn)
rtn = json.decode(rtn)
--print("json.decode:")
--print(utile.dump(rtn))

rtn = msgpack.pack(ab) --string
rtn = msgpack.unpack(rtn)
--print("msgpack.unpack:")
--print(utile.dump(rtn))

