require("lib.funcs")
local srey = require("lib.srey")
local syn = require("lib.synsl")
local simpel = require("lib.simple")
local cbs = require("lib.cbs")

local function timeout(tmoarg)
    local task2<close> = srey.task_grab(TASK_NAME.TEST2)
    if not task2 then
        if not srey.task_register("test.test2", TASK_NAME.TEST2) then
            printd("task_synregister error.")
        end
    end
    syn.timeout(50, timeout, tmoarg)
end
local function timeout2()
    --printd(dump(srey.worker_load()))
    printd("test1 message qu lens %d coro_pool_size %s co_sess_size %d co_tmo_size %d",
        srey.task_qusize(), srey.coro_pool_size(), syn.coro_cnt())
    syn.timeout(5000, timeout2)
end
local function startup()
    printd("test1 startup.")
    local bg = os.time()
    syn.sleep(1000)
    if 1 ~= os.time() - bg  then
        printd("sleep error.")
    end
    srey.listen("0.0.0.0", 15000, PACK_TYPE.SIMPLE, nil, 1)
    local tmoarg = {
        a = 10,
    }
    syn.timeout(1000, timeout, tmoarg)
    syn.timeout(5000, timeout2)
end
cbs.cb_startup(startup)

local function accept(msg)
    --printd("accept %d", msg.fd)
end
cbs.cb_accept(accept)
local function recv(msg)
    --printd("recv lens %d", msg.size)
    --syn.sleep(10)
    local pack, size = simpel.unpack(msg.data)
    pack, size = simpel.pack(pack, size)
    srey.send(msg.fd, msg.skid, pack, size, false)
end
cbs.cb_recv(recv)
local function sended(msg)
    --printd("sended lens %d", msg.size)
end
cbs.cb_send(sended)
local function closed(msg)
    --printd("closed %d", msg.fd)
end
cbs.cb_close(closed)

local function request(msg)
    if 0 ~= msg.sess and INVALID_TNAME ~= msg.src then
        local test<close> = srey.task_grab(msg.src)
        srey.task_response(test, msg.sess, ERR_OK, msg.data, msg.size, 1)
    end
end
cbs.cb_request(request)

local function closing()
    printd("test1 closing.")
end
cbs.cb_closing(closing)
