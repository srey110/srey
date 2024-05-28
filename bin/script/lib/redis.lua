local srey = require("lib.srey")
local srey_redis = require("srey.redis")
local table = table
local redis = {}

function redis.connect(ip, port, sslname, psw, netev)
    local fd, skid = srey.connect(PACK_TYPE.REDIS, sslname, ip, port, netev)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    if str_nullorempty(psw) then
        return fd, skid
    end
    local auth = redis.pack("AUTH", psw)
    local rtn, _ = srey.syn_send(fd, skid, auth, #auth, 1)
    if not rtn or "OK" ~= redis.unpack(rtn) then
        srey.close(fd, skid)
        return INVALID_SOCK
    end
    return fd, skid
end
function redis.pack(...)
    local param = {...}
    local req = {}
    table.insert(req, string.format("*%d\r\n", #param))
    for _, value in ipairs(param) do
        value = tostring(value)
        table.insert(req, string.format("$%d\r\n%s\r\n", #value, value))
    end
    return table.concat(req)
end
function redis.value(pk)
    return srey_redis.value(pk)
end
function redis.next(pk)
    return srey_redis.next(pk)
end

local function _is_map(val)
    return "map" == val.resp_type or "attr" == val.resp_type
end
local function _is_attr(val)
    return "attr" == val.resp_type
end
local function _is_agg(val)
    if "table" ~= type(val) then
        return false
    end
    return "array" == val.resp_type or "set" == val.resp_type or "push" == val.resp_type or
           "map" == val.resp_type or "attr" == val.resp_type
end
local function _update_mark(mark)
    local mk
    local attr
    while true do
        mk = mark[#mark]
        if not mk then
            break
        end
        mk.nelem = mk.nelem - 1
        if mk.nelem > 0 then
            break
        end
        table.remove(mark, #mark)
        attr = _is_attr(mk.agg)
        mk.agg.resp_nelem = nil
        mk.agg.resp_type = nil
        if attr then
            break
        end
    end
end
local function _add_mark(mark, val)
    local nelem
    if _is_map(val) then
        nelem = val.resp_nelem * 2
    else
        nelem = val.resp_nelem
    end
    table.insert(mark,
    {
        status = 0,
        nelem = nelem,
        key = nil,
        agg = val
    })
end
local function _single_node(pk)
    local val = redis.value(pk)
    if _is_agg(val) then
        if -1 == val.resp_nelem then
            return nil
        elseif 0 == val.resp_nelem then
            return {}
        else
            WARN("resp message error.")
           return nil
        end
    end
    return val
end
local function _fisrt_nodes(mark, pk)
    local val = redis.value(pk)
    if not _is_agg(val) then
        WARN("resp message error.")
        return nil
    end
    if val.resp_nelem > 0  then
        _add_mark(mark, val)
        if _is_attr(val) then
            return {val}
        else
            return val
        end
    elseif 0 == val.resp_nelem then
        if _is_attr(val) then
            return {{}}
        end
    else
        if _is_attr(val) then
            return {}
        end
    end
    WARN("resp message error.")
    return nil
end
function redis.unpack(pk)
    --单一节点
    if not redis.next(pk) then
        return _single_node(pk)
    end
    --多节点，第一个应该为aggregate data
    local mark = {}
    local rtn = _fisrt_nodes(mark, pk)
    if not rtn then
        return nil
    end
    local val, parent
    pk = redis.next(pk)
    while pk do
        parent = mark[#mark]
        val = redis.value(pk)
        if not parent then--无父节点
            if _is_agg(val) then--aggregate
                if val.resp_nelem > 0 then
                    table.insert(rtn, val)
                    _add_mark(mark, val)
                elseif 0 == val.resp_nelem then
                    table.insert(rtn, {})
                end
            else--非aggregate
                if nil ~= val then
                    table.insert(rtn, val)
                end
            end
        else--有父节点
            if _is_agg(val) then--aggregate
                if _is_map(parent.agg) and not _is_attr(val) then--父节点 为map attr 当前节点不为attr
                    if 0 == parent.status then--key
                        parent.status = 1
                        if val.resp_nelem > 0 then
                            parent.key = val
                        elseif 0 == val.resp_nelem then
                            parent.key = {}
                        else
                            parent.key = nil
                        end
                    else--val
                        parent.status = 0
                        if nil ~= parent.key then
                            if val.resp_nelem > 0 then
                                parent.agg[parent.key] = val
                            elseif 0 == val.resp_nelem  then
                                parent.agg[parent.key] = {}
                            end
                        end
                    end
                else--父节点 非map attr 或 当前节点为attr
                    if val.resp_nelem > 0 then
                        table.insert(parent.agg, val)
                    elseif 0 == val.resp_nelem then
                        table.insert(parent.agg, {})
                    end
                end
                if val.resp_nelem > 0 then
                    _add_mark(mark, val)
                else
                    if not _is_attr(val) then
                        _update_mark(mark)
                    end
                end
            else--非aggregate
                if _is_map(parent.agg) then--父节点 为map attr
                    if 0 == parent.status then--key
                        parent.status = 1
                        parent.key = val
                    else--val
                        parent.status = 0
                        if nil ~= parent.key and nil ~= val then
                            parent.agg[parent.key] = val
                        end
                    end
                else--父节点 非map attr
                    if nil ~= val then
                        table.insert(parent.agg, val)
                    end
                end
                _update_mark(mark)
            end
        end
        pk = redis.next(pk)
    end
    return rtn
end

return redis
