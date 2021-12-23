local srey = require("lib.srey")
local json = require("cjson")
local utile = require("lib.utile")
local msgpack = require("cmsgpack")
srey.INFO("1111111111111111111")
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

local rtn
local loop = 100000
local tm1 = srey.ms()
for i = 0, loop do
    rtn = json.encode(ab) --string
    rtn = json.decode(rtn)
end
print("json:" .. (srey.ms() - tm1))

tm1 = srey.ms()
for i = 0, loop do
    rtn = msgpack.pack(ab) --string
    rtn = msgpack.unpack(rtn)
end
print("msgpack:" .. (srey.ms() - tm1))

srey.newtask("test2.lua", "test2", 3)
srey.newtask("test1.lua", "test1", 3)
