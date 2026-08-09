-- 代码注入模块：向当前 task 注入并执行 Lua 源码，将业务状态暴露为 _U。
-- 原理：从 message_dispatch 的 upvalue 树中递归收集所有 table，
--       注入代码在隔离环境中执行，print 输出通过返回值传回调用方。
-- 注意：仅作用于当前 task 的 Lua 状态，不跨 task。
-- _U 的键有两种：限定名 "名字@文件" 恒有，短名仅在该名字全局唯一时才有；重名清单见 _UDUP。
-- 同一个文件里还有同名的，第二个起在限定名后面缀 "#2"、"#3"。

-- 递归收集函数 func 及其子函数的 upvalue 中的 table。
-- 每个表都以 "名字@文件" 的限定名收一份，短名只在全局唯一时才给：
-- 收集范围横跨整棵闭包树，两个模块各有一个 local cache 是常事，只按名字放的话后来者
-- 直接覆盖前者，操作者拿 _U.cache 看到的是哪个模块的完全说不清，据此排障会走偏。
-- 同名冲突时短名撤掉并记入 dup，逼操作者改用限定名。
-- 同一个文件里也会有同名的（各自 do 块、工厂被调了多次），那时限定名同样分辨不开：
-- 挨个缀 "#2"、"#3" 收下，一个都不丢；这种情况短名必然也是歧义的，一并撤掉。
-- _ENV 不收：它挂在几乎每个函数上，收进来只是反复覆盖出一条等同 _G 的噪声条目（沙箱已 __index = _G）
local function _collect(u, dup, func, seen)
    if not func or seen[func] then
        return
    end
    seen[func] = true
    local src = debug.getinfo(func, "S").short_src
    local i = 1
    while true do
        local name, val = debug.getupvalue(func, i)
        if not name then
            break
        end
        if "_ENV" ~= name then
            local t = type(val)
            if "table" == t then
                local qname = name .. "@" .. src
                if nil == u[qname] then
                    u[qname] = val
                elseif u[qname] ~= val then
                    local n = 2
                    local key = qname .. "#" .. n
                    while nil ~= u[key] and u[key] ~= val do
                        n = n + 1
                        key = qname .. "#" .. n
                    end
                    u[key] = val
                    u[name] = nil
                    dup[name] = true
                end
                if not dup[name] then
                    if nil == u[name] then
                        u[name] = val
                    elseif u[name] ~= val then
                        u[name] = nil
                        dup[name] = true
                    end
                end
            elseif "function" == t then
                _collect(u, dup, val, seen)
            end
        end
        i = i + 1
    end
end

---在当前 task 的沙箱中编译并执行 Lua 源码；print 输出通过返回值传回。
---注入代码可见两个变量：
---  _U    从 message_dispatch upvalue 树收集到的 table。每个都有 "名字@文件" 的限定名，
---        短名只在全局唯一时才有——同名不同表时短名为 nil，须改用限定名（如 _U["cache@lib/mysql.lua"]）；
---        同一文件里还有同名的，第二个起缀 "#2"、"#3"（如 _U["cache@lib/mysql.lua#2"]）
---  _UDUP 上述被撤掉短名的名字列表；为空表示没有重名
---@param source string Lua 源码字符串
---@param filename string? 调试显示名；nil 时默认为 "=(inject)"
---@return boolean ok 执行成功 true，编译/运行出错 false
---@return string[] output print 输出行列表；失败时末尾附加错误信息
local function _inject(source, filename)
    local u, dup, seen = {}, {}, {}
    if message_dispatch then
        _collect(u, dup, message_dispatch, seen)
    end
    local dupnames = {}
    for name in pairs(dup) do
        dupnames[#dupnames + 1] = name
    end
    table.sort(dupnames)
    local output = {}
    local env = setmetatable({
        print = function(...)
            local n = select("#", ...)
            local t = { ... }
            for i = 1, n do
                t[i] = tostring(t[i])
            end
            output[#output + 1] = table.concat(t, "\t", 1, n)
        end,
        _U = u,
        _UDUP = dupnames,
    }, { __index = _G })
    local chunk, err = load(source, filename or "=(inject)", "t", env)
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
