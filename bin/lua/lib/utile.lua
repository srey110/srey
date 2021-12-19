local type = type
local table = table
local pairs = pairs
local string = string
local tostring = tostring

local utile = {}

function utile.dump(obj, offset)
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

return utile
