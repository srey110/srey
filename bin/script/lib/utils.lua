-- 工具函数库（业务侧使用）。
-- 提供：主机类型判断、调试打印、字符串分割、空值检测、随机字符串、
--       表结构转储（dump）、面向对象 class 基础设施。

local os = os
local math = math
local table = table
local debug = debug
local string = string
local utils = require("srey.utils")
local pathsep = _pathsep   -- 由 C 层注入的路径分隔符（Linux："/"，Windows："\"）
local PRINT_DEBUG = true   -- 控制 printd 是否输出；可在运行时置 false 关闭调试打印

---判断 host 字符串的地址类型
---@param host string 主机地址（IPv4 / IPv6 / 域名）
---@return string kind "ipv4" / "ipv6" / "hostname"
function host_type(host)
	if host:match("^%d+%.%d+%.%d+%.%d+$") then
		return "ipv4"
	end
	if host:find(":") then
		return "ipv6"
	end
	return "hostname"
end

---调试打印：附加 [时:分:秒][文件名 行号] 前缀；PRINT_DEBUG=false 时静默
---@param fmt string 格式串
---@param ... any 格式参数
function printd(fmt, ...)
    if not PRINT_DEBUG then
        return
    end
    local info = debug.getinfo(2)
    local file = string.match(info.source, string.format("^.+%s(.+)$", pathsep))
    local tag = string.format("[%s][%s %d] ", os.date("%H:%M:%S", os.time()), file or "", info.currentline)
    print(string.format(tag..fmt, ...))
end

---按 delimiter 分割字符串
---@param str string 原始字符串
---@param delimiter string 分隔符；为空串时不拆分直接返回 {str}
---@return string[] parts 子串数组
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

---判断字符串是否为 nil 或空串
---@param str string|nil 待检测的字符串
---@return boolean is_empty nil 或 "" 时返回 true
function str_nullorempty(str)
    return not str or '' == str
end

---统计表中键值对的数量（含非连续整数键；# 运算符仅对序列有效，此函数适用于任意表）
---@param tb table<any,any> 任意表
---@return integer count 键值对数量
function table_size(tb)
    local cnt = 0
    for _, _ in pairs(tb) do
        cnt = cnt + 1
    end
    return cnt
end

---判断表是否为 nil 或空表（无任何键值对）
---@param tb table<any,any>|nil 待检测的表
---@return boolean is_empty nil 或无任何键值对时返回 true
function table_nullorempty(tb)
    if not tb then
        return true
    end
    for _, _ in pairs(tb) do
        return false
    end
    return true
end

-- 随机字符串字符集（0-9、a-z、A-Z），共 62 个字符。
local _RANDSTR_CHARS = {
    "0","1","2","3","4","5","6","7","8","9",
    "a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z",
    "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"
}
local _RANDSTR_LEN = #_RANDSTR_CHARS

---生成指定长度的随机字母数字字符串（字符集 0-9 / a-z / A-Z），由 srey.utils.csprng_rand 提供 CSPRNG
---@param cnt integer 字符串长度
---@return string? str 随机字符串；CSPRNG 失败（熵未就绪等）返回 nil
function randstr(cnt)
    local bytes = utils.csprng_rand(cnt)
    if not bytes then
        return nil
    end
    local rtn = {}
    for i = 1, cnt do
        rtn[i] = _RANDSTR_CHARS[(string.byte(bytes, i) % _RANDSTR_LEN) + 1]
    end
    return table.concat(rtn)
end

---将任意 Lua 值格式化为可读字符串（类似 Python repr）；表递归展开，数组与普通表分别格式化
---@param obj any 任意 Lua 值
---@param offset integer? 初始缩进层级，默认 0
---@return string text 可读字符串
function dump(obj, offset)
    local dumpObj
    local visited = {}
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
    -- 判断表是否为纯序列（键为连续正整数 1..n，无空洞）。
    -- 返回 true, count 或 false。
    local isArray = function(arr)
        local count = 0
        local max   = 0
        for k, _ in pairs(arr) do
            if type(k) ~= "number" or k < 1 or k ~= math.floor(k) then
                return false
            end
            count = count + 1
            if k > max then
                 max = k
            end
        end
        -- count 个不同正整数均 ≤ max，且 count == max，由鸽巢原理可知恰好是 {1..max}，无空洞
        if count ~= max then
            return false
        end
        return true, count
    end
    dumpObj = function(obj, level)
        if type(obj) ~= "table" then
            return wrapVal(obj)
        end
        if visited[obj] then
            return "<circular>"
        end
        visited[obj] = true
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
        visited[obj] = nil
        return table.concat(tokens, "\n")
    end
    return dumpObj(obj, offset)
end

---递归设置元表的 __index 字段；__index 被占用且不等于新值时沿元表链向上追加，形成链式继承
---@param t table<any,any> 目标表
---@param index table<any,any> 要设置的 __index 表
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

---轻量级 OOP class 实现，支持多继承；调用 cls.new(...) 创建实例并触发 ctor
---@param classname string 类名（存入 __cname，仅用于调试）
---@param ... table<any,any> 零个或多个父类；多继承时按顺序查找 key
---@return table<any,any> cls 类表（含 new / ctor / super / __cname / __supers）
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
        -- 单继承：直接将父类设为元表 __index
        setmetatable(cls, { __index = cls.super })
    else
        -- 多继承：按 __supers 顺序依次查找
        setmetatable(cls, { __index = function(_, key)
            local supers = cls.__supers
            for i = 1, #supers do
                local super = supers[i]
                local v = super[key]
                if v ~= nil then return v end
            end
        end })
    end

    if not cls.ctor then
        cls.ctor = function()
        end
    end
    -- new：创建类实例，将元表 __index 指向 cls，然后调用 ctor 初始化。
    cls.new = function(...)
        local instance = {}
        setmetatableindex(instance, cls)
        instance.class = cls
        instance:ctor(...)
        return instance
    end
    return cls
end
