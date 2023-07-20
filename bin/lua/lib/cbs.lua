require("lib.define")
local cb_funcs = {}
local cbs = {}

function cbs.cb_startup(func)
    cb_funcs[MSG_TYPE.STARTUP] = func
end
function cbs.cb_closing(func)
    cb_funcs[MSG_TYPE.CLOSING] = func
end
function cbs.cb_accept(func)
    cb_funcs[MSG_TYPE.ACCEPT] = func
end
function cbs.cb_connect(func)
    cb_funcs[MSG_TYPE.CONNECT] = func
end
function cbs.cb_handshake(func)
    cb_funcs[MSG_TYPE.HANDSHAKED] = func
end
function cbs.cb_recv(func)
    cb_funcs[MSG_TYPE.RECV] = func
end
function cbs.cb_send(func)
    cb_funcs[MSG_TYPE.SEND] = func
end
function cbs.cb_close(func)
    cb_funcs[MSG_TYPE.CLOSE] = func
end
function cbs.cb_recvfrom(func)
    cb_funcs[MSG_TYPE.RECVFROM] = func
end
function cbs.cb_request(func)
    cb_funcs[MSG_TYPE.REQUEST] = func
end
function cbs.cb(mtype)
    return cb_funcs[mtype]
end

return cbs
