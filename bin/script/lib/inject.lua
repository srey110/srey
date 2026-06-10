-- 代码注入模块：向当前 task 注入并执行 Lua 源码，将业务状态暴露为 _U。
-- 原理：从 message_dispatch 的 upvalue 树中递归收集所有 table，
--       注入代码在隔离环境中执行，print 输出通过返回值传回调用方。
-- 注意：仅作用于当前 task 的 Lua 状态，不跨 task。

-- 递归收集函数 func 及其子函数的 upvalue 中的 table
local function _collect(u, func, seen)
    if not func or seen[func] then
        return
    end
    seen[func] = true
    local i = 1
    while true do
        local name, val = debug.getupvalue(func, i)
        if not name then
            break
        end
        local t = type(val)
        if t == "table" then
            u[name] = val
        elseif t == "function" then
            _collect(u, val, seen)
        end
        i = i + 1
    end
end

---在当前 task 的沙箱中编译并执行 Lua 源码；
---print 输出通过返回值传回，_U 持有从 message_dispatch upvalue 树收集到的所有 table
---@param source string Lua 源码字符串
---@param filename string? 调试显示名；nil 时默认为 "=(inject)"
---@return boolean ok 执行成功 true，编译/运行出错 false
---@return string[] output print 输出行列表；失败时末尾附加错误信息
local function _inject(source, filename)
    local u, seen = {}, {}
    if message_dispatch then
        _collect(u, message_dispatch, seen)
    end
    local output = {}
    local env = setmetatable({
        print = function(...)
            local t = { ... }
            for k, v in ipairs(t) do
                t[k] = tostring(v)
            end
            output[#output + 1] = table.concat(t, "\t")
        end,
        _U = u,
    }, { __index = _G })
    local chunk, err = load(source, filename or "=(inject)", "bt", env)
    if not chunk then
        return false, { err }
    end
    local ok, err2 = pcall(chunk)
    if not ok then
        output[#output + 1] = tostring(err2)
        return false, output
    end
    return true, output
end

return _inject
