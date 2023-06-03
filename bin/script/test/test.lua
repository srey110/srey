local json = require("cjson")
local msgpack = require("cmsgpack")
local srey = require("lib.srey")
local log = require("lib.log")

printd("program path:%s", srey.path())

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

local rtn = json.encode(ab)
rtn = json.decode(rtn)

rtn = msgpack.pack(ab)
rtn = msgpack.unpack(rtn)
