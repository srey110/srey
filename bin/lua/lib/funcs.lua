require("lib.define")
local os = os
local math = math
local table = table
local debug = debug
local string = string
local pathsep = _pathsep

function printd(fmt, ...)
    if PRINT_DEBUG then
        local info = debug.getinfo(2)
        local file = string.match(info.source, string.format("^.+%s(.+)$", pathsep))
        local tag = string.format("[%s][%s %d] ",
                                   os.date(FMT_TIME, os.time()), file or "", info.currentline)
        print(string.format(tag..fmt, ...))
    end
end
function enum(tb, bg)
    local tbenum = {}
    local index = bg or 0
    for key, val in pairs(tb) do
        tbenum[val] = index + key - 1
    end
    return tbenum
end
function strempty(str)
    return not str or '' == str
end
function split(str, delimiter)
    if ('' == delimiter) then
        return {str}
    end
    local pos,arr = 0, {}
    for st,sp in function() return string.find(str, delimiter, pos, true) end do
        table.insert(arr, string.sub(str, pos, st - 1))
        pos = sp + 1
    end
    table.insert(arr, string.sub(str, pos))
    return arr
end
function tbsize(tb)
    local cnt = 0
    for _, _ in pairs(tb) do
        cnt = cnt + 1
    end
    return cnt
end
function tbempty(tb)
    if not tb then
        return true
    end
    for _, _ in pairs(tb) do
        return false
    end
    return true
end
function randstr(cnt)
    local char = {"0","1","2","3","4","5","6","7","8","9",
                  "a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z",
                  "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"}
    local rtn = {}
    for i =1, cnt do
        table.insert(rtn, char[math.random(#char)])
    end
    return table.concat(rtn)
end
--dump
function dump(obj, offset)
    local dumpObj
    offset = offset or 0
    local function getIndent(level)
        return string.rep("    ", level)
    end
    local function quoteStr(str)
        str = string.gsub(str, "[%c\\\"]", {
            ["\t"] = "\\t",
            ["\r"] = "\\r",
            ["\n"] = "\\n",
            ["\""] = "\\\"",
            ["\\"] = "\\\\",
        })
        return '"' .. str .. '"'
    end
    local function wrapKey(val)
        if type(val) == "number" then
            return "[" .. val .. "]"
        elseif type(val) == "string" then
            return "[" .. quoteStr(val) .. "]"
        else
            return "[" .. tostring(val) .. "]"
        end
    end
    local function wrapVal(val, level)
        if type(val) == "table" then
            return dumpObj(val, level)
        elseif type(val) == "number" then
            return val
        elseif type(val) == "string" then
            return quoteStr(val)
        else
            return tostring(val)
        end
    end
    local isArray = function(arr)
        local count = 0
        for k, v in pairs(arr) do
            count = count + 1
        end
        for i = 1, count do
            if arr[i] == nil then
                return false
            end
        end
        return true, count
    end
    dumpObj = function(obj, level)
        if type(obj) ~= "table" then
            return wrapVal(obj)
        end
        --level = level + 1
        local tokens = {}
        tokens[#tokens + 1] = "\n"..getIndent(level).."{"
        --tokens[#tokens + 1] = "{"
        local ret, count = isArray(obj)
        if ret then
            for i = 1, count do
                tokens[#tokens + 1] = getIndent(level + 1) .. wrapVal(obj[i], level + 1) .. ","
            end
        else
            for k, v in pairs(obj) do
                tokens[#tokens + 1] = getIndent(level + 1) .. wrapKey(k) .. " = " .. wrapVal(v, level + 1) .. ","
            end
        end
        tokens[#tokens + 1] = getIndent(level) .. "}"
        return table.concat(tokens, "\n")
    end
    return dumpObj(obj, offset)
end

local function setmetatableindex(t, index)
    if nil == t or nil == index then
        assert(false, "nil value")
        return
    end
    local mt = getmetatable(t)
    if not mt then
        mt = {}
    end
    if not mt.__index then
        mt.__index = index
        setmetatable(t, mt)
    elseif mt.__index ~= index then
        setmetatableindex(mt, index)
    end
end
--class
function class(classname, ...)
    local cls = { __cname = classname }
    local supers = { ... }
    for _, super in ipairs(supers) do
        local superType = type(super)
        assert("table" == type(super), string.format("class() - create class \"%s\" with invalid super class type \"%s\"",
                classname, superType))
        cls.__supers = cls.__supers or {}
        cls.__supers[#cls.__supers + 1] = super
        if not cls.super then
             cls.super = super
        end
    end

    cls.__index = cls
    if not cls.__supers or #cls.__supers == 1 then
        setmetatable(cls, { __index = cls.super })
    else
        setmetatable(cls, { __index = function(_, key)
            local supers = cls.__supers
            for i = 1, #supers do
                local super = supers[i]
                if super[key] then
                    return super[key]
                end
            end
        end })
    end

    if not cls.ctor then
        cls.ctor = function()
        end
    end
    cls.new = function(...)
        local instance = {}
        setmetatableindex(instance, cls)
        instance.class = cls
        instance:ctor(...)
        return instance
    end
    return cls
end
